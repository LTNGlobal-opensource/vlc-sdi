/*****************************************************************************
 * decklink.cpp: BlackMagic DeckLink SDI output module
 *****************************************************************************
 * Copyright (C) 2012-2013 Rafaël Carré
 * Copyright (C) 2009 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2009 Baptiste Coudurier <baptiste dot coudurier at gmail dot com>
 *
 * Authors: Rafaël Carré <funman@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*
 * TODO: test non stereo audio
 */

#define __STDC_FORMAT_MACROS
#define __STDC_CONSTANT_MACROS

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <cassert>

#include <stdint.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_threads.h>

#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>

#include <vlc_block.h>
#include <vlc_image.h>
#include <vlc_network.h>
#include <vlc_atomic.h>
#include <vlc_aout.h>
#include <arpa/inet.h>

#include <DeckLinkAPI.h>
#include <DeckLinkAPIDispatch.cpp>

#define MAX_AUDIO_SOURCES 8

/* Number of audio samples we hold in a queue, per stereo pair.
 * Queue them, then process all queues when there is sufficient
 * data from all audio upstream decoders.
 * 48000 samples per second * 16-bits per channel * 2 channels / 30 FPS
 * (i.e. 6400)
 */
#define MIN_FIFO_SIZE (48000 * 2 * 2 / 30)

static void initAudioSources();
static void destroyAudioSources();

#define FRAME_SIZE 1920
#define CHANNELS_MAX 6

#if 0
static const int pi_channels_maps[CHANNELS_MAX+1] =
{
    0,
    AOUT_CHAN_CENTER,
    AOUT_CHANS_STEREO,
    AOUT_CHANS_3_0,
    AOUT_CHANS_4_0,
    AOUT_CHANS_5_0,
    AOUT_CHANS_5_1,
};
#endif

#define NOSIGNAL_INDEX_TEXT N_("Timelength after which we assume there is no signal.")
#define NOSIGNAL_INDEX_LONGTEXT N_(\
    "Timelength after which we assume there is no signal.\n"\
    "After this delay we black out the video."\
    )

#define CCLINE_INDEX_TEXT N_("Closed Captions line.")
#define CCLINE_INDEX_LONGTEXT N_("VBI line on which to output Closed Captions.")

#define AFDLINE_INDEX_TEXT N_("Active Format Descriptor line.")
#define AFDLINE_INDEX_LONGTEXT N_("VBI line on which to output Active Format Descriptor.")

#define NOSIGNAL_IMAGE_TEXT N_("Picture to display on input signal loss.")
#define NOSIGNAL_IMAGE_LONGTEXT NOSIGNAL_IMAGE_TEXT

#define CARD_INDEX_TEXT N_("Output card")
#define CARD_INDEX_LONGTEXT N_(\
    "DeckLink output card, if multiple exist. " \
    "The cards are numbered from 0.")

#define MODE_TEXT N_("Desired output mode")
#define MODE_LONGTEXT N_(\
    "Desired output mode for DeckLink output. " \
    "This value should be a FOURCC code in textual " \
    "form, e.g. \"ntsc\".")

#define AUDIO_CONNECTION_TEXT N_("Audio connection")
#define AUDIO_CONNECTION_LONGTEXT N_(\
    "Audio connection for DeckLink output.")


#define RATE_TEXT N_("Audio sampling rate in Hz")
#define RATE_LONGTEXT N_(\
    "Audio sampling rate (in hertz) for DeckLink output. " \
    "0 disables audio output.")

#define CHANNELS_TEXT N_("Number of audio channels")
#define CHANNELS_LONGTEXT N_(\
    "Number of output channels for DeckLink output. " \
    "Must be 2, 8 or 16. 0 disables audio output.")

#define AUDIO_CHANNEL_MAP_TEXT N_("Remap audio channels")
#define AUDIO_CHANNEL_MAP_LONGTEXT N_(\
    "Channel mapping list for remapped channels.  Ex: G1P2:G2P1")

#define VIDEO_CONNECTION_TEXT N_("Video connection")
#define VIDEO_CONNECTION_LONGTEXT N_(\
    "Video connection for DeckLink output.")

#define VIDEO_TENBITS_TEXT N_("10 bits")
#define VIDEO_TENBITS_LONGTEXT N_(\
    "Use 10 bits per pixel for video frames.")

#define CFG_PREFIX "decklink-output-"
#define VIDEO_CFG_PREFIX "decklink-vout-"
#define AUDIO_CFG_PREFIX "decklink-aout-"



static const char *const ppsz_videoconns[] = {
    "sdi", "hdmi", "opticalsdi", "component", "composite", "svideo"
};
static const char *const ppsz_videoconns_text[] = {
    N_("SDI"), N_("HDMI"), N_("Optical SDI"), N_("Component"), N_("Composite"), N_("S-video")
};

static const char *const ppsz_audioconns[] = {
    "embedded", "aesebu", "analog"
};
static const char *const ppsz_audioconns_text[] = {
    N_("Embedded"), N_("AES/EBU"), N_("Analog")
};


struct vout_display_sys_t
{
    picture_pool_t *pool;
    bool tenbits;
    int nosignal_delay;
    picture_t *pic_nosignal;
};

/* Only one audio output module and one video output module
 * can be used per process.
 * We use a static mutex in audio/video submodules entry points.  */
struct decklink_sys_t
{
    IDeckLinkOutput *p_output;

    /*
     * Synchronizes aout and vout modules:
     * vout module waits until aout has been initialized.
     * That means video-only output is NOT supported.
     */
    vlc_mutex_t lock;
    vlc_cond_t cond;
    uint8_t users;
    BMDAudioConnection aconn;

    //int i_channels;
    int i_rate;
    int64_t i_max_audio_channels;
    int remap_table[MAX_AUDIO_SOURCES];

    int i_width;
    int i_height;

    BMDTimeScale timescale;
    BMDTimeValue frameduration;

    /* XXX: workaround card clock drift */
    mtime_t offset;

    int fd;
};

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/

static int  OpenVideo           (vlc_object_t *);
static void CloseVideo          (vlc_object_t *);
static int  OpenAudio           (vlc_object_t *);
static void CloseAudio          (vlc_object_t *);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

