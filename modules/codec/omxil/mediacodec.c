/*****************************************************************************
 * mediacodec.c: Video decoder module using the Android MediaCodec API
 *****************************************************************************
 * Copyright (C) 2012 Martin Storsjo
 *
 * Authors: Martin Storsjo <martin@martin.st>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <jni.h>
#include <stdint.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_aout.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_block_helper.h>
#include <vlc_cpu.h>
#include <vlc_memory.h>
#include <vlc_timestamp_helper.h>
#include <vlc_threads.h>

#include "mediacodec.h"
#include "../../packetizer/h264_nal.h"
#include "../../packetizer/hevc_nal.h"
#include <OMX_Core.h>
#include <OMX_Component.h>
#include "omxil_utils.h"
#include "../../video_output/android/android_window.h"

/* JNI functions to get/set an Android Surface object. */
extern void jni_EventHardwareAccelerationError(); // TODO REMOVE

#define BLOCK_FLAG_CSD (0x01 << BLOCK_FLAG_PRIVATE_SHIFT)

/* Codec Specific Data */
struct csd
{
    uint8_t *p_buf;
    size_t i_size;
};

#define NEWBLOCK_FLAG_RESTART (0x01)
#define NEWBLOCK_FLAG_FLUSH (0x02)
/**
 * Callback called when a new block is processed from DecodeCommon.
 * It returns -1 in case of error, 0 if block should be dropped, 1 otherwise.
 */
typedef int (*dec_on_new_block_cb)(decoder_t *, block_t *, int *);

/**
 * Callback called when decoder is flushing.
 */
typedef void (*dec_on_flush_cb)(decoder_t *);

/**
 * Callback called when DecodeCommon try to get an output buffer (pic or block).
 * It returns -1 in case of error, or the number of output buffer returned.
 */
typedef int (*dec_process_output_cb)(decoder_t *, mc_api_out *, picture_t **, block_t **);

struct decoder_sys_t
{
    mc_api *api;

    /* Codec Specific Data buffer: sent in DecodeCommon after a start or a flush
     * with the BUFFER_FLAG_CODEC_CONFIG flag.*/
    block_t **pp_csd;
    size_t i_csd_count;
    size_t i_csd_send;

    bool b_update_format;
    bool b_has_format;

    int64_t i_preroll_end;
    int     i_quirks;

    /* Specific Audio/Video callbacks */
    dec_on_new_block_cb     pf_on_new_block;
    dec_on_flush_cb         pf_on_flush;
    dec_process_output_cb   pf_process_output;

    vlc_mutex_t     lock;
    vlc_thread_t    out_thread;
    /* Cond used to signal the output thread */
    vlc_cond_t      cond;
    /* Cond used to signal the decoder thread */
    vlc_cond_t      dec_cond;
    /* Set to true by pf_flush to signal the output thread to flush */
    bool            b_flush_out;
    /* If true, the output thread will start to dequeue output pictures */
    bool            b_output_ready;
    /* If true, the first input block was successfully dequeued */
    bool            b_input_dequeued;
    bool            b_error;
    /* TODO: remove. See jni_EventHardwareAccelerationError */
    bool            b_error_signaled;

    union
    {
        struct
        {
            AWindowHandler *p_awh;
            int i_pixel_format, i_stride, i_slice_height, i_width, i_height;
            uint8_t i_nal_length_size;
            size_t i_h264_profile;
            ArchitectureSpecificCopyData ascd;
            /* stores the inflight picture for each output buffer or NULL */
            picture_sys_t** pp_inflight_pictures;
            unsigned int i_inflight_pictures;
            timestamp_fifo_t *timestamp_fifo;
        } video;
        struct {
            date_t i_end_date;
            int i_channels;
            bool b_extract;
            /* Some audio decoders need a valid channel count */
            bool b_need_channels;
            int pi_extraction[AOUT_CHAN_MAX];
        } audio;
    } u;
};

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  OpenDecoderJni(vlc_object_t *);
static int  OpenDecoderNdk(vlc_object_t *);
static void CleanDecoder(decoder_t *);
static void CloseDecoder(vlc_object_t *);

static int Video_OnNewBlock(decoder_t *, block_t *, int *);
static void Video_OnFlush(decoder_t *);
static int Video_ProcessOutput(decoder_t *, mc_api_out *, picture_t **, block_t **);
static picture_t *DecodeVideo(decoder_t *, block_t **);

static int Audio_OnNewBlock(decoder_t *, block_t *, int *);
static void Audio_OnFlush(decoder_t *);
static int Audio_ProcessOutput(decoder_t *, mc_api_out *, picture_t **, block_t **);
static block_t *DecodeAudio(decoder_t *, block_t **);

static void DecodeFlushLocked(decoder_t *);
static void DecodeFlush(decoder_t *);
static void StopMediaCodec(decoder_t *);
static void *OutThread(void *);

static void InvalidateAllPictures(decoder_t *);
static void RemoveInflightPictures(decoder_t *);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define DIRECTRENDERING_TEXT N_("Android direct rendering")
#define DIRECTRENDERING_LONGTEXT N_(\
        "Enable Android direct rendering using opaque buffers.")

#define MEDIACODEC_AUDIO_TEXT "Use MediaCodec for audio decoding"
#define MEDIACODEC_AUDIO_LONGTEXT "Still experimental."

#define CFG_PREFIX "mediacodec-"

vlc_module_begin ()
    set_description( N_("Video decoder using Android MediaCodec via NDK") )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_section( N_("Decoding") , NULL )
    set_capability( "decoder", 0 ) /* Only enabled via commandline arguments */
    add_bool(CFG_PREFIX "dr", true,
             DIRECTRENDERING_TEXT, DIRECTRENDERING_LONGTEXT, true)
    add_bool(CFG_PREFIX "audio", false,
             MEDIACODEC_AUDIO_TEXT, MEDIACODEC_AUDIO_LONGTEXT, true)
    set_callbacks( OpenDecoderNdk, CloseDecoder )
    add_shortcut( "mediacodec_ndk" )
    add_submodule ()
        set_description( N_("Video decoder using Android MediaCodec via JNI") )
        set_capability( "decoder", 0 )
        set_callbacks( OpenDecoderJni, CloseDecoder )
        add_shortcut( "mediacodec_jni" )
vlc_module_end ()

static void CSDFree(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if (p_sys->pp_csd)
    {
        for (unsigned int i = 0; i < p_sys->i_csd_count; ++i)
            block_Release(p_sys->pp_csd[i]);
        free(p_sys->pp_csd);
        p_sys->pp_csd = NULL;
    }
    p_sys->i_csd_count = 0;
}

