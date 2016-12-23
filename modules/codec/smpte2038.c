/*****************************************************************************
 * smpte2038.c: SMPTE 2038-2008 Decoder
 *****************************************************************************
 * Copyright (C) 2016 Kernel Labs Inc.
 *
 * Authors:  Devin Heitmueller <dheitmueller@kernellabs.com>
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
 * Operational Notes:
 *
 * This decoder behaves a bit differently than other subpicture decoders you
 * might see in the codecs tree.  The most obvious difference is that we
 * receive the entire elementary stream (including the MPEG TS headers). 
 * This is to accommodate certain malformed TS streams seen from hardware
 * encoders which don't obey the typical rules related to the PUEI bit and
 * alignment of the PES packets relative to the payload field start.  Also,
 * unlike as specified in the 13818-1 spec, a single MPEG packet with the
 * PEUI bit set can contain multiple PES packets.
 *
 * To accommodate these deviations from the spec, instead of hacking the TS
 * demux to deal with the alignment problems we pass off the entire ES to
 * the decoder and invoke the Kernel Labs demux to do PES packetization.  The
 * need for this will hopefully go away as the TS demux in VLC continues to
 * mature, at which point we can go back to the Decode() method simply
 * receiving a single complete PES packet.
 *
 * The other big challenge is we have to support a subpicture composed of
 * data derived from multiple PES packets, since each 2038 PES packet will
 * only carry a single VANC line worth of data.  The PES packets are required
 * to contains a PTS (and all PES packets containing lines in the same frame
 * are supposed to have the exact same PTS).  However, at on least one popular
 * hardware encoder we've found the PTS values don't properly correspond to
 * the video stream.  Hence we can rely on the PTS to correlate all the
 * VANC lines which make up a single frame, but we have to ignore the PTS from
 * a timing perspective and rely on the PCR.
 *
 * Because at any given point we don't know if we've yet received all the
 * lines corresponding to a given frame, we don't know when it's safe to
 * finally construct the completed subpicture.  Hence the implementation will
 * create an empty subpicture whenever the decoder receives a PES where the
 * PTS has changed, insert the actual data onto a queue, and then defer
 * construction of the subpicture content until the subpicture updater is
 * invoked during the display phase.  At that phase we can dequeue all packets
 * with the corresponding PTS and be confident that there won't be any more
 * arriving at the decoder for the video frame we're about to display.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>

#include <libklvanc/vanc.h>
#include <libklvanc/klbitstream_readwriter.h>
#include "pes_extractor.h"

struct subpicture_updater_sys_t {
    struct decoder_t *p_dec;
    mtime_t i_pts;
};

static int SubpictureTextValidateSmpte2038(subpicture_t *subpic,
                                           bool has_src_changed,
                                           const video_format_t *fmt_src,
                                           bool has_dst_changed,
                                           const video_format_t *fmt_dst,
                                           mtime_t ts)
{
    VLC_UNUSED(subpic); VLC_UNUSED(fmt_src); VLC_UNUSED(fmt_dst); VLC_UNUSED(ts);

    if (!has_src_changed && !has_dst_changed)
        return VLC_SUCCESS;

    return VLC_EGENERIC;
}

#if 0
static void hexdump(unsigned char *buf, unsigned int len, int bytesPerRow /* Typically 16 */)
{
	for (unsigned int i = 0; i < len; i++)
		printf("%02x%s", buf[i], ((i + 1) % bytesPerRow) ? " " : "\n");
	printf("\n");
}
#endif

struct decoder_sys_t
{
    subpicture_t *subpic;
    struct pes_extractor_s *pe;
    block_fifo_t *fifo;
    mtime_t i_last_pts; /* PTS from last PES packet (for calculating PTS skew if needed) */
    mtime_t i_demux_pts; /* Just so p_block PTS is available to pes_cb */
    mtime_t i_pts_skew; /* For cases where PTS for 2038 stream is *way* out of sync with video */
};