vlc_module_begin()
    set_shortname(N_("DecklinkOutput"))
    set_description(N_("output module to write to Blackmagic SDI card"))
    set_section(N_("Decklink General Options"), NULL)
    add_integer(CFG_PREFIX "card-index", 0,
                CARD_INDEX_TEXT, CARD_INDEX_LONGTEXT, true)
    add_string(CFG_PREFIX "udp-monitor", "",
                NULL, NULL, true)

    add_submodule ()
    set_description (N_("Decklink Video Output module"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 0)
    set_callbacks (OpenVideo, CloseVideo)
    set_section(N_("Decklink Video Options"), NULL)
    add_string(VIDEO_CFG_PREFIX "video-connection", "sdi",
                VIDEO_CONNECTION_TEXT, VIDEO_CONNECTION_LONGTEXT, true)
                change_string_list(ppsz_videoconns, ppsz_videoconns_text)
    add_string(VIDEO_CFG_PREFIX "mode", "",
                MODE_TEXT, MODE_LONGTEXT, true)
    add_bool(VIDEO_CFG_PREFIX "tenbits", false,
                VIDEO_TENBITS_TEXT, VIDEO_TENBITS_LONGTEXT, true)
    add_integer(VIDEO_CFG_PREFIX "nosignal-delay", 5,
                NOSIGNAL_INDEX_TEXT, NOSIGNAL_INDEX_LONGTEXT, true)
    add_integer(VIDEO_CFG_PREFIX "cc-line", 15,
                CCLINE_INDEX_TEXT, CCLINE_INDEX_LONGTEXT, true)
    add_integer(VIDEO_CFG_PREFIX "afd-line", 16,
                AFDLINE_INDEX_TEXT, AFDLINE_INDEX_LONGTEXT, true)
    add_loadfile(VIDEO_CFG_PREFIX "nosignal-image", NULL,
                NOSIGNAL_IMAGE_TEXT, NOSIGNAL_IMAGE_LONGTEXT, true)


    add_submodule ()
    set_description (N_("Decklink Audio Output module"))
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AOUT)
    set_capability("audio output", 0)
    set_callbacks (OpenAudio, CloseAudio)
    set_section(N_("Decklink Audio Options"), NULL)
    add_string(AUDIO_CFG_PREFIX "audio-connection", "embedded",
                AUDIO_CONNECTION_TEXT, AUDIO_CONNECTION_LONGTEXT, true)
                change_string_list(ppsz_audioconns, ppsz_audioconns_text)
    add_integer(AUDIO_CFG_PREFIX "audio-rate", 48000,
                RATE_TEXT, RATE_LONGTEXT, true)
    add_integer(AUDIO_CFG_PREFIX "audio-channels", 2,
                CHANNELS_TEXT, CHANNELS_LONGTEXT, true)
    add_string(AUDIO_CFG_PREFIX "channel-map", "",
                AUDIO_CHANNEL_MAP_TEXT, AUDIO_CHANNEL_MAP_LONGTEXT, true)

vlc_module_end ()

/* Protects decklink_sys_t creation/deletion */
static vlc_mutex_t sys_lock = VLC_STATIC_MUTEX;

static struct decklink_sys_t *GetDLSys(vlc_object_t *obj)
{
    vlc_object_t *libvlc = VLC_OBJECT(obj->p_libvlc);
    struct decklink_sys_t *sys;

    vlc_mutex_lock(&sys_lock);

    if (var_Type(libvlc, "decklink-sys") == VLC_VAR_ADDRESS)
        sys = (struct decklink_sys_t*)var_GetAddress(libvlc, "decklink-sys");
    else {
        sys = (struct decklink_sys_t*)malloc(sizeof(*sys));
        if (sys) {
            sys->p_output = NULL;
            sys->offset = 0;
            sys->users = 0;
            sys->aconn = 0;
            sys->fd = -1;
            char *address = var_InheritString(obj, CFG_PREFIX "udp-monitor");
            if (address) {
                char *psz = strchr(address, ':');
                int port = 0;
                if (psz) {
                    *psz++ = '\0';
                    port = atoi(psz);
                }
                if (port <= 0)
                    port = 1234;
                msg_Dbg(obj, "Monitoring on %s : %d", address, port);
                sys->fd = net_ConnectUDP(obj, address, port, -1 /* TTL? */);
                if (sys->fd < 0)
                    msg_Err(obj, "Couldn't enable monitoring (%m)");
                free(address);
            }
            vlc_mutex_init(&sys->lock);
            vlc_cond_init(&sys->cond);
            var_Create(libvlc, "decklink-sys", VLC_VAR_ADDRESS);
            var_SetAddress(libvlc, "decklink-sys", (void*)sys);
        }
    }

    initAudioSources();
    vlc_mutex_unlock(&sys_lock);
    return sys;
}

static void ReleaseDLSys(vlc_object_t *obj)
{
    vlc_object_t *libvlc = VLC_OBJECT(obj->p_libvlc);

    vlc_mutex_lock(&sys_lock);

    struct decklink_sys_t *sys = (struct decklink_sys_t*)var_GetAddress(libvlc, "decklink-sys");

    if (--sys->users == 0) {
        msg_Dbg(obj, "Destroying decklink data");
        vlc_mutex_destroy(&sys->lock);
        vlc_cond_destroy(&sys->cond);

        if (sys->p_output) {
            sys->p_output->StopScheduledPlayback(0, NULL, 0);
            sys->p_output->DisableVideoOutput();
            sys->p_output->DisableAudioOutput();
            sys->p_output->Release();
        }

        if (sys->fd > 0)
            net_Close(sys->fd);

        destroyAudioSources();

        free(sys);
        var_Destroy(libvlc, "decklink-sys");
    }

    vlc_mutex_unlock(&sys_lock);
}

// Connection mode
static BMDAudioConnection getAConn(audio_output_t *aout)
{
    BMDAudioConnection conn = bmdAudioConnectionEmbedded;
    char *psz = var_InheritString(aout, AUDIO_CFG_PREFIX "audio-connection");
    if (!psz)
        return conn;

    if (!strcmp(psz, "embedded"))
        conn = bmdAudioConnectionEmbedded;
    else if (!strcmp(psz, "aesebu"))
        conn = bmdAudioConnectionAESEBU;
    else if (!strcmp(psz, "analog"))
        conn = bmdAudioConnectionAnalog;

    free(psz);
    return conn;
}

static BMDVideoConnection getVConn(vout_display_t *vd)
{
    BMDVideoConnection conn = bmdVideoConnectionSDI;
    char *psz = var_InheritString(vd, VIDEO_CFG_PREFIX "video-connection");
    if (!psz)
        goto end;

         if (!strcmp(psz, "sdi"))
        conn = bmdVideoConnectionSDI;
    else if (!strcmp(psz, "hdmi"))
        conn = bmdVideoConnectionHDMI;
    else if (!strcmp(psz, "opticalsdi"))
        conn = bmdVideoConnectionOpticalSDI;
    else if (!strcmp(psz, "component"))
        conn = bmdVideoConnectionComponent;
    else if (!strcmp(psz, "composite"))
        conn = bmdVideoConnectionComposite;
    else if (!strcmp(psz, "svideo"))
        conn = bmdVideoConnectionSVideo;

end:
    free(psz);
    return conn;
}

/*****************************************************************************
 *
 *****************************************************************************/