/* Create the p_sys->p_csd that will be sent from DecodeCommon */
static int CSDDup(decoder_t *p_dec, const struct csd *p_csd, size_t i_count)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    CSDFree(p_dec);

    p_sys->pp_csd = malloc(i_count * sizeof(block_t *));
    if (!p_sys->pp_csd)
        return VLC_ENOMEM;

    for (size_t i = 0; i < i_count; ++i)
    {
        p_sys->pp_csd[i] = block_Alloc(p_csd[i].i_size);
        if (!p_sys->pp_csd[i])
        {
            CSDFree(p_dec);
            return VLC_ENOMEM;
        }
        p_sys->pp_csd[i]->i_flags = BLOCK_FLAG_CSD;
        memcpy(p_sys->pp_csd[i]->p_buffer, p_csd[i].p_buf, p_csd[i].i_size);
        p_sys->i_csd_count++;
    }

    p_sys->i_csd_send = 0;
    return VLC_SUCCESS;
}

static bool CSDCmp(decoder_t *p_dec, struct csd *p_csd, size_t i_csd_count)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if (p_sys->i_csd_count != i_csd_count)
        return false;
    for (size_t i = 0; i < i_csd_count; ++i)
    {
        if (p_sys->pp_csd[i]->i_buffer != p_csd[i].i_size
         || memcmp(p_sys->pp_csd[i]->p_buffer, p_csd[i].p_buf,
                   p_csd[i].i_size) != 0)
            return false;
    }
    return true;
}

/* Fill the p_sys->p_csd struct with H264 Parameter Sets */
static int H264SetCSD(decoder_t *p_dec, void *p_buf, size_t i_size,
                      bool *p_size_changed)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    struct h264_nal_sps sps;
    uint8_t *p_sps_buf = NULL, *p_pps_buf = NULL;
    size_t i_sps_size = 0, i_pps_size = 0;

    /* Check if p_buf contains a valid SPS PPS */
    if (h264_get_spspps(p_buf, i_size, &p_sps_buf, &i_sps_size,
                        &p_pps_buf, &i_pps_size) == 0
     && h264_parse_sps(p_sps_buf, i_sps_size, &sps) == 0
     && sps.i_width && sps.i_height)
    {
        struct csd csd[2];
        int i_csd_count = 0;

        if (i_sps_size)
        {
            csd[i_csd_count].p_buf = p_sps_buf;
            csd[i_csd_count].i_size = i_sps_size;
            i_csd_count++;
        }
        if (i_pps_size)
        {
            csd[i_csd_count].p_buf = p_pps_buf;
            csd[i_csd_count].i_size = i_pps_size;
            i_csd_count++;
        }

        /* Compare the SPS PPS with the old one */
        if (!CSDCmp(p_dec, csd, i_csd_count))
        {
            msg_Warn(p_dec, "New SPS/PPS found, id: %d size: %dx%d sps: %d pps: %d",
                     sps.i_id, sps.i_width, sps.i_height,
                     i_sps_size, i_pps_size);

            /* In most use cases, p_sys->p_csd[0] contains a SPS, and
             * p_sys->p_csd[1] contains a PPS */
            if (CSDDup(p_dec, csd, i_csd_count))
                return VLC_ENOMEM;

            if (p_size_changed)
                *p_size_changed = (sps.i_width != p_sys->u.video.i_width
                                || sps.i_height != p_sys->u.video.i_height);

            p_sys->u.video.i_width = sps.i_width;
            p_sys->u.video.i_height = sps.i_height;
            return VLC_SUCCESS;
        }
    }
    return VLC_EGENERIC;
}