static void SubpictureTextUpdateSmpte2038(subpicture_t *subpic,
                                          const video_format_t *fmt_src,
                                          const video_format_t *fmt_dst,
                                          mtime_t ts)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
    block_fifo_t *fifo = sys->p_dec->p_sys->fifo;
    VLC_UNUSED(fmt_src); VLC_UNUSED(ts);
    block_t *p_block;

    if (fmt_dst->i_sar_num <= 0 || fmt_dst->i_sar_den <= 0)
        return;

    subpic->i_original_picture_width  = fmt_dst->i_width * fmt_dst->i_sar_num / fmt_dst->i_sar_den;
    subpic->i_original_picture_height = fmt_dst->i_height;

    /* Pop all PES packets which have the appropriate timestamp and
       assemble the subpicture content */
    while (block_FifoSize(fifo) > 0) {
        p_block = block_FifoShow(fifo);
        if (p_block->i_pts > sys->i_pts) {
            /* We've hit blocks which aren't ready to present, so bail out */
            break;
        }
        p_block = block_FifoGet(fifo);

#ifdef BROKEN_WITH_CURRENT_VLC_DEMUX
        if (p_block->i_pts < sys->i_pts) {
            /* We've hit blocks which are too old, so throw them away and move on */
            fprintf(stderr, "Pblock too old.  Discarding...\n");
            continue;
        }
#endif

        struct smpte2038_anc_data_packet_s *pkt = 0;
        smpte2038_parse_pes_packet(p_block->p_buffer, p_block->i_buffer, &pkt);
        block_Release(p_block);
        if (pkt == NULL) {
            fprintf(stderr, "%s failed to decode PES packet\n", __func__);
            continue;
        }
    
        for (int i = 0; i < pkt->lineCount; i++) {
            struct smpte2038_anc_data_line_s *l = &pkt->lines[i];
            uint16_t *vancWords = NULL;
            uint16_t vancWordCount;

            if (smpte2038_convert_line_to_words(l, &vancWords, &vancWordCount) < 0)
                break;

#if 0
            fprintf(stderr, "LineEntry[%d] DID=0x%02x SDID=0x%02x (line %d): ", i, l->DID, l->SDID, l->line_number);
            for (int j = 0; j < vancWordCount; j++)
                fprintf(stderr, "%03x ", vancWords[j]);
            fprintf(stderr, "\n");
#endif

            /* Create a VLC subpicture region with the VANC line */
            video_format_t fmt;
            video_format_Init(&fmt, VLC_CODEC_VANC);
            fmt.i_sar_num = 1;
            fmt.i_sar_den = 1;
            fmt.i_width = fmt.i_visible_width = vancWordCount * 2;
            fmt.i_height = fmt.i_visible_height = 1;

            subpicture_region_t *r = subpicture_region_New(&fmt);
            if (!r) {
                free(vancWords);
                break;
            }

            r->i_align = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
            r->i_x = 0;
            r->i_y = l->line_number;
            memcpy(r->p_picture->Y_PIXELS, vancWords, vancWordCount * 2);
            r->p_next = subpic->p_region;
            subpic->p_region = r;

            free(vancWords); /* Free the allocated resource */
        }
        smpte2038_anc_data_packet_free(pkt);
    }
}

static void SubpictureTextDestroySmpte2038(subpicture_t *subpic)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
    free(sys);
}

static inline subpicture_t *decoder_NewSubpictureSmpte2038(decoder_t *decoder)
{
    subpicture_updater_sys_t *sys = calloc(1, sizeof(*sys));
    subpicture_updater_t updater = {
        .pf_validate = SubpictureTextValidateSmpte2038,
        .pf_update   = SubpictureTextUpdateSmpte2038,
        .pf_destroy  = SubpictureTextDestroySmpte2038,
        .p_sys       = sys,
    };

    subpicture_t *subpic = decoder_NewSubpicture(decoder, &updater);
    if (!subpic)
        free(sys);
    else
        sys->p_dec = decoder;

    return subpic;
}