static struct decklink_sys_t *OpenDecklink(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    video_format_t *fmt = &vd->fmt;
#define CHECK(message) do { \
    if (result != S_OK) \
    { \
        msg_Err(vd, message ": 0x%X", result); \
        goto error; \
    } \
} while(0)

    HRESULT result;
    IDeckLinkIterator *decklink_iterator = NULL;
    IDeckLinkDisplayMode *p_display_mode = NULL;
    IDeckLinkDisplayModeIterator *p_display_iterator = NULL;
    IDeckLinkConfiguration *p_config = NULL;
    IDeckLinkAttributes *p_attrs = NULL;
    IDeckLink *p_card = NULL;

    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(vd));
    vlc_mutex_lock(&decklink_sys->lock);
    decklink_sys->users++;

    /* wait until aout is ready */
    while (decklink_sys->aconn == 0)
        vlc_cond_wait(&decklink_sys->cond, &decklink_sys->lock);

    int i_card_index = var_InheritInteger(vd, CFG_PREFIX "card-index");
    BMDVideoConnection vconn = getVConn(vd);
    char *mode = var_InheritString(vd, VIDEO_CFG_PREFIX "mode");
    size_t len = mode ? strlen(mode) : 0;
    if (len > 4)
    {
        free(mode);
        msg_Err(vd, "Invalid mode %s", mode);
        goto error;
    }

    BMDDisplayMode wanted_mode_id;
    memset(&wanted_mode_id, ' ', 4);
    if (mode) {
        strncpy((char*)&wanted_mode_id, mode, 4);
        free(mode);
    }

    if (i_card_index < 0)
    {
        msg_Err(vd, "Invalid card index %d", i_card_index);
        goto error;
    }

    decklink_iterator = CreateDeckLinkIteratorInstance();
    if (!decklink_iterator)
    {
        msg_Err(vd, "DeckLink drivers not found.");
        goto error;
    }

    for(int i = 0; i <= i_card_index; ++i)
    {
        if (p_card)
            p_card->Release();
        result = decklink_iterator->Next(&p_card);
        CHECK("Card not found");
    }

    const char *psz_model_name;
    result = p_card->GetModelName(&psz_model_name);
    CHECK("Unknown model name");

    msg_Dbg(vd, "Opened DeckLink PCI card %s", psz_model_name);

    result = p_card->QueryInterface(IID_IDeckLinkOutput,
        (void**)&decklink_sys->p_output);
    CHECK("No outputs");

    result = p_card->QueryInterface(IID_IDeckLinkConfiguration,
        (void**)&p_config);
    CHECK("Could not get config interface");

    result = p_card->QueryInterface(IID_IDeckLinkAttributes,
        (void**)&p_attrs);
    CHECK("Could not get attributes interface");

    if (vconn)
    {
        result = p_config->SetInt(
            bmdDeckLinkConfigVideoOutputConnection, vconn);
        CHECK("Could not set video output connection");
    }

    result = decklink_sys->p_output->GetDisplayModeIterator(&p_display_iterator);
    CHECK("Could not enumerate display modes");


    unsigned int num, den;
    vlc_ureduce(&num, &den, fmt->i_frame_rate, fmt->i_frame_rate_base, 0);
    unsigned fmt_width, fmt_height;
    fmt_width = fmt->i_width;
    fmt_height = fmt->i_height;
    if (fmt_height == 480)
	fmt_height = 486; /* Decklink outputs 486 NTSC */
    msg_Info(vd, "Looking for %dx%d (%d/%d fps)", fmt_width, fmt_height, num, den);

    for (; ; p_display_mode->Release())
    {
        unsigned w, h;
        result = p_display_iterator->Next(&p_display_mode);
        if (result != S_OK)
            break;

        BMDDisplayMode mode_id = ntohl(p_display_mode->GetDisplayMode());

        const char *psz_mode_name;
        result = p_display_mode->GetName(&psz_mode_name);
        CHECK("Could not get display mode name");

        result = p_display_mode->GetFrameRate(&decklink_sys->frameduration,
            &decklink_sys->timescale);
        CHECK("Could not get frame rate");

        w = p_display_mode->GetWidth();
        h = p_display_mode->GetHeight();
        msg_Dbg(vd, "Found mode '%4.4s': %s (%dx%d, %.3f fps)",
                (char*)&mode_id, psz_mode_name, w, h,
                double(decklink_sys->timescale) / decklink_sys->frameduration);
        msg_Dbg(vd, "scale %d dur %d", (int)decklink_sys->timescale,
            (int)decklink_sys->frameduration);

        if (w == fmt_width && h == fmt_height) {
            unsigned int num_deck, den_deck;
            unsigned int num_stream, den_stream;
            vlc_ureduce(&num_deck, &den_deck,
                decklink_sys->timescale, decklink_sys->frameduration, 0);
            vlc_ureduce(&num_stream, &den_stream,
                fmt->i_frame_rate, fmt->i_frame_rate_base, 0);

            if (len == 0 && num_deck == num_stream && den_deck == den_stream) {
                msg_Info(vd, "Matches incoming stream!");
                wanted_mode_id = mode_id;
            }
        }

        if (wanted_mode_id != mode_id)
            continue;

        decklink_sys->i_width = w;
        decklink_sys->i_height = h;

        mode_id = htonl(mode_id);

        BMDVideoOutputFlags flags = bmdVideoOutputVANC;
        if (mode_id == bmdModeNTSC ||
            mode_id == bmdModeNTSC2398 ||
            mode_id == bmdModePAL)
        {
            flags = bmdVideoOutputVITC;
        }

        BMDDisplayModeSupport support;
        IDeckLinkDisplayMode *resultMode;

        result = decklink_sys->p_output->DoesSupportVideoMode(mode_id,
            sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV,
            flags, &support, &resultMode);
        CHECK("Does not support video mode");
        if (support == bmdDisplayModeNotSupported)
        {
            msg_Err(vd, "Video mode not supported");
                goto error;
        }

        result = decklink_sys->p_output->EnableVideoOutput(mode_id, flags);
        CHECK("Could not enable video output");

        break;
    }

    if (decklink_sys->i_width < 0 || decklink_sys->i_width & 1)
    {
        msg_Err(vd, "Unknown video mode specified.");
        goto error;
    }

    result = p_attrs->GetInt(BMDDeckLinkMaximumAudioChannels,
                             &decklink_sys->i_max_audio_channels);

    CHECK("Could not read maximum supported audio channels");
    msg_Dbg(vd, "Maximum audio channels supported: %ld",
            decklink_sys->i_max_audio_channels);

    result = p_config->SetInt(
        bmdDeckLinkConfigAudioInputConnection, decklink_sys->aconn);
    CHECK("Could not set audio connection");

    if (/*decklink_sys->i_channels > 0 &&*/ decklink_sys->i_rate > 0)
    {
        result = decklink_sys->p_output->EnableAudioOutput(
            decklink_sys->i_rate,
            bmdAudioSampleType16bitInteger,
            MAX_AUDIO_SOURCES * 2,
            bmdAudioOutputStreamTimestamped);
    }
    CHECK("Could not start audio output");

    /* start */
    result = decklink_sys->p_output->StartScheduledPlayback(
        (mdate() * decklink_sys->timescale) / CLOCK_FREQ, decklink_sys->timescale, 1.0);
    CHECK("Could not start playback");

    p_config->Release();
    p_attrs->Release();
    p_display_mode->Release();
    p_display_iterator->Release();
    p_card->Release();
    decklink_iterator->Release();

    vlc_mutex_unlock(&decklink_sys->lock);

    return decklink_sys;