static int ParseVideoExtra(decoder_t *p_dec, uint8_t *p_extra, int i_extra)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if (p_dec->fmt_in.i_codec == VLC_CODEC_H264
     || p_dec->fmt_in.i_codec == VLC_CODEC_HEVC)
    {
        if (p_dec->fmt_in.i_codec == VLC_CODEC_H264)
        {
            if (h264_isavcC(p_extra, i_extra))
            {
                size_t i_size = 0;
                uint8_t *p_buf = h264_avcC_to_AnnexB_NAL(p_extra, i_extra, &i_size,
                                                         &p_sys->u.video.i_nal_length_size);
                if(p_buf)
                {
                    H264SetCSD(p_dec, p_buf, i_size, NULL);
                    free(p_buf);
                }
            }
            else
                H264SetCSD(p_dec, p_extra, i_extra, NULL);
        }
        else  /* FIXME and refactor: CSDDup vs CSDless SetCSD */
        {
            if (hevc_ishvcC(p_extra, i_extra))
            {
                struct csd csd;
                csd.p_buf = hevc_hvcC_to_AnnexB_NAL(p_extra, i_extra, &csd.i_size,
                                                    &p_sys->u.video.i_nal_length_size);
                if(csd.p_buf)
                {
                    CSDDup(p_dec, &csd, 1);
                    free(csd.p_buf);
                }
            }
            /* FIXME: what to do with AnnexB ? */
        }
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * StartMediaCodec: Create the mediacodec instance
 *****************************************************************************/
static int StartMediaCodec(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i_ret = 0;
    union mc_api_args args;

    if (p_dec->fmt_in.i_extra && !p_sys->pp_csd)
    {
        /* Try first to configure specific Video CSD */
        if (p_dec->fmt_in.i_cat == VIDEO_ES)
            i_ret = ParseVideoExtra(p_dec, p_dec->fmt_in.p_extra,
                                    p_dec->fmt_in.i_extra);

        if (i_ret != VLC_SUCCESS)
            return i_ret;

        /* Set default CSD if ParseVideoExtra failed to configure one */
        if (!p_sys->pp_csd)
        {
            struct csd csd;

            csd.p_buf = p_dec->fmt_in.p_extra;
            csd.i_size = p_dec->fmt_in.i_extra;
            CSDDup(p_dec, &csd, 1);
        }
    }

    if (p_dec->fmt_in.i_cat == VIDEO_ES)
    {
        if (!p_sys->u.video.i_width || !p_sys->u.video.i_height)
        {
            msg_Err(p_dec, "invalid size, abort MediaCodec");
            return VLC_EGENERIC;
        }
        args.video.i_width = p_sys->u.video.i_width;
        args.video.i_height = p_sys->u.video.i_height;

        switch (p_dec->fmt_in.video.orientation)
        {
            case ORIENT_ROTATED_90:
                args.video.i_angle = 90;
                break;
            case ORIENT_ROTATED_180:
                args.video.i_angle = 180;
                break;
            case ORIENT_ROTATED_270:
                args.video.i_angle = 270;
                break;
            default:
                args.video.i_angle = 0;
        }

        /* Check again the codec name if h264 profile changed */
        if (p_dec->fmt_in.i_codec == VLC_CODEC_H264
         && !p_sys->u.video.i_h264_profile)
        {
            h264_get_profile_level(&p_dec->fmt_in,
                                   &p_sys->u.video.i_h264_profile, NULL, NULL);
            if (p_sys->u.video.i_h264_profile)
            {
                free(p_sys->api->psz_name);
                p_sys->api->psz_name = MediaCodec_GetName(VLC_OBJECT(p_dec),
                                                          p_sys->api->psz_mime,
                                                          p_sys->u.video.i_h264_profile);
                if (!p_sys->api->psz_name)
                    return VLC_EGENERIC;
            }
        }

        if (!p_sys->u.video.p_awh && var_InheritBool(p_dec, CFG_PREFIX "dr"))
        {
            if ((p_sys->u.video.p_awh = AWindowHandler_new(VLC_OBJECT(p_dec))))
            {
                /* Direct rendering:
                 * The surface must be released by the Vout before calling
                 * start. Request a valid OPAQUE Vout to release any non-OPAQUE
                 * Vout that will release the surface.
                 */
                p_dec->fmt_out.video.i_width = p_sys->u.video.i_width;
                p_dec->fmt_out.video.i_height = p_sys->u.video.i_height;
                p_dec->fmt_out.i_codec = VLC_CODEC_ANDROID_OPAQUE;
                if (decoder_UpdateVideoFormat(p_dec) != 0)
                {
                    msg_Err(p_dec, "Opaque Vout request failed: "
                                   "fallback to non opaque");

                    AWindowHandler_destroy(p_sys->u.video.p_awh);
                    p_sys->u.video.p_awh = NULL;
                }
            }
        }
        args.video.p_awh = p_sys->u.video.p_awh;
    }
    else
    {
        date_Set(&p_sys->u.audio.i_end_date, VLC_TS_INVALID);

        args.audio.i_sample_rate    = p_dec->fmt_in.audio.i_rate;
        args.audio.i_channel_count  = p_dec->p_sys->u.audio.i_channels;
    }

    return p_sys->api->start(p_sys->api, &args);
}

/*****************************************************************************
 * StopMediaCodec: Close the mediacodec instance
 *****************************************************************************/
static void StopMediaCodec(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    /* Remove all pictures that are currently in flight in order
     * to prevent the vout from using destroyed output buffers. */
    if (p_sys->api->b_direct_rendering)
        RemoveInflightPictures(p_dec);

    p_sys->api->stop(p_sys->api);
    if (p_dec->fmt_in.i_cat == VIDEO_ES && p_sys->u.video.p_awh)
        AWindowHandler_releaseSurface(p_sys->u.video.p_awh, AWindow_Video);
}

/*****************************************************************************
 * OpenDecoder: Create the decoder instance
 *****************************************************************************/
static int OpenDecoder(vlc_object_t *p_this, pf_MediaCodecApi_init pf_init)
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys;
    mc_api *api;
    const char *mime = NULL;
    bool b_late_opening = false;

    /* Video or Audio if "mediacodec-audio" bool is true */
    if (p_dec->fmt_in.i_cat != VIDEO_ES && (p_dec->fmt_in.i_cat != AUDIO_ES
     || !var_InheritBool(p_dec, CFG_PREFIX "audio")))
        return VLC_EGENERIC;

    if (p_dec->fmt_in.i_cat == VIDEO_ES)
    {
        if (!p_dec->fmt_in.video.i_width || !p_dec->fmt_in.video.i_height)
        {
            /* We can handle h264 without a valid video size */
            if (p_dec->fmt_in.i_codec != VLC_CODEC_H264)
            {
                msg_Dbg(p_dec, "resolution (%dx%d) not supported",
                        p_dec->fmt_in.video.i_width, p_dec->fmt_in.video.i_height);
                return VLC_EGENERIC;
            }
        }

        switch (p_dec->fmt_in.i_codec) {
        case VLC_CODEC_HEVC: mime = "video/hevc"; break;
        case VLC_CODEC_H264: mime = "video/avc"; break;
        case VLC_CODEC_H263: mime = "video/3gpp"; break;
        case VLC_CODEC_MP4V: mime = "video/mp4v-es"; break;
        case VLC_CODEC_WMV3: mime = "video/x-ms-wmv"; break;
        case VLC_CODEC_VC1:  mime = "video/wvc1"; break;
        case VLC_CODEC_VP8:  mime = "video/x-vnd.on2.vp8"; break;
        case VLC_CODEC_VP9:  mime = "video/x-vnd.on2.vp9"; break;
        /* case VLC_CODEC_MPGV: mime = "video/mpeg2"; break; */
        }
    }
    else
    {
        switch (p_dec->fmt_in.i_codec) {
        case VLC_CODEC_AMR_NB: mime = "audio/3gpp"; break;
        case VLC_CODEC_AMR_WB: mime = "audio/amr-wb"; break;
        case VLC_CODEC_MPGA:
        case VLC_CODEC_MP3:    mime = "audio/mpeg"; break;
        case VLC_CODEC_MP2:    mime = "audio/mpeg-L2"; break;
        case VLC_CODEC_MP4A:   mime = "audio/mp4a-latm"; break;
        case VLC_CODEC_QCELP:  mime = "audio/qcelp"; break;
        case VLC_CODEC_VORBIS: mime = "audio/vorbis"; break;
        case VLC_CODEC_OPUS:   mime = "audio/opus"; break;
        case VLC_CODEC_ALAW:   mime = "audio/g711-alaw"; break;
        case VLC_CODEC_MULAW:  mime = "audio/g711-mlaw"; break;
        case VLC_CODEC_FLAC:   mime = "audio/flac"; break;
        case VLC_CODEC_GSM:    mime = "audio/gsm"; break;
        case VLC_CODEC_A52:    mime = "audio/ac3"; break;
        case VLC_CODEC_EAC3:   mime = "audio/eac3"; break;
        case VLC_CODEC_ALAC:   mime = "audio/alac"; break;
        case VLC_CODEC_DTS:    mime = "audio/vnd.dts"; break;
        /* case VLC_CODEC_: mime = "audio/mpeg-L1"; break; */
        /* case VLC_CODEC_: mime = "audio/aac-adts"; break; */
        }
    }
    if (!mime)
    {
        msg_Dbg(p_dec, "codec %4.4s not supported",
                (char *)&p_dec->fmt_in.i_codec);
        return VLC_EGENERIC;
    }

    api = calloc(1, sizeof(mc_api));
    if (!api)
        return VLC_ENOMEM;
    api->p_obj = p_this;
    api->b_video = p_dec->fmt_in.i_cat == VIDEO_ES;
    if (pf_init(api) != VLC_SUCCESS)
    {
        free(api);
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if ((p_sys = calloc(1, sizeof(*p_sys))) == NULL)
    {
        api->clean(api);
        free(api);
        return VLC_ENOMEM;
    }
    p_sys->api = api;
    p_dec->p_sys = p_sys;

    p_dec->pf_decode_video = DecodeVideo;
    p_dec->pf_decode_audio = DecodeAudio;
    p_dec->pf_flush        = DecodeFlush;

    p_dec->fmt_out.i_cat = p_dec->fmt_in.i_cat;
    p_dec->fmt_out.video = p_dec->fmt_in.video;
    p_dec->fmt_out.audio = p_dec->fmt_in.audio;
    p_sys->api->psz_mime = mime;

    vlc_mutex_init(&p_sys->lock);
    vlc_cond_init(&p_sys->cond);
    vlc_cond_init(&p_sys->dec_cond);

    if (p_dec->fmt_in.i_cat == VIDEO_ES)
    {
        p_sys->pf_on_new_block = Video_OnNewBlock;
        p_sys->pf_on_flush = Video_OnFlush;
        p_sys->pf_process_output = Video_ProcessOutput;
        p_sys->u.video.i_width = p_dec->fmt_in.video.i_width;
        p_sys->u.video.i_height = p_dec->fmt_in.video.i_height;

        p_sys->u.video.timestamp_fifo = timestamp_FifoNew(32);
        if (!p_sys->u.video.timestamp_fifo)
            goto bailout;
        TAB_INIT( p_sys->u.video.i_inflight_pictures,
                  p_sys->u.video.pp_inflight_pictures );

        if (p_dec->fmt_in.i_codec == VLC_CODEC_H264)
            h264_get_profile_level(&p_dec->fmt_in,
                                   &p_sys->u.video.i_h264_profile, NULL, NULL);

        p_sys->api->psz_name = MediaCodec_GetName(VLC_OBJECT(p_dec),
                                                  p_sys->api->psz_mime,
                                                  p_sys->u.video.i_h264_profile);
        if (!p_sys->api->psz_name)
            goto bailout;

        p_sys->i_quirks = OMXCodec_GetQuirks(VIDEO_ES,
                                             p_dec->fmt_in.i_codec,
                                             p_sys->api->psz_name,
                                             strlen(p_sys->api->psz_name));

        if ((p_sys->i_quirks & OMXCODEC_VIDEO_QUIRKS_NEED_SIZE)
         && (!p_sys->u.video.i_width || !p_sys->u.video.i_height))
        {
            msg_Warn(p_dec, "waiting for a valid video size for codec %4.4s",
                     (const char *)&p_dec->fmt_in.i_codec);
            b_late_opening = true;
        }
    }
    else
    {
        p_sys->pf_on_new_block = Audio_OnNewBlock;
        p_sys->pf_on_flush = Audio_OnFlush;
        p_sys->pf_process_output = Audio_ProcessOutput;
        p_sys->u.audio.i_channels = p_dec->fmt_in.audio.i_channels;

        p_sys->api->psz_name = MediaCodec_GetName(VLC_OBJECT(p_dec),
                                                  p_sys->api->psz_mime, 0);
        if (!p_sys->api->psz_name)
            goto bailout;

        p_sys->i_quirks = OMXCodec_GetQuirks(AUDIO_ES,
                                             p_dec->fmt_in.i_codec,
                                             p_sys->api->psz_name,
                                             strlen(p_sys->api->psz_name));
        if ((p_sys->i_quirks & OMXCODEC_AUDIO_QUIRKS_NEED_CHANNELS)
         && !p_sys->u.audio.i_channels)
        {
            msg_Warn(p_dec, "waiting for valid channel count");
            b_late_opening = true;
        }
    }
    if ((p_sys->i_quirks & OMXCODEC_QUIRKS_NEED_CSD)
     && !p_dec->fmt_in.i_extra)
    {
        msg_Warn(p_dec, "waiting for extra data for codec %4.4s",
                 (const char *)&p_dec->fmt_in.i_codec);
        if (p_dec->fmt_in.i_codec == VLC_CODEC_MP4V)
        {
            msg_Warn(p_dec, "late opening with MPEG4 not handled"); /* TODO */
            goto bailout;
        }
        b_late_opening = true;
    }

    if (!b_late_opening && StartMediaCodec(p_dec) != VLC_SUCCESS)
    {
        msg_Err(p_dec, "StartMediaCodec failed");
        goto bailout;
    }

    if (vlc_clone(&p_sys->out_thread, OutThread, p_dec,
                  VLC_THREAD_PRIORITY_LOW))
    {
        msg_Err(p_dec, "vlc_clone failed");
        vlc_mutex_unlock(&p_sys->lock);
        goto bailout;
    }

    return VLC_SUCCESS;

bailout:
    CleanDecoder(p_dec);
    return VLC_EGENERIC;
}

static int OpenDecoderNdk(vlc_object_t *p_this)
{
    return OpenDecoder(p_this, MediaCodecNdk_Init);
}

static int OpenDecoderJni(vlc_object_t *p_this)
{
    return OpenDecoder(p_this, MediaCodecJni_Init);
}

static void AbortDecoderLocked(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if (!p_sys->b_error)
    {
        p_sys->b_error = true;
        vlc_cancel(p_sys->out_thread);
    }
}

static void CleanDecoder(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    vlc_mutex_destroy(&p_sys->lock);
    vlc_cond_destroy(&p_sys->cond);
    vlc_cond_destroy(&p_sys->dec_cond);

    StopMediaCodec(p_dec);

    CSDFree(p_dec);
    p_sys->api->clean(p_sys->api);

    if (p_dec->fmt_in.i_cat == VIDEO_ES)
    {
        ArchitectureSpecificCopyHooksDestroy(p_sys->u.video.i_pixel_format,
                                             &p_sys->u.video.ascd);
        if (p_sys->u.video.timestamp_fifo)
            timestamp_FifoRelease(p_sys->u.video.timestamp_fifo);
        if (p_sys->u.video.p_awh)
            AWindowHandler_destroy(p_sys->u.video.p_awh);
    }
    free(p_sys->api->psz_name);
    free(p_sys->api);
    free(p_sys);
}

/*****************************************************************************
 * CloseDecoder: Close the decoder instance
 *****************************************************************************/
static void CloseDecoder(vlc_object_t *p_this)
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    vlc_mutex_lock(&p_sys->lock);
    /* Unblock output thread waiting in dequeue_out */
    DecodeFlushLocked(p_dec);
    /* Cancel the output thread */
    AbortDecoderLocked(p_dec);
    vlc_mutex_unlock(&p_sys->lock);

    vlc_join(p_sys->out_thread, NULL);

    CleanDecoder(p_dec);
}

