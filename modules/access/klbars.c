/*****************************************************************************
 * klbars.c : Kernel Labs Colorbar Input Module
 *****************************************************************************
 * Copyright (C) 2016 Kernel Labs Inc.
 *
 * Author: Devin Heitmueller <dheitmueller@kernellabs.com>
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <libklbars/klbars.h>

/* Forward declarations */
int DemuxOpen( vlc_object_t *obj );
void DemuxClose( vlc_object_t *obj );
static void *klbarsThread (void *);
static int DemuxControl( demux_t *, int, va_list );

#define WIDTH_TEXT N_( "Width" )
#define HEIGHT_TEXT N_( "Height" )
#define SIZE_LONGTEXT N_( \
    "The specified pixel resolution is forced " \
    "(if both width and height are strictly positive)." )
#define CUSTOM_TEXT N_("Custom text to be shown on line 2")
#define CUSTOM_LONGTEXT N_( \
    "Inserts the following text onto line 2 of the colorbar output" )

#define CFG_PREFIX "klbars-"

vlc_module_begin ()
    set_shortname( N_("KL Colorbars") )
    set_description( N_("Kernel Labs Colorbars input") )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_ACCESS )

    add_integer( CFG_PREFIX "width", 1920, WIDTH_TEXT, SIZE_LONGTEXT, false )
        change_integer_range( 0, VOUT_MAX_WIDTH )
        change_safe()
    add_integer( CFG_PREFIX "height", 1080, HEIGHT_TEXT, SIZE_LONGTEXT, false )
        change_integer_range( 0, VOUT_MAX_WIDTH )
        change_safe()
    add_string( CFG_PREFIX "custom-text", NULL, CUSTOM_TEXT, CUSTOM_LONGTEXT, true )
        change_safe()

    add_shortcut( "klbars" )
    set_capability( "access_demux", 0 )
    set_callbacks( DemuxOpen, DemuxClose )
vlc_module_end ()

#define MAX_AUDIOS 8
typedef struct klbars_audio_t
{
    int         i_channel; /* i_group * 2 + i_pair */

    /* ES stuff */
    es_out_id_t *p_es;
} klbars_audio_t;

struct demux_sys_t
{
    vlc_thread_t thread;

    es_out_id_t *es;
    mtime_t start;
    uint32_t frame_number;
    struct kl_colorbar_context osd_ctx;
    struct kl_colorbar_audio_context audio_ctx;
    uint32_t width;
    uint32_t height;
    char *customText;

    /* Audio related */
    klbars_audio_t p_audios[MAX_AUDIOS];
    unsigned int i_ablock_size;
    int i_aincr;
    int i_anum_channels;
    int i_asamplerate;
    int i_asamplesize; /* in bits (e.g. 16) */
    unsigned char *audio_data; /* Temporary buffer to hold generated 1Khz tone */
};

static int InitAudio( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    es_format_t fmt;

    p_sys->i_anum_channels = 2;
    p_sys->i_asamplerate = 48000;
    p_sys->i_asamplesize = 16;

    for ( int i = 0; i < MAX_AUDIOS; i++ )
    {
        klbars_audio_t *p_audio = &p_sys->p_audios[i];

        es_format_Init( &fmt, AUDIO_ES, VLC_FOURCC('a','r','a','w') );
        fmt.i_id = i;
        fmt.audio.i_channels          = p_sys->i_anum_channels;
        fmt.audio.i_original_channels =
            fmt.audio.i_physical_channels = AOUT_CHANS_STEREO;
        fmt.audio.i_rate              = p_sys->i_asamplerate;
        fmt.audio.i_bitspersample     = p_sys->i_asamplesize;
        fmt.audio.i_blockalign = fmt.audio.i_channels *
            fmt.audio.i_bitspersample / 8;
        fmt.i_bitrate = fmt.audio.i_channels * fmt.audio.i_rate *
            fmt.audio.i_bitspersample;
        p_audio->p_es = es_out_Add( p_demux->out, &fmt );
        p_sys->i_ablock_size = p_sys->i_asamplerate * (p_sys->i_asamplesize / 8) * p_sys->i_anum_channels * 1001 / 30000;
        p_sys->i_aincr = 1000000. * p_sys->i_ablock_size / p_sys->i_asamplerate / 4;
    }

    for ( int i = 0 ; i < MAX_AUDIOS; i++ )
        p_sys->p_audios[i].i_channel = -1;

    return 0;
}