error:
    if (decklink_sys->p_output) {
        decklink_sys->p_output->Release();
        decklink_sys->p_output = NULL;
    }
    if (p_card)
        p_card->Release();
    if (p_config)
        p_config->Release();
    if (p_attrs)
        p_attrs->Release();
    if (p_display_iterator)
        p_display_iterator->Release();
    if (decklink_iterator)
        decklink_iterator->Release();
    if (p_display_mode)
        p_display_mode->Release();

    vlc_mutex_unlock(&decklink_sys->lock);
    ReleaseDLSys(VLC_OBJECT(vd));

    return NULL;
#undef CHECK
}

/*****************************************************************************
 * Video
 *****************************************************************************/

static picture_pool_t *PoolVideo(vout_display_t *vd, unsigned requested_count)
{
    vout_display_sys_t *sys = vd->sys;
    if (!sys->pool)
        sys->pool = picture_pool_NewFromFormat(&vd->fmt, requested_count);
    return sys->pool;
}

static inline void put_le32(uint8_t **p, uint32_t d)
{
    SetDWLE(*p, d);
    (*p) += 4;
}

static inline int clip(int a)
{
    if      (a < 4) return 4;
    else if (a > 1019) return 1019;
    else               return a;
}

static void v210_convert(void *frame_bytes, picture_t *pic, int dst_stride)
{
    int width = pic->format.i_width;
    int height = pic->format.i_height;
    int line_padding = dst_stride - ((width * 8 + 11) / 12) * 4;
    int h, w;
    uint8_t *data = (uint8_t*)frame_bytes;

    const uint16_t *y = (const uint16_t*)pic->p[0].p_pixels;
    const uint16_t *u = (const uint16_t*)pic->p[1].p_pixels;
    const uint16_t *v = (const uint16_t*)pic->p[2].p_pixels;

#define WRITE_PIXELS(a, b, c)           \
    do {                                \
        val =   clip(*a++);             \
        val |= (clip(*b++) << 10) |     \
               (clip(*c++) << 20);      \
        put_le32(&data, val);           \
    } while (0)

    for (h = 0; h < height; h++) {
        uint32_t val = 0;
        for (w = 0; w < width - 5; w += 6) {
            WRITE_PIXELS(u, y, v);
            WRITE_PIXELS(y, u, y);
            WRITE_PIXELS(v, y, u);
            WRITE_PIXELS(y, v, y);
        }
        if (w < width - 1) {
            WRITE_PIXELS(u, y, v);

            val = clip(*y++);
            if (w == width - 2)
                put_le32(&data, val);
#undef WRITE_PIXELS
        }
        if (w < width - 3) {
            val |= (clip(*u++) << 10) | (clip(*y++) << 20);
            put_le32(&data, val);

            val = clip(*v++) | (clip(*y++) << 10);
            put_le32(&data, val);
        }

        memset(data, 0, line_padding);
        data += line_padding;

        y += pic->p[0].i_pitch / 2 - width;
        u += pic->p[1].i_pitch / 2 - width / 2;
        v += pic->p[2].i_pitch / 2 - width / 2;
    }
}

static void report(vlc_object_t *obj, const char *str, uint64_t val)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(obj);
    int fd = decklink_sys->fd;

    if (fd < 0)
        return;

    char *buf;
    if (asprintf(&buf, "%s: %"PRId64"\n", str, val) < 0)
        return;

    net_Write(obj, fd, NULL, buf, strlen(buf));
    free(buf);
}
#define report(obj, str, val) report(VLC_OBJECT(obj), str, val)

static void send_AFD(vout_display_t *vd, uint8_t *buf)
{
    const size_t len = 6 /* vanc header */ + 8 /* AFD data */ + 1 /* csum */;
    const size_t s = ((len + 5) / 6) * 6; // align for v210

    uint16_t afd[s];

    afd[0] = 0x000;
    afd[1] = 0x3ff;
    afd[2] = 0x3ff;
    afd[3] = 0x41; // DID
    afd[4] = 0x05; // SDID
    afd[5] = 8; // Data Count

    int afd_code = 0;
    int AR = 0;
    int bar_data_flags = 0;
    int bar_data_val1 = 0;
    int bar_data_val2 = 0;

    afd[ 6] = (afd_code << 3) | (AR << 2);
    afd[ 7] = 0; // reserved
    afd[ 8] = 0; // reserved
    afd[ 9] = bar_data_flags << 4;
    afd[10] = bar_data_val1 << 8;
    afd[11] = bar_data_val1 & 0xff;
    afd[12] = bar_data_val2 << 8;
    afd[13] = bar_data_val2 & 0xff;

    /* parity bit */
    for (size_t i = 3; i < len - 1; i++)
        afd[i] |= parity(afd[i]) ? 0x100 : 0x200;

    /* vanc checksum */
    uint16_t vanc_sum = 0;
    for (size_t i = 3; i < len - 1; i++) {
        vanc_sum += afd[i];
        vanc_sum &= 0x1ff;
    }

    afd[len - 1] = vanc_sum | ((~vanc_sum & 0x100) << 1);

    /* pad */
    for (size_t i = len; i < s; i++)
        afd[i] = 0x040;

    /* convert to v210 and write into VANC */
    for (size_t w = 0; w < s / 6 ; w++) {
        put_le32(&buf, afd[w*6+0] << 10);
        put_le32(&buf, afd[w*6+1] | (afd[w*6+2] << 20));
        put_le32(&buf, afd[w*6+3] << 10);
        put_le32(&buf, afd[w*6+4] | (afd[w*6+5] << 20));
    }
}

static void send_vanc_msg(vout_display_t *vd, uint8_t *buf, uint16_t *msg, uint16_t msgWordLength)
{
    /* convert to v210 and write into VBI line of VANC */
    size_t len = msgWordLength / 6;
    for (size_t w = 0; w < len; w++) {
        put_le32(&buf, msg[w * 6 + 0] << 10);
        put_le32(&buf, msg[w * 6 + 1] | (msg[w * 6 + 2] << 20));
        put_le32(&buf, msg[w * 6 + 3] << 10);
        put_le32(&buf, msg[w * 6 + 4] | (msg[w * 6 + 5] << 20));
    }
}