/*****************************************************************************
 * vout callbacks
 *****************************************************************************/
static void ReleasePicture(decoder_t *p_dec, unsigned i_index, bool b_render)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_sys->api->release_out(p_sys->api, i_index, b_render);
}

static void InvalidateAllPictures(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    for (unsigned int i = 0; i < p_sys->u.video.i_inflight_pictures; ++i)
        AndroidOpaquePicture_Release(p_sys->u.video.pp_inflight_pictures[i],
                                     false);
}

static int InsertInflightPicture(decoder_t *p_dec, picture_sys_t *p_picsys)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    if (!p_picsys->priv.hw.p_dec)
    {
        p_picsys->priv.hw.p_dec = p_dec;
        p_picsys->priv.hw.pf_release = ReleasePicture;
        TAB_APPEND_CAST((picture_sys_t **),
                        p_sys->u.video.i_inflight_pictures,
                        p_sys->u.video.pp_inflight_pictures,
                        p_picsys);
    } /* else already attached */
    return 0;
}

static void RemoveInflightPictures(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    for (unsigned int i = 0; i < p_sys->u.video.i_inflight_pictures; ++i)
        AndroidOpaquePicture_DetachDecoder(p_sys->u.video.pp_inflight_pictures[i]);
    TAB_CLEAN(p_sys->u.video.i_inflight_pictures,
              p_sys->u.video.pp_inflight_pictures);
}