int DemuxOpen( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;

    demux_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    demux->p_sys = sys;
    sys->frame_number = 0;
    sys->width = var_InheritInteger (obj, CFG_PREFIX"width");
    sys->height = var_InheritInteger (obj, CFG_PREFIX"height");
    sys->customText = var_InheritString (obj, CFG_PREFIX"custom-text");
    sys->audio_data = NULL;

    kl_colorbar_init(&sys->osd_ctx, sys->width, sys->height, KL_COLORBAR_8BIT);

    InitAudio(demux);

    /* Declare our unique elementary (video) stream */
    es_format_t es_fmt;
    es_format_Init (&es_fmt, VIDEO_ES, VLC_CODEC_UYVY);
    es_fmt.video.i_width = sys->width;
    es_fmt.video.i_height = sys->height;
    es_fmt.video.i_frame_rate = 1001;
    es_fmt.video.i_frame_rate_base = 30000;
    es_fmt.video.i_sar_num = es_fmt.video.i_sar_den = 1;
    sys->es = es_out_Add (demux->out, &es_fmt);

    /* Start capture thread */
    if (vlc_clone (&sys->thread, klbarsThread, demux, VLC_THREAD_PRIORITY_INPUT))
        goto error;

    sys->start = mdate ();
    demux->pf_demux = NULL;
    demux->pf_control = DemuxControl;
    demux->info.i_update = 0;
    demux->info.i_title = 0;
    demux->info.i_seekpoint = 0;

    return VLC_SUCCESS;

error:
    kl_colorbar_free(&sys->osd_ctx);
    free (sys);
    return VLC_EGENERIC;
}

void DemuxClose( vlc_object_t *obj )
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *sys = demux->p_sys;

    vlc_cancel (sys->thread);
    vlc_join (sys->thread, NULL);

    kl_colorbar_free(&sys->osd_ctx);
    kl_colorbar_tonegenerator_free(&sys->audio_ctx);
    free( sys->audio_data);
    free( sys );
}

static void *klbarsThread (void *data)
{
    demux_t *demux = data;
    demux_sys_t *sys = demux->p_sys;
    char label[64];

    int rowWidth = sys->width * 2;

    int buf_size = sys->i_asamplerate * (sys->i_asamplesize / 8) * sys->i_anum_channels;
    sys->audio_data = (unsigned char *) malloc (buf_size);

    /* Setup the sine data once, and we will just stuff the resulting bytes
       in when we generate each rrame of video.  Note that we pre-generate exactly
       one second worth of audio so that we can wraparound the buffer and not
       get a glitch in the waveform */
    int ret = kl_colorbar_tonegenerator(&sys->audio_ctx, 1000, sys->i_asamplesize,
                                        sys->i_anum_channels, 1000000,
                                        sys->i_asamplerate, 1);
    if (ret != 0)
        return 0;

    for (;;)
    {
        int canc = vlc_savecancel ();
        mtime_t cur_date = mdate();
        block_t *block = block_Alloc(sys->height * rowWidth);
        block->i_pts = cur_date;

        kl_colorbar_fill_colorbars(&sys->osd_ctx);

        if (sys->customText)
        {
            kl_colorbar_render_string(&sys->osd_ctx, (unsigned char *) sys->customText,
                                      strlen(sys->customText), 1, 2);
        }
        snprintf(label, sizeof(label), "Frame: %d", sys->frame_number++);
        kl_colorbar_render_string(&sys->osd_ctx, (unsigned char *) label, strlen(label), 1, 3);

        kl_colorbar_finalize(&sys->osd_ctx, block->p_buffer, rowWidth);
        if (block != NULL)
        {
            es_out_Control (demux->out, ES_OUT_SET_PCR, block->i_pts);
            es_out_Send (demux->out, sys->es, block);
        }
        vlc_restorecancel (canc);

        /* Extract the audio from the tone generator once, and propagate
           across all channels.  We do this using a temporary buffer rather
           than calling the extract routine for each channel so that the
           sine wave stays properly aligned.  */
        kl_colorbar_tonegenerator_extract(&sys->audio_ctx, sys->audio_data,
                                          sys->i_ablock_size);
        for (int i = 0; i < MAX_AUDIOS; i++)
        {
            klbars_audio_t *p_audio = &sys->p_audios[i];
            block_t *p_block = block_Alloc( sys->i_ablock_size );
            if( unlikely( !p_block ) )
                continue;

            memcpy(p_block->p_buffer, sys->audio_data, sys->i_ablock_size);
            p_block->i_dts = p_block->i_pts = cur_date;
            p_block->i_length = sys->i_aincr;
            es_out_Send( demux->out, p_audio->p_es, p_block );
        }

        /* When determining how to sleep, take into account how long it took to actually
           generate the frame */
        mtime_t now = mdate();
        mtime_t wait_time = 33367 - (now - cur_date);
        if (wait_time > 0)
            usleep(wait_time);
    }

    assert (0);
    return 0;
}

static int DemuxControl( demux_t *demux, int query, va_list args )
{
    demux_sys_t *sys = demux->p_sys;

    switch( query )
    {
        /* Special for access_demux */
        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_PACE:
            *va_arg( args, bool * ) = false;
            return VLC_SUCCESS;

        case DEMUX_GET_PTS_DELAY:
            *va_arg(args,int64_t *) = INT64_C(1000)
                * var_InheritInteger( demux, "live-caching" );
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            *va_arg (args, int64_t *) = mdate() - sys->start;
            return VLC_SUCCESS;

        /* TODO implement others */
        default:
            return VLC_EGENERIC;
    }

    return VLC_EGENERIC;
}