/* 708 */
static void send_CC(vout_display_t *vd, cc_data_t *cc, uint8_t *buf)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(vd));
    uint8_t cc_count = cc->i_data / 3;
    if (cc_count == 0)
        return;

    assert(cc_count == 20);

    report(vd, "CC COUNT", 20);
    #if 0
    if (cc_count < 20) {
        printf("PADDING\n");
        for(int i = cc_count; i < 20; i++) {
            cc->p_data[3*i + 0] = 0xfa;
            cc->p_data[3*i + 0] = 0x00;
            cc->p_data[3*i + 0] = 0x00;
        }
        cc_count = 20;
        cc->i_data = 3 * 20;
    }
    #endif

    uint16_t len = 6 /* vanc header */
        + 9 /* cdp header */
        + 3 * cc_count /* cc_data */
        + 4 /* cdp footer */
        + 1 /* vanc checksum */;

    static uint16_t hdr = 0; /* cdp counter */
    size_t s = ((len + 5) / 6) * 6; /* align to 6 for v210 conversion */

    uint16_t *cdp = new uint16_t[s];

    unsigned int num, den;
    vlc_ureduce(&num, &den, decklink_sys->timescale, decklink_sys->frameduration, 0);

    int rate;
    if (num == 24000 && den == 1001) {
        rate = 1;
    } else if (num == 24 && den == 1) {
        rate = 2;
    } else if (num == 25 && den == 1) {
        rate = 3;
    } else if (num == 30000 && den == 1001) {
        rate = 4;
    } else if (num == 30 && den == 1) {
        rate = 5;
    } else if (num == 50 && den == 1) {
        rate = 6;
    } else if (num == 60000 && den == 1001) {
        rate = 7;
    } else if (num == 60 && den == 1) {
        rate = 8;
    } else {
        printf("Unknown frame rate %d / %d = %.3f\n", num, den, (float)num / den);
        return;
    }

    uint16_t cdp_header[6+9] = {
        /* VANC header = 6 words */
        0x000, 0x3ff, 0x3ff, /* Ancillary Data Flag */

        /* following words need parity bits */

        0x61, /* Data ID */
        0x01, /* Secondary Data I D= CEA-708 */
        (uint16_t)(len - 6 - 1), /* Data Count (not including VANC header) */

        /* cdp header */

        0x96, // header id
        0x69,
        (uint16_t)(len - 6 - 1),
        (uint16_t)((rate << 4) | 0x0f),
        0x43, // cc_data_present | caption_service_active | reserved
        (uint16_t)(hdr >> 8),
        (uint16_t)(hdr & 0xff),
        0x72, // ccdata_id
        (uint16_t)(0xe0 | cc_count), // cc_count
    };

    /* cdp header */
    memcpy(cdp, cdp_header, sizeof(cdp_header));

    /* cdp data */
    for (size_t i = 0; i < cc_count; i++) { // copy cc_data
        cdp[6+9+3*i+0] = cc->p_data[3*i+0] /*| 0xfc*/; // marker bits + cc_valid
        cdp[6+9+3*i+1] = cc->p_data[3*i+1];
        cdp[6+9+3*i+2] = cc->p_data[3*i+2];
    }

    /* cdp footer */
    cdp[len-5] = 0x74; // footer id
    cdp[len-4] = hdr >> 8;
    cdp[len-3] = hdr & 0xff;
    hdr++;

    /* cdp checksum */
    uint8_t sum = 0;
    for (uint16_t i = 6; i < len - 2; i++) {
        sum += cdp[i];
        sum &= 0xff;
    }
    cdp[len-2] = sum ? 256 - sum : 0;

    /* parity bit */
    for (uint16_t i = 3; i < len - 1; i++)
        cdp[i] |= parity(cdp[i]) ? 0x100 : 0x200;

    /* vanc checksum */
    uint16_t vanc_sum = 0;
    for (uint16_t i = 3; i < len - 1; i++) {
        vanc_sum += cdp[i];
        vanc_sum &= 0x1ff;
    }
    cdp[len - 1] = vanc_sum | ((~vanc_sum & 0x100) << 1);

    /* pad */
    for (size_t i = len; i < s; i++)
        cdp[i] = 0x040;

    /* convert to v210 and write into VBI line of VANC */
    for (size_t w = 0; w < s / 6 ; w++) {
        put_le32(&buf, cdp[w*6+0] << 10);
        put_le32(&buf, cdp[w*6+1] | (cdp[w*6+2] << 20));
        put_le32(&buf, cdp[w*6+3] << 10);
        put_le32(&buf, cdp[w*6+4] | (cdp[w*6+5] << 20));
    }

    delete[] cdp;
}