static int Video_ProcessOutput(decoder_t *p_dec, mc_api_out *p_out,
                               picture_t **pp_out_pic, block_t **pp_out_block)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    (void) pp_out_block;
    assert(pp_out_pic);

    if (p_out->type == MC_OUT_TYPE_BUF)
    {
        picture_t *p_pic = NULL;

        /* Use the aspect ratio provided by the input (ie read from packetizer).
         * Don't check the current value of the aspect ratio in fmt_out, since we
         * want to allow changes in it to propagate. */
        if (p_dec->fmt_in.video.i_sar_num != 0 && p_dec->fmt_in.video.i_sar_den != 0
         && (p_dec->fmt_out.video.i_sar_num != p_dec->fmt_in.video.i_sar_num ||
             p_dec->fmt_out.video.i_sar_den != p_dec->fmt_in.video.i_sar_den))
        {
            p_dec->fmt_out.video.i_sar_num = p_dec->fmt_in.video.i_sar_num;
            p_dec->fmt_out.video.i_sar_den = p_dec->fmt_in.video.i_sar_den;
            p_sys->b_update_format = true;
        }

        if (p_sys->b_update_format)
        {
            p_sys->b_update_format = false;
            if (decoder_UpdateVideoFormat(p_dec) != 0)
            {
                msg_Err(p_dec, "decoder_UpdateVideoFormat failed");
                p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false);
                return -1;
            }
        }

        /* If the oldest input block had no PTS, the timestamp of
         * the frame returned by MediaCodec might be wrong so we
         * overwrite it with the corresponding dts. Call FifoGet
         * first in order to avoid a gap if buffers are released
         * due to an invalid format or a preroll */
        int64_t forced_ts = timestamp_FifoGet(p_sys->u.video.timestamp_fifo);

        if (!p_sys->b_has_format) {
            msg_Warn(p_dec, "Buffers returned before output format is set, dropping frame");
            return p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false);
        }

        if (p_out->u.buf.i_ts <= p_sys->i_preroll_end)
            return p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false);

        p_pic = decoder_NewPicture(p_dec);
        if (!p_pic) {
            msg_Warn(p_dec, "NewPicture failed");
            return p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false);
        }

        if (forced_ts == VLC_TS_INVALID)
            p_pic->date = p_out->u.buf.i_ts;
        else
            p_pic->date = forced_ts;

        if (p_sys->api->b_direct_rendering)
        {
            p_pic->p_sys->priv.hw.i_index = p_out->u.buf.i_index;
            InsertInflightPicture(p_dec, p_pic->p_sys);
        } else {
            unsigned int chroma_div;
            GetVlcChromaSizes(p_dec->fmt_out.i_codec,
                              p_dec->fmt_out.video.i_width,
                              p_dec->fmt_out.video.i_height,
                              NULL, NULL, &chroma_div);
            CopyOmxPicture(p_sys->u.video.i_pixel_format, p_pic,
                           p_sys->u.video.i_slice_height, p_sys->u.video.i_stride,
                           (uint8_t *)p_out->u.buf.p_ptr, chroma_div,
                           &p_sys->u.video.ascd);

            if (p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false))
            {
                picture_Release(p_pic);
                return -1;
            }
        }
        assert(!(*pp_out_pic));
        *pp_out_pic = p_pic;
        return 1;
    } else {
        assert(p_out->type == MC_OUT_TYPE_CONF);
        p_sys->u.video.i_pixel_format = p_out->u.conf.video.pixel_format;
        ArchitectureSpecificCopyHooksDestroy(p_sys->u.video.i_pixel_format,
                                             &p_sys->u.video.ascd);

        const char *name = "unknown";
        if (p_sys->api->b_direct_rendering)
            p_dec->fmt_out.i_codec = VLC_CODEC_ANDROID_OPAQUE;
        else
        {
            if (!GetVlcChromaFormat(p_sys->u.video.i_pixel_format,
                                    &p_dec->fmt_out.i_codec, &name)) {
                msg_Err(p_dec, "color-format not recognized");
                return -1;
            }
        }

        msg_Err(p_dec, "output: %d %s, %dx%d stride %d %d, crop %d %d %d %d",
                p_sys->u.video.i_pixel_format, name, p_out->u.conf.video.width, p_out->u.conf.video.height,
                p_out->u.conf.video.stride, p_out->u.conf.video.slice_height,
                p_out->u.conf.video.crop_left, p_out->u.conf.video.crop_top,
                p_out->u.conf.video.crop_right, p_out->u.conf.video.crop_bottom);

        p_dec->fmt_out.video.i_width = p_out->u.conf.video.crop_right + 1 - p_out->u.conf.video.crop_left;
        p_dec->fmt_out.video.i_height = p_out->u.conf.video.crop_bottom + 1 - p_out->u.conf.video.crop_top;
        if (p_dec->fmt_out.video.i_width <= 1
            || p_dec->fmt_out.video.i_height <= 1) {
            p_dec->fmt_out.video.i_width = p_out->u.conf.video.width;
            p_dec->fmt_out.video.i_height = p_out->u.conf.video.height;
        }
        p_dec->fmt_out.video.i_visible_width = p_dec->fmt_out.video.i_width;
        p_dec->fmt_out.video.i_visible_height = p_dec->fmt_out.video.i_height;

        p_sys->u.video.i_stride = p_out->u.conf.video.stride;
        p_sys->u.video.i_slice_height = p_out->u.conf.video.slice_height;
        if (p_sys->u.video.i_stride <= 0)
            p_sys->u.video.i_stride = p_out->u.conf.video.width;
        if (p_sys->u.video.i_slice_height <= 0)
            p_sys->u.video.i_slice_height = p_out->u.conf.video.height;

        ArchitectureSpecificCopyHooks(p_dec, p_out->u.conf.video.pixel_format,
                                      p_out->u.conf.video.slice_height,
                                      p_sys->u.video.i_stride, &p_sys->u.video.ascd);
        if (p_sys->u.video.i_pixel_format == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar)
            p_sys->u.video.i_slice_height -= p_out->u.conf.video.crop_top/2;
        if ((p_sys->i_quirks & OMXCODEC_VIDEO_QUIRKS_IGNORE_PADDING))
        {
            p_sys->u.video.i_slice_height = 0;
            p_sys->u.video.i_stride = p_dec->fmt_out.video.i_width;
        }
        p_sys->b_update_format = true;
        p_sys->b_has_format = true;
        return 0;
    }
}