static pes_extractor_callback pes_cb(void *cb_context, uint8_t *buf, int byteCount)
{
    struct decoder_t *p_dec = cb_context;
    struct decoder_sys_t *p_sys = p_dec->p_sys;
    struct smpte2038_anc_data_packet_s *pkt = 0;
    subpicture_t  *p_spu = NULL;
    
    smpte2038_parse_pes_packet(buf, byteCount, &pkt);
    if (pkt == NULL) {
        fprintf(stderr, "%s failed to decode PES packet\n", __func__);
        return 0;
    }

    pkt->PTS += p_sys->i_pts_skew;

    /* Note: the checking for subpic == NULL is to work around a condition where
       an MPEG packet might contain multiple PES packets with different PTS values.
       Because VLC decoders cannot return more than one subpictures, we have to continue
       insert the lines into the current subpicture.  Once the VLC demux is modified to
       call the decoder once per PES, this limitation can be removed */
    if (pkt->PTS != p_sys->i_last_pts && p_sys->subpic == NULL) {
        /* If the PTS has changed, we need to create a new empty subpicture */
        p_spu = decoder_NewSubpictureSmpte2038( p_dec );
        if (p_spu == NULL) {
            fprintf(stderr, "Failed to allocate subpicture 2038\n");
            smpte2038_anc_data_packet_free(pkt);
            return 0;
        }
        
        if (p_sys->i_pts_skew == 0) {
            p_sys->i_pts_skew = p_sys->i_demux_pts - pkt->PTS;
        }

        p_spu->i_start = pkt->PTS * 100 / 9;
        p_spu->i_stop = p_spu->i_start + (CLOCK_FREQ / 30);
#if 0
        p_spu->i_stop = p_spu->i_start + CLOCK_FREQ * 10;
#endif   

        /* Also store the real PTS as a property of the subpicture.  We cannot
           rely on subpicture->i_start because it gets recomputed against a
           master clock before calling update, and we want to ensure we assign
           the lines with the correct video frame */
        p_spu->updater.p_sys->i_pts = pkt->PTS;

        p_spu->b_ephemer  = false;
        p_spu->b_absolute = true;

        p_sys->subpic = p_spu;
        p_sys->i_last_pts = pkt->PTS;
    }

    /* Stick the PES onto a queue to be interpreted at display time */
    block_t *p_block = block_Alloc(byteCount);
    if (p_block == NULL) {
        smpte2038_anc_data_packet_free(pkt);
        return 0;
    }

    p_block->i_pts = pkt->PTS;
    memcpy(p_block->p_buffer, buf, byteCount);
    block_FifoPut(p_sys->fifo, p_block);
    smpte2038_anc_data_packet_free(pkt);

    return 0;
}



/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  Open( vlc_object_t * );
static void Close( vlc_object_t * );
static subpicture_t *Decode( decoder_t *, block_t ** );

vlc_module_begin ()
    set_description( N_("SMPTE 2038 decoder") )
    set_shortname( N_("SMPTE 2038-2008 Carriage of Ancillary Data in an MPEG-2 TS") )
    set_capability( "decoder", 50 )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_SCODEC )
    set_callbacks( Open, Close )
vlc_module_end ()

/*****************************************************************************
 * Open: probe the decoder and return score
 *****************************************************************************
 * Tries to launch a decoder and return score so that the interface is able
 * to choose.
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t *) p_this;
    decoder_sys_t *p_sys;
    
    if( p_dec->fmt_in.i_codec != VLC_CODEC_VANC )
    {
        return VLC_EGENERIC;
    }

    p_dec->p_sys = p_sys = calloc( 1, sizeof( *p_sys ) );
    if( p_sys == NULL )
        return VLC_ENOMEM;

	pe_alloc(&p_sys->pe, p_dec, (pes_extractor_callback)pes_cb, 0x1fff);

    p_sys->fifo = block_FifoNew();
    p_dec->pf_decode_sub = Decode;

    es_format_Init( &p_dec->fmt_out, SPU_ES, 0 );
    p_dec->fmt_out.video.i_chroma = VLC_CODEC_VANC;
    
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t *) p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;
    
    block_FifoRelease( p_sys->fifo );
    pe_free(&p_sys->pe);
    free(p_sys);
}

/*****************************************************************************
 * Decode:
 *****************************************************************************/
static subpicture_t *Decode( decoder_t *p_dec, block_t **pp_block )
{
    block_t       *p_block;
    subpicture_t  *p_spu = NULL;
    decoder_sys_t *p_sys = p_dec->p_sys;
    
    if( ( pp_block == NULL ) || ( *pp_block == NULL ) )
    {
        return NULL;
    }
    p_block = *pp_block;

    if( p_block->i_flags & BLOCK_FLAG_CORRUPTED )
    {
        block_Release( p_block );
        return NULL;
    }

    if (p_block->i_pts <= 0) {
        block_Release( p_block );
        return NULL;
    }
    
    /* Cache the value so it's available to the PES callback */
    p_sys->i_demux_pts = p_block->i_pts;
    
    /* Push packet into the Kernel Labs demux.  the PES callback will
       fire as needed when PES packets are fully packetized */
    pe_push(p_sys->pe, p_block->p_buffer, p_block->i_buffer / 188);

    block_Release( p_block );
    *pp_block = NULL;

    if (p_sys->subpic != NULL) {
        /* A new subpicture got created by the PES callback, so return it */
        p_spu = p_sys->subpic;
        p_sys->subpic = NULL;
    }
    
    return p_spu;
}