static void DisplayVideo(vout_display_t *vd, picture_t *picture, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(vd));
    mtime_t now = mdate();

    if (!picture)
        return;

    picture_t *orig_picture = picture;

    if (now - picture->date > sys->nosignal_delay * CLOCK_FREQ) {
        report(vd, "NO SIGNAL", picture->date);
        msg_Dbg(vd, "no signal");
        if (sys->pic_nosignal) {
            picture = sys->pic_nosignal;
        } else {
            if (sys->tenbits) { // I422_10L
                plane_t *y = &picture->p[0];
                memset(y->p_pixels, 0x0, y->i_lines * y->i_pitch);
                for (int i = 1; i < picture->i_planes; i++) {
                    plane_t *p = &picture->p[i];
                    size_t len = p->i_lines * p->i_pitch / 2;
                    int16_t *data = (int16_t*)p->p_pixels;
                    for (size_t j = 0; j < len; j++) // XXX: SIMD
                        data[j] = 0x200;
                }
            } else { // UYVY
                size_t len = picture->p[0].i_lines * picture->p[0].i_pitch;
                for (size_t i = 0; i < len; i+= 2) { // XXX: SIMD
                    picture->p[0].p_pixels[i+0] = 0x80;
                    picture->p[0].p_pixels[i+1] = 0;
                }
            }
        }
        picture->date = now;
    }

    HRESULT result;
    int w, h, stride, length;
    w = decklink_sys->i_width;
    h = decklink_sys->i_height;

    IDeckLinkMutableVideoFrame *pDLVideoFrame;
    result = decklink_sys->p_output->CreateVideoFrame(w, h, w*3,
        sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV,
        bmdFrameFlagDefault, &pDLVideoFrame);

    if (result != S_OK) {
        msg_Err(vd, "Failed to create video frame: 0x%X", result);
        report(vd, "ERROR PICTURE", (uint64_t)result);
        pDLVideoFrame = NULL;
        goto end;
    }

    report(vd, "PICTURE", picture->date);

    void *frame_bytes;
    pDLVideoFrame->GetBytes((void**)&frame_bytes);
    stride = pDLVideoFrame->GetRowBytes();

    if (sys->tenbits) {
        IDeckLinkVideoFrameAncillary *vanc;
        result = decklink_sys->p_output->CreateAncillaryData(
                sys->tenbits ? bmdFormat10BitYUV : bmdFormat8BitYUV, &vanc);
        if (result != S_OK) {
            msg_Err(vd, "Failed to create vanc: %d", result);
            goto end;
        }

        int line = var_InheritInteger(vd, VIDEO_CFG_PREFIX "cc-line");
        void *buf;
        result = vanc->GetBufferForVerticalBlankingLine(line, &buf);
        if (result != S_OK) {
            msg_Err(vd, "Failed to get VBI line %d: %d", line, result);
            goto end;
            //break;
        }

	/* TODO: Bug. If the AFD and CC are on the same line,
	 * the second attempt to write to the same line trashes the
	 * first message. Each call to GetBufferForVerticalBlankingLine()
	 * returns the beginning of the line buffer, and we trample it.
	 * Workaround: Ensure ALL VANC messages are on seperate lines for the time being.
	 */

        send_CC(vd, &picture->cc, (uint8_t*)buf);

        if (0 && picture->cc.i_data) {
            printf("cc_count %d: ", picture->cc.i_data / 3);
            for (int i = 0; i < picture->cc.i_data; i++)
                printf("%.2x ", picture->cc.p_data[i]);
            printf("\n");
        }

	/* Output VANC lines provided by decoders */
	if (subpicture) {
	  for (subpicture_region_t *r = subpicture->p_region; r != NULL;
	       r = r->p_next) {
	    if (r->fmt.i_chroma == VLC_CODEC_VANC) {
	      result = vanc->GetBufferForVerticalBlankingLine(r->i_y, &buf);
	      if (result == S_OK) {
		send_vanc_msg(vd, (uint8_t *) buf, (uint16_t *) r->p_picture->Y_PIXELS,
			      r->fmt.i_width / sizeof(uint16_t));
	      }
	    }
	  }
	}

        line = var_InheritInteger(vd, VIDEO_CFG_PREFIX "afd-line");
        result = vanc->GetBufferForVerticalBlankingLine(line, &buf);
        if (result != S_OK) {
            msg_Err(vd, "Failed to get VBI line %d: %d", line, result);
            goto end;
        }
        send_AFD(vd, (uint8_t*)buf);

        v210_convert(frame_bytes, picture, stride);

        result = pDLVideoFrame->SetAncillaryData(vanc);
        vanc->Release();
        if (result != S_OK) {
            msg_Err(vd, "Failed to set vanc: %d", result);
            goto end;
        }

    } else for(int y = 0; y < h; ++y) {
        uint8_t *dst = (uint8_t *)frame_bytes + stride * y;
        const uint8_t *src = (const uint8_t *)picture->p[0].p_pixels +
            picture->p[0].i_pitch * y;
        memcpy(dst, src, w * 2 /* bpp */);
    }


    // compute frame duration in CLOCK_FREQ units
    length = (decklink_sys->frameduration * CLOCK_FREQ) / decklink_sys->timescale;

    picture->date -= decklink_sys->offset;
    result = decklink_sys->p_output->ScheduleVideoFrame(pDLVideoFrame,
        picture->date, length, CLOCK_FREQ);

    if (result != S_OK) {
        msg_Err(vd, "Dropped Video frame %"PRId64 ": 0x%x",
            picture->date, result);
        goto end;
    }

    now = mdate() - decklink_sys->offset;

    BMDTimeValue decklink_now;
    double speed;
    decklink_sys->p_output->GetScheduledStreamTime (CLOCK_FREQ, &decklink_now, &speed);
    report(vd, "CARD TIME", (uint64_t)decklink_now);
    report(vd, "VLC TIME", (uint64_t)now);

    if ((now - decklink_now) > 400000) {
        /* XXX: workaround card clock drift */
        decklink_sys->offset += 50000;
        msg_Err(vd, "Delaying: offset now %"PRId64"", decklink_sys->offset);
    }

end:
    if (pDLVideoFrame)
        pDLVideoFrame->Release();
    picture_Release(orig_picture);
}

static int ControlVideo(vout_display_t *vd, int query, va_list args)
{
    VLC_UNUSED(vd);
    const vout_display_cfg_t *cfg;

    switch (query) {
    case VOUT_DISPLAY_CHANGE_FULLSCREEN:
        cfg = va_arg(args, const vout_display_cfg_t *);
        return cfg->is_fullscreen ? VLC_EGENERIC : VLC_SUCCESS;
    default:
        return VLC_EGENERIC;
    }
}

static int OpenVideo(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    video_format_t *fmt = &vd->fmt;
    vout_display_sys_t *sys;
    struct decklink_sys_t *decklink_sys;

    vd->sys = sys = (vout_display_sys_t*)malloc(sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->tenbits = var_InheritBool(p_this, VIDEO_CFG_PREFIX "tenbits");
    sys->nosignal_delay = var_InheritInteger(p_this, VIDEO_CFG_PREFIX "nosignal-delay");
    sys->pic_nosignal = NULL;

    decklink_sys = OpenDecklink(vd);
    if (!decklink_sys) {
        if (sys->pic_nosignal)
            picture_Release(sys->pic_nosignal);
        free(sys);
        return VLC_EGENERIC;
    }

    sys->pool = NULL;

    fmt->i_chroma = sys->tenbits
        ? VLC_CODEC_I422_10L /* we will convert to v210 */
        : VLC_CODEC_UYVY;
    //video_format_FixRgb(fmt);

    fmt->i_width = decklink_sys->i_width;
    fmt->i_height = decklink_sys->i_height;

    /* FIXME: should only advertise VANC for SDI outputs */
    vd->info.supports_vanc = true;

    char *pic_file = var_InheritString(p_this, VIDEO_CFG_PREFIX "nosignal-image");
    if (pic_file) {
        image_handler_t *img = image_HandlerCreate(p_this);
        if (!img) {
            msg_Err(p_this, "Could not create image converter");
        } else {
            video_format_t in, dummy;

            video_format_Init(&in, 0);
            video_format_Setup(&in, 0, fmt->i_width, fmt->i_height, 1, 1);

            video_format_Init(&dummy, 0);

            picture_t *png = image_ReadUrl(img, pic_file, &dummy, &in);
            if (png) {
                msg_Err(p_this, "Converting");
                sys->pic_nosignal = image_Convert(img, png, &in, fmt);
                picture_Release(png);
            }

            image_HandlerDelete(img);
        }

        free(pic_file);
        if (!sys->pic_nosignal) {
            CloseVideo(p_this);
            msg_Err(p_this, "Could not create no signal picture");
            return VLC_EGENERIC;
        }
    }
    vd->info.has_hide_mouse = true;
    vd->pool    = PoolVideo;
    vd->prepare = NULL;
    vd->display = DisplayVideo;
    vd->control = ControlVideo;
    vd->manage  = NULL;
    vout_display_SendEventFullscreen(vd, false);

    return VLC_SUCCESS;
}

static void CloseVideo(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;

    if (sys->pool)
        picture_pool_Delete(sys->pool);

    if (sys->pic_nosignal)
        picture_Release(sys->pic_nosignal);

    free(sys);

    ReleaseDLSys(p_this);
}

/*****************************************************************************
 * Audio
 *****************************************************************************/

static struct audio_source_s
{
	int		nr;		/* 1..MAX_AUDIO_SOURCES */
	audio_output_t *aout;
	block_fifo_t   *fifo;		/* PCM data for a given audio pair */
	int             fifo_ready;	/* Flag: Whether the fifo contains 'enough' data to process */
} audioSources[MAX_AUDIO_SOURCES];

static vlc_mutex_t g_audio_source_lock = VLC_STATIC_MUTEX;
static int g_audio_source_init = 1;
static int g_audio_source_count = 0;

static void initAudioSources()
{
	if (g_audio_source_init) {
		g_audio_source_init = 0;
		g_audio_source_count = 0;
		vlc_mutex_init(&g_audio_source_lock);
		memset(audioSources, 0, sizeof(audioSources));
	}
}

static void destroyAudioSources()
{
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		struct audio_source_s *s = &audioSources[i];
		if (s->nr == 0)
			continue;

		s->nr = 0;
		block_FifoRelease(s->fifo);
	}
}