/* samples will be in the following order: FL FR FC LFE BL BR BC SL SR */
uint32_t pi_audio_order_src[] =
{
    AOUT_CHAN_LEFT, AOUT_CHAN_RIGHT, AOUT_CHAN_CENTER, AOUT_CHAN_LFE,
    AOUT_CHAN_REARLEFT, AOUT_CHAN_REARRIGHT, AOUT_CHAN_REARCENTER,
    AOUT_CHAN_MIDDLELEFT, AOUT_CHAN_MIDDLERIGHT,
};

static int Audio_ProcessOutput(decoder_t *p_dec, mc_api_out *p_out,
                               picture_t **pp_out_pic, block_t **pp_out_block)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    (void) pp_out_pic;
    assert(pp_out_block);

    if (p_out->type == MC_OUT_TYPE_BUF)
    {
        block_t *p_block = NULL;
        if (!p_sys->b_has_format) {
            msg_Warn(p_dec, "Buffers returned before output format is set, dropping frame");
            return p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false);
        }

        p_block = block_Alloc(p_out->u.buf.i_size);
        if (!p_block)
            return -1;
        p_block->i_nb_samples = p_out->u.buf.i_size
                              / p_dec->fmt_out.audio.i_bytes_per_frame;

        if (p_sys->u.audio.b_extract)
        {
            aout_ChannelExtract(p_block->p_buffer,
                                p_dec->fmt_out.audio.i_channels,
                                p_out->u.buf.p_ptr, p_sys->u.audio.i_channels,
                                p_block->i_nb_samples, p_sys->u.audio.pi_extraction,
                                p_dec->fmt_out.audio.i_bitspersample);
        }
        else
            memcpy(p_block->p_buffer, p_out->u.buf.p_ptr, p_out->u.buf.i_size);

        if (p_out->u.buf.i_ts != 0 && p_out->u.buf.i_ts != date_Get(&p_sys->u.audio.i_end_date))
            date_Set(&p_sys->u.audio.i_end_date, p_out->u.buf.i_ts);

        p_block->i_pts = date_Get(&p_sys->u.audio.i_end_date);
        p_block->i_length = date_Increment(&p_sys->u.audio.i_end_date,
                                           p_block->i_nb_samples)
                          - p_block->i_pts;

        if (p_sys->api->release_out(p_sys->api, p_out->u.buf.i_index, false))
        {
            block_Release(p_block);
            return -1;
        }
        *pp_out_block = p_block;
        return 1;
    } else {
        uint32_t i_layout_dst;
        int      i_channels_dst;

        assert(p_out->type == MC_OUT_TYPE_CONF);

        if (p_out->u.conf.audio.channel_count <= 0
         || p_out->u.conf.audio.channel_count > 8
         || p_out->u.conf.audio.sample_rate <= 0)
        {
            msg_Warn( p_dec, "invalid audio properties channels count %d, sample rate %d",
                      p_out->u.conf.audio.channel_count,
                      p_out->u.conf.audio.sample_rate);
            return -1;
        }

        msg_Err(p_dec, "output: channel_count: %d, channel_mask: 0x%X, rate: %d",
                p_out->u.conf.audio.channel_count, p_out->u.conf.audio.channel_mask,
                p_out->u.conf.audio.sample_rate);

        p_dec->fmt_out.i_codec = VLC_CODEC_S16N;
        p_dec->fmt_out.audio.i_format = p_dec->fmt_out.i_codec;

        p_dec->fmt_out.audio.i_rate = p_out->u.conf.audio.sample_rate;
        date_Init(&p_sys->u.audio.i_end_date, p_out->u.conf.audio.sample_rate, 1 );

        p_sys->u.audio.i_channels = p_out->u.conf.audio.channel_count;
        p_sys->u.audio.b_extract =
            aout_CheckChannelExtraction(p_sys->u.audio.pi_extraction,
                                        &i_layout_dst, &i_channels_dst,
                                        NULL, pi_audio_order_src,
                                        p_sys->u.audio.i_channels);

        if (p_sys->u.audio.b_extract)
            msg_Warn(p_dec, "need channel extraction: %d -> %d",
                     p_sys->u.audio.i_channels, i_channels_dst);

        p_dec->fmt_out.audio.i_original_channels =
        p_dec->fmt_out.audio.i_physical_channels = i_layout_dst;
        aout_FormatPrepare(&p_dec->fmt_out.audio);

        if (decoder_UpdateAudioFormat(p_dec))
            return -1;

        p_sys->b_has_format = true;
        return 0;
    }
}

static void H264ProcessBlock(decoder_t *p_dec, block_t *p_block,
                             bool *p_csd_changed, bool *p_size_changed)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    assert(p_dec->fmt_in.i_codec == VLC_CODEC_H264 && p_block);

    if (p_sys->u.video.i_nal_length_size)
    {
        h264_AVC_to_AnnexB(p_block->p_buffer, p_block->i_buffer,
                               p_sys->u.video.i_nal_length_size);
    } else if (H264SetCSD(p_dec, p_block->p_buffer, p_block->i_buffer,
                          p_size_changed) == VLC_SUCCESS)
    {
        *p_csd_changed = true;
    }
}

static void HEVCProcessBlock(decoder_t *p_dec, block_t *p_block,
                             bool *p_csd_changed, bool *p_size_changed)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    assert(p_dec->fmt_in.i_codec == VLC_CODEC_HEVC && p_block);

    if (p_sys->u.video.i_nal_length_size)
    {
        h264_AVC_to_AnnexB(p_block->p_buffer, p_block->i_buffer,
                               p_sys->u.video.i_nal_length_size);
    }

    /* TODO */
    VLC_UNUSED(p_csd_changed);
    VLC_UNUSED(p_size_changed);
}

static void DecodeFlushLocked(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    bool b_had_input = p_sys->b_input_dequeued;

    p_sys->b_input_dequeued = false;
    p_sys->b_flush_out = true;
    p_sys->i_preroll_end = 0;
    p_sys->b_output_ready = false;
    /* Resend CODEC_CONFIG buffer after a flush */
    p_sys->i_csd_send = 0;

    p_sys->pf_on_flush(p_dec);

    if (b_had_input && p_sys->api->flush(p_sys->api) != VLC_SUCCESS)
    {
        AbortDecoderLocked(p_dec);
        return;
    }

    vlc_cond_broadcast(&p_sys->cond);

    while (!p_sys->b_error && p_sys->b_flush_out)
        vlc_cond_wait(&p_sys->dec_cond, &p_sys->lock);
}

static void DecodeFlush(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    vlc_mutex_lock(&p_sys->lock);
    DecodeFlushLocked(p_dec);
    vlc_mutex_unlock(&p_sys->lock);
}

static void *OutThread(void *data)
{
    decoder_t *p_dec = data;
    decoder_sys_t *p_sys = p_dec->p_sys;

    vlc_mutex_lock(&p_sys->lock);
    mutex_cleanup_push(&p_sys->lock);
    for (;;)
    {
        int i_index;

        /* Wait for output ready */
        while (!p_sys->b_flush_out && !p_sys->b_output_ready)
            vlc_cond_wait(&p_sys->cond, &p_sys->lock);

        if (p_sys->b_flush_out)
        {
            /* Acknowledge flushed state */
            p_sys->b_flush_out = false;
            vlc_cond_broadcast(&p_sys->dec_cond);
            continue;
        }

        int canc = vlc_savecancel();

        vlc_mutex_unlock(&p_sys->lock);

        /* Wait for an output buffer. This function returns when a new output
         * is available or if output is flushed. */
        i_index = p_sys->api->dequeue_out(p_sys->api, -1);

        vlc_mutex_lock(&p_sys->lock);

        /* Ignore dequeue_out errors caused by flush */
        if (p_sys->b_flush_out)
        {
            /* If i_index >= 0, Release it. There is no way to know if i_index
             * is owned by us, so don't check the error. */
            if (i_index >= 0)
                p_sys->api->release_out(p_sys->api, i_index, false);

            /* Parse output format/buffers even when we are flushing */
            if (i_index != MC_API_INFO_OUTPUT_FORMAT_CHANGED
             && i_index != MC_API_INFO_OUTPUT_BUFFERS_CHANGED)
            {
                vlc_restorecancel(canc);
                continue;
            }
        }

        /* Process output returned by dequeue_out */
        if (i_index >= 0 || i_index == MC_API_INFO_OUTPUT_FORMAT_CHANGED
         || i_index == MC_API_INFO_OUTPUT_BUFFERS_CHANGED)
        {
            struct mc_api_out out;
            int i_ret = p_sys->api->get_out(p_sys->api, i_index, &out);

            if (i_ret == 1)
            {
                picture_t *p_pic = NULL;
                block_t *p_block = NULL;

                if (p_sys->pf_process_output(p_dec, &out, &p_pic,
                                             &p_block) == -1)
                {
                    msg_Err(p_dec, "pf_process_output failed");
                    vlc_restorecancel(canc);
                    break;
                }
                if (p_pic)
                    decoder_QueueVideo(p_dec, p_pic);
                else if (p_block)
                    decoder_QueueAudio(p_dec, p_block);
            } else if (i_ret != 0)
            {
                msg_Err(p_dec, "get_out failed");
                vlc_restorecancel(canc);
                break;
            }
        }
        else
        {
            msg_Err(p_dec, "dequeue_out failed");
            vlc_restorecancel(canc);
            break;
        }
        vlc_restorecancel(canc);
    }
    msg_Warn(p_dec, "OutThread stopped");

    /* Signal DecoderFlush that the output thread aborted */
    p_sys->b_error = true;
    vlc_cond_signal(&p_sys->dec_cond);

    vlc_cleanup_pop();
    vlc_mutex_unlock(&p_sys->lock);

    return NULL;
}

static block_t *GetNextBlock(decoder_sys_t *p_sys, block_t *p_block)
{
    if (p_sys->i_csd_send < p_sys->i_csd_count)
        return p_sys->pp_csd[p_sys->i_csd_send++];
    else
        return p_block;
}

/**
 * DecodeCommon called from DecodeVideo or DecodeAudio.
 * It returns -1 in case of error, 0 otherwise.
 */
static int DecodeCommon(decoder_t *p_dec, block_t **pp_block)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i_flags = 0;
    int i_ret;
    bool b_dequeue_timeout = false;
    block_t *p_block;

    if (!pp_block || !*pp_block)
        return 0;

    vlc_mutex_lock(&p_sys->lock);

    if (p_sys->b_error)
        goto end;

    p_block = *pp_block;

    if (p_block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED))
    {
        DecodeFlushLocked(p_dec);
        if (p_sys->b_error)
            goto end;
        if (p_block->i_flags & BLOCK_FLAG_CORRUPTED)
            goto end;
    }

    /* Parse input block */
    if ((i_ret = p_sys->pf_on_new_block(p_dec, p_block, &i_flags)) == 1)
    {
        if (i_flags & (NEWBLOCK_FLAG_FLUSH|NEWBLOCK_FLAG_RESTART))
        {
            msg_Warn(p_dec, "Flushing from DecodeCommon");

            /* Flush before restart to unblock OutThread */
            DecodeFlushLocked(p_dec);
            if (p_sys->b_error)
                goto end;

            if (i_flags & NEWBLOCK_FLAG_RESTART)
            {
                msg_Warn(p_dec, "Restarting from DecodeCommon");
                StopMediaCodec(p_dec);
                if (StartMediaCodec(p_dec) != VLC_SUCCESS)
                {
                    msg_Err(p_dec, "StartMediaCodec failed");
                    AbortDecoderLocked(p_dec);
                    goto end;
                }
            }
        }
    }
    else
    {
        if (i_ret != 0)
        {
            AbortDecoderLocked(p_dec);
            msg_Err(p_dec, "pf_on_new_block failed");
        }
        goto end;
    }

    /* Abort if MediaCodec is not yet started */
    if (!p_sys->api->b_started)
        goto end;

    /* Queue CSD blocks and input blocks */
    while ((p_block = GetNextBlock(p_sys, *pp_block)))
    {
        int i_index;

        vlc_mutex_unlock(&p_sys->lock);
        /* Wait for an input buffer. This function returns when a new input
         * buffer is available or after 1sec of timeout. */
        i_index = p_sys->api->dequeue_in(p_sys->api,
                                         p_sys->api->b_direct_rendering ?
                                         INT64_C(1000000) : -1);
        vlc_mutex_lock(&p_sys->lock);

        if (p_sys->b_error)
            goto end;

        if (i_index >= 0)
        {
            bool b_config = p_block && (p_block->i_flags & BLOCK_FLAG_CSD);
            mtime_t i_ts = 0;
            p_sys->b_input_dequeued = true;

            if (!b_config)
            {
                i_ts = p_block->i_pts;
                if (!i_ts && p_block->i_dts)
                    i_ts = p_block->i_dts;
            }

            if (p_sys->api->queue_in(p_sys->api, i_index,
                                     p_block->p_buffer, p_block->i_buffer, i_ts,
                                     b_config) == 0)
            {
                if (!b_config)
                {
                    if (p_block->i_flags & BLOCK_FLAG_PREROLL )
                        p_sys->i_preroll_end = i_ts;

                    /* One input buffer is queued, signal OutThread that will
                     * fetch output buffers */
                    p_sys->b_output_ready = true;
                    vlc_cond_broadcast(&p_sys->cond);

                    assert(p_block == *pp_block),
                    block_Release(p_block);
                    *pp_block = NULL;
                }
                b_dequeue_timeout = false;
            } else
            {
                msg_Err(p_dec, "queue_in failed");
                AbortDecoderLocked(p_dec);
                goto end;
            }
        }
        else if (i_index == MC_API_INFO_TRYAGAIN)
        {
            /* HACK: When direct rendering is enabled, there is a possible
             * deadlock between the Decoder and the Vout. It happens when the
             * Vout is paused and when the Decoder is flushing. In that case,
             * the Vout won't release any output buffers, therefore MediaCodec
             * won't dequeue any input buffers. To work around this issue,
             * release all output buffers if DecodeCommon is waiting more than
             * 1sec for a new input buffer. */
            if (!b_dequeue_timeout)
            {
                msg_Warn(p_dec, "Decoder stuck: invalidate all buffers");
                InvalidateAllPictures(p_dec);
                b_dequeue_timeout = true;
                continue;
            }
            else
            {
                msg_Err(p_dec, "dequeue_in timeout: no input available for 2secs");
                AbortDecoderLocked(p_dec);
                goto end;
            }
        }
        else
        {
            msg_Err(p_dec, "dequeue_in failed");
            AbortDecoderLocked(p_dec);
            goto end;
        }
    }