static void destroyAudioSource(audio_output_t *aout)
{
	vlc_mutex_lock(&g_audio_source_lock);
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		struct audio_source_s *s = &audioSources[i];
		if (s->aout == aout) {
			s->nr = 0;
			s->aout = 0;
			block_FifoRelease(s->fifo);
			break;
		}
	}
	vlc_mutex_unlock(&g_audio_source_lock);
}
static int createAudioSource(audio_output_t *aout)
{
	int ret = -1;

	vlc_mutex_lock(&g_audio_source_lock);
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		struct audio_source_s *s = &audioSources[i];
		if (s->nr == 0) {
			s->nr = ++g_audio_source_count;
			s->aout = aout;
			s->fifo = block_FifoNew();
			//printf("Created Audio Source[%d] nr = %d aout=%p\n", i, s->nr, s->aout);
			ret = 0;
			break;
		}
	}
	vlc_mutex_unlock(&g_audio_source_lock);

	return ret;
}

static struct audio_source_s *findAudioSource(audio_output_t *aout)
{
	struct audio_source_s *ret = 0;

	vlc_mutex_lock(&g_audio_source_lock);
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		if (audioSources[i].aout == aout) {
			ret = &audioSources[i];
			break;
		}
	}
	vlc_mutex_unlock(&g_audio_source_lock);

	return ret;
}

static int sourcesReadyToRender()
{
	int active_sources = 0, ready_sources = 0;

	vlc_mutex_lock(&g_audio_source_lock);
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
		if (audioSources[i].nr > 0) {
			active_sources++;
			if (block_FifoSize(audioSources[i].fifo) >= MIN_FIFO_SIZE)
				ready_sources++;
		}
	}
	vlc_mutex_unlock(&g_audio_source_lock);

	return active_sources == ready_sources;
}

static void Flush (audio_output_t *aout, bool drain)
{
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));
    vlc_mutex_lock(&decklink_sys->lock);
    IDeckLinkOutput *p_output = decklink_sys->p_output;
    vlc_mutex_unlock(&decklink_sys->lock);
    if (!p_output)
        return;

    if (drain) {
        uint32_t samples;
        decklink_sys->p_output->GetBufferedAudioSampleFrameCount(&samples);
        msleep(CLOCK_FREQ * samples / decklink_sys->i_rate);
    } else if (decklink_sys->p_output->FlushBufferedAudioSamples() == E_FAIL)
        msg_Err(aout, "Flush failed");
}

static int TimeGet(audio_output_t *, mtime_t* restrict)
{
    /* synchronization is handled by the card */
    return -1;
}

static int Start(audio_output_t *aout, audio_sample_format_t *restrict fmt)
{
    struct audio_source_s *s = findAudioSource(aout);
    if (!s)
        return VLC_EGENERIC;

    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));

    if (decklink_sys->i_rate == 0)
        return VLC_EGENERIC;

    fmt->i_format = VLC_CODEC_S16N;
    fmt->i_channels = 2; //decklink_sys->i_channels;
    fmt->i_physical_channels = AOUT_CHANS_STEREO; //pi_channels_maps[fmt->i_channels];
    fmt->i_rate = decklink_sys->i_rate;
    fmt->i_bitspersample = 16;
    fmt->i_blockalign = fmt->i_channels * fmt->i_bitspersample /8 ;
    fmt->i_frame_length  = FRAME_SIZE;

    return VLC_SUCCESS;
}

static int groupPairStringToSlot(const char *grouppair)
{
  unsigned int group;
  unsigned int pair;
  if (sscanf(grouppair, "G%uP%u", &group, &pair) == 2) {
    if (group == 0 || group > 8)
      return -1;
    if (pair == 0 || pair > 2)
      return -1;
    return (group - 1) * 2 + (pair - 1);
  } else {
    return -1;
  }
}

static void interleaveAudio(unsigned char *inbuf, unsigned char *out, int num_samples,
			    int out_channel)
{
    int x = out_channel * 4;
    for (int c = 0; c < num_samples * 4; c++) {
       out[x] = inbuf[c++];
       out[x+1] = inbuf[c++];
       out[x+2] = inbuf[c++];
       out[x+3] = inbuf[c];
       x += (4 * MAX_AUDIO_SOURCES);
    }
}