end:
    if (pp_block && *pp_block)
    {
        block_Release(*pp_block);
        *pp_block = NULL;
    }
    if (p_sys->b_error)
    {
        if (!p_sys->b_error_signaled) {
            /* Signal the error to the Java.
             * TODO: remove this when there is a decoder fallback */
            jni_EventHardwareAccelerationError();
            p_sys->b_error_signaled = true;
            vlc_cond_broadcast(&p_sys->cond);
        }
        vlc_mutex_unlock(&p_sys->lock);
        return -1;
    }
    else
    {
        vlc_mutex_unlock(&p_sys->lock);
        return 0;
    }
}

static int Video_OnNewBlock(decoder_t *p_dec, block_t *p_block, int *p_flags)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    bool b_csd_changed = false, b_size_changed = false;

    if (p_block->i_flags & BLOCK_FLAG_INTERLACED_MASK
        && !p_sys->api->b_support_interlaced)
        return -1;

    if (p_dec->fmt_in.i_codec == VLC_CODEC_H264)
        H264ProcessBlock(p_dec, p_block, &b_csd_changed, &b_size_changed);
    else if (p_dec->fmt_in.i_codec == VLC_CODEC_HEVC)
        HEVCProcessBlock(p_dec, p_block, &b_csd_changed, &b_size_changed);

    if (b_csd_changed)
    {
        if (b_size_changed || !p_sys->api->b_started)
        {
            if (p_sys->api->b_started)
                msg_Err(p_dec, "SPS/PPS changed during playback and "
                        "video size are different. Restart it !");
            *p_flags |= NEWBLOCK_FLAG_RESTART;
        } else
        {
            msg_Err(p_dec, "SPS/PPS changed during playback. Flush it");
            *p_flags |= NEWBLOCK_FLAG_FLUSH;
        }
    }

    if (!p_sys->api->b_started)
    {
        *p_flags |= NEWBLOCK_FLAG_RESTART;

        /* Don't start if we don't have any csd */
        if ((p_sys->i_quirks & OMXCODEC_QUIRKS_NEED_CSD)
         && !p_dec->fmt_in.i_extra && !p_sys->pp_csd)
            *p_flags &= ~NEWBLOCK_FLAG_RESTART;

        /* Don't start if we don't have a valid video size */
        if ((p_sys->i_quirks & OMXCODEC_VIDEO_QUIRKS_NEED_SIZE)
         && (!p_sys->u.video.i_width || !p_sys->u.video.i_height))
            *p_flags &= ~NEWBLOCK_FLAG_RESTART;
    }

    timestamp_FifoPut(p_sys->u.video.timestamp_fifo,
                      p_block->i_pts ? VLC_TS_INVALID : p_block->i_dts);

    return 1;
}

static void Video_OnFlush(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    timestamp_FifoEmpty(p_sys->u.video.timestamp_fifo);
    /* Invalidate all pictures that are currently in flight
     * since flushing make all previous indices returned by
     * MediaCodec invalid. */
    if (p_sys->api->b_direct_rendering)
        InvalidateAllPictures(p_dec);
}

static picture_t *DecodeVideo(decoder_t *p_dec, block_t **pp_block)
{
    DecodeCommon(p_dec, pp_block);
    return NULL;
}

static int Audio_OnNewBlock(decoder_t *p_dec, block_t *p_block, int *p_flags)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    /* We've just started the stream, wait for the first PTS. */
    if (!date_Get(&p_sys->u.audio.i_end_date))
    {
        if (p_block->i_pts <= VLC_TS_INVALID)
            return 0;
        date_Set(&p_sys->u.audio.i_end_date, p_block->i_pts);
    }

    /* try delayed opening if there is a new extra data */
    if (!p_sys->api->b_started)
    {
        p_dec->p_sys->u.audio.i_channels = p_dec->fmt_in.audio.i_channels;

        *p_flags |= NEWBLOCK_FLAG_RESTART;

        /* Don't start if we don't have any csd */
        if ((p_sys->i_quirks & OMXCODEC_QUIRKS_NEED_CSD)
         && !p_dec->fmt_in.i_extra)
            *p_flags &= ~NEWBLOCK_FLAG_RESTART;

        /* Don't start if we don't have a valid channels count */
        if ((p_sys->i_quirks & OMXCODEC_AUDIO_QUIRKS_NEED_CHANNELS)
         && !p_dec->p_sys->u.audio.i_channels)
            *p_flags &= ~NEWBLOCK_FLAG_RESTART;
    }
    return 1;
}

static void Audio_OnFlush(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    date_Set(&p_sys->u.audio.i_end_date, VLC_TS_INVALID);
}

static block_t *DecodeAudio(decoder_t *p_dec, block_t **pp_block)
{
    DecodeCommon(p_dec, pp_block);
    return NULL;
}