/* Don't call this unless all the fifos are ready */
static block_t *audioFramer(struct decklink_sys_t *decklink_sys, block_t *firstchan_block,
			    int firstchan_num)
{
   block_t *final_block = block_Alloc(firstchan_block->i_buffer * MAX_AUDIO_SOURCES);
   memset(final_block->p_buffer, 0, final_block->i_buffer);
   final_block->i_pts = firstchan_block->i_pts;
   final_block->i_nb_samples = firstchan_block->i_nb_samples;

   vlc_mutex_lock(&g_audio_source_lock);

   /* Handle the first channel differently than the others since we
      already dequeued the block for it */
   interleaveAudio(firstchan_block->p_buffer, final_block->p_buffer,
		   firstchan_block->i_nb_samples,
		   decklink_sys->remap_table[firstchan_num]);

   /* Handle all the other channels */
   for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
      if (audioSources[i].nr == 0 || audioSources[i].nr == 1)
	  continue;
      block_t *blk = block_FifoShow(audioSources[i].fifo);
      long int delta = firstchan_block->i_pts - blk->i_pts;

      if (delta < -30000) {
	/* Because the earliest block in this queue is much later than the
	   PTS of the first channel, we're going to be putting out audio
	   silence on this channel until we get to that block.  So let's
	   keep the block on the queue until the first channel catches up
	   to that point... */
	continue;
      }

      /* Throw away samples that are way too old */
      int found = 0;
      while (block_FifoSize(audioSources[i].fifo) > MIN_FIFO_SIZE) {
	blk = block_FifoShow(audioSources[i].fifo);
	delta = firstchan_block->i_pts - blk->i_pts;
	if (delta < 30000) {
	  found = 1;
	  break;
	}

	blk = block_FifoGet(audioSources[i].fifo);
	block_Release(blk);
      }

      /* Either we found a reasonably recent sample or we ran discarded all the
	 available items in the FIFO and ran out of samples to look at */
      if (found == 0)
	continue;

      blk = block_FifoGet(audioSources[i].fifo);
      interleaveAudio(blk->p_buffer, final_block->p_buffer,
		      firstchan_block->i_nb_samples,
		      decklink_sys->remap_table[i]);
      block_Release(blk);
   }

   vlc_mutex_unlock(&g_audio_source_lock);

   return final_block;
}

static void PlayAudio(audio_output_t *aout, block_t *audio)
{
    struct audio_source_s *s = findAudioSource(aout);
    if (!s)
        return;

    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));
    vlc_mutex_lock(&decklink_sys->lock);
    IDeckLinkOutput *p_output = decklink_sys->p_output;
    vlc_mutex_unlock(&decklink_sys->lock);
    if (!p_output) {
        block_Release(audio);
        return;
    }

    audio->i_pts -= decklink_sys->offset;

    /* We need to copy the audio block because the block_t is already
       on a list and putting it onto a block_Fifo will corrupt p_next */
    struct block_t *p_block_copy = block_Duplicate(audio);
    if (p_block_copy == NULL) {
        msg_Err(aout, "Block duplication failed");
        block_Release(audio);
        return;
    }

    /* Done with the original audio block handed to us by the decoder */
    block_Release(audio);

    /* Push the current audio pair payload into its fifo */
    block_FifoPut(s->fifo, p_block_copy);

    /* We slave the entire output from a single pts, we'll use the pts from the
     * first audio pair. If we're not currently the first audio pair, we're done.
     */
    if (s->nr != 1) {
        return;
    }

    /* Enumerate all the source fifos, check if they have enough data for us to output
     * a buffer containing all audio for all pairs.
     */
    if (sourcesReadyToRender() == 0)
        return;

    block_t *firstchan_block = block_FifoGet(s->fifo);

    /* Walk the source fifos, prepare a combined buffer for delivery. */
    block_t *p_final = audioFramer(decklink_sys, firstchan_block, s->nr - 1);
    block_Release(firstchan_block);

    uint32_t written;
    HRESULT result = p_output->ScheduleAudioSamples(p_final->p_buffer, p_final->i_nb_samples, 
						    p_final->i_pts, CLOCK_FREQ, &written);

    report(aout, "PLAY AUDIO BYTES", (uint64_t)audio->i_buffer);

    uint32_t samples;
    p_output->GetBufferedAudioSampleFrameCount(&samples);
    report(aout, "AUDIO BUFFERED SAMPLES", samples);

    if (result != S_OK) {
        msg_Err(aout, "Failed to schedule audio sample: 0x%X", result);
        report(aout, "ERROR AUDIO", (uint64_t)result);
    }
    else if (p_final->i_nb_samples != written) {
        msg_Err(aout, "Written only %d samples out of %d", written, p_final->i_nb_samples);
        report(aout, "ERROR AUDIO SAMPLES LOST", (uint64_t)(p_final->i_nb_samples - written));
    }
    block_Release(p_final);
}

static int OpenAudio(vlc_object_t *p_this)
{
    audio_output_t *aout = (audio_output_t *)p_this;
    struct decklink_sys_t *decklink_sys = GetDLSys(VLC_OBJECT(aout));

    createAudioSource((audio_output_t *)p_this);

    vlc_mutex_lock(&decklink_sys->lock);
    decklink_sys->aconn = getAConn(aout);
    //decklink_sys->i_channels = var_InheritInteger(vd, AUDIO_CFG_PREFIX "audio-channels");
    decklink_sys->i_rate = var_InheritInteger(aout, AUDIO_CFG_PREFIX "audio-rate");
    decklink_sys->users++;
    vlc_cond_signal(&decklink_sys->cond);
    vlc_mutex_unlock(&decklink_sys->lock);

    /* Initialize remap table for passthrough */
	for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
        decklink_sys->remap_table[i] = i;
	}

	char *remap_string = var_InheritString(p_this, AUDIO_CFG_PREFIX "channel-map");
	//    char *remap_string = "G2P2:G1P2:G8P1";
    if (remap_string) {
        /* If the user specifies a remap table, all non-specified channels
           should be discarded */
        for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
            decklink_sys->remap_table[i] = -1;
        }

        /* Loop through the string and extract channels */
        char s_remap_chan[MAX_AUDIO_SOURCES][5];
        memset(&s_remap_chan, 0, sizeof(s_remap_chan));
        sscanf(remap_string, "%4c:%4c:%4c:%4c:%4c:%4c:%4c:%4c",
               s_remap_chan[0], s_remap_chan[1], s_remap_chan[2],
               s_remap_chan[3], s_remap_chan[4], s_remap_chan[5],
               s_remap_chan[6], s_remap_chan[7]);

        for (int i = 0; i < MAX_AUDIO_SOURCES; i++) {
            int remap_dest = groupPairStringToSlot(s_remap_chan[i]);

            /* FIXME: convert to using decklink_sys->i_max_audio_channels */
            if (remap_dest >= MAX_AUDIO_SOURCES) {
                fprintf(stderr, "Channel %s not supported by this card\n",
                        s_remap_chan[i]);
                continue;
            }
            decklink_sys->remap_table[i] = remap_dest;
        }
    }

    aout->play      = PlayAudio;
    aout->start     = Start;
    aout->flush     = Flush;
    aout->time_get  = TimeGet;

    aout->pause     = NULL;
    aout->stop      = NULL;
    aout->mute_set  = NULL;
    aout->volume_set= NULL;

    return VLC_SUCCESS;
}

static void CloseAudio(vlc_object_t *p_this)
{
    destroyAudioSource((audio_output_t *)p_this);

    struct decklink_sys_t *decklink_sys = GetDLSys(p_this);
    vlc_mutex_lock(&decklink_sys->lock);
    decklink_sys->aconn = 0;
    vlc_mutex_unlock(&decklink_sys->lock);
    ReleaseDLSys(p_this);
}
