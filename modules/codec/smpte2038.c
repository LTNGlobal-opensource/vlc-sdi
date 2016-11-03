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
    struct pes_extractor_s *pe;
    uint8_t *buf;
    size_t buf_size;
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

static pes_extractor_callback pes_cb(void *cb_context, uint8_t *buf, int byteCount)
{
    subpicture_t *subpic = cb_context;
    struct smpte2038_anc_data_packet_s *pkt = 0;
    
	smpte2038_parse_pes_packet(buf, byteCount, &pkt);
    if (pkt == NULL) {
        fprintf(stderr, "%s failed to decode PES packet\n", __func__);
        return 0;
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

        /* Create a VLC subpicture with the VANC line */
        video_format_t fmt;
        video_format_Init(&fmt, VLC_CODEC_VANC);
        fmt.i_sar_num = 1;
        fmt.i_sar_den = 1;
        fmt.i_width = fmt.i_visible_width = vancWordCount * 2;
        fmt.i_height = fmt.i_visible_height = 1;

        subpicture_region_t *r = subpicture_region_New(&fmt);
        if (!r)
            return 0;

        r->i_align = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
        r->i_x = 0;
        r->i_y = l->line_number;
        memcpy(r->p_picture->Y_PIXELS, vancWords, vancWordCount * 2);
        r->p_next = subpic->p_region;
        subpic->p_region = r;

        free(vancWords); /* Free the allocated resource */
    }
    return 0;
}

static void SubpictureTextUpdateSmpte2038(subpicture_t *subpic,
                                          const video_format_t *fmt_src,
                                          const video_format_t *fmt_dst,
                                          mtime_t ts)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
    VLC_UNUSED(fmt_src); VLC_UNUSED(ts);

    if (fmt_dst->i_sar_num <= 0 || fmt_dst->i_sar_den <= 0)
        return;

    subpic->i_original_picture_width  = fmt_dst->i_width * fmt_dst->i_sar_num / fmt_dst->i_sar_den;
    subpic->i_original_picture_height = fmt_dst->i_height;

	pe_push(sys->pe, sys->buf, sys->buf_size / 188);
}

static void SubpictureTextDestroySmpte2038(subpicture_t *subpic)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
	pe_free(&sys->pe);
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
    {
        free(sys);
    }

	pe_alloc(&sys->pe, subpic, (pes_extractor_callback)pes_cb, 0x1fff);

    return subpic;
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

    if( p_dec->fmt_in.i_codec != VLC_CODEC_VANC )
    {
        return VLC_EGENERIC;
    }

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
}

/*****************************************************************************
 * Decode:
 *****************************************************************************/
static subpicture_t *Decode( decoder_t *p_dec, block_t **pp_block )
{
    block_t       *p_block;
    subpicture_t  *p_spu = NULL;

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

    p_spu = decoder_NewSubpictureSmpte2038( p_dec );
    if( p_spu )
    {
        subpicture_updater_sys_t *p_spu_sys = p_spu->updater.p_sys;

        p_spu->i_start = p_block->i_pts;
        p_spu->i_stop = p_spu->i_start + (CLOCK_FREQ / 30);
#if 0
        p_spu->i_stop = p_spu->i_start + CLOCK_FREQ * 10;
#endif   
        
        p_spu->b_ephemer  = false;
        p_spu->b_absolute = true;
        
        p_spu_sys->buf = calloc(1, p_block->i_buffer);
        if (p_spu_sys->buf != NULL) {
            memcpy(p_spu_sys->buf, p_block->p_buffer, p_block->i_buffer);
            p_spu_sys->buf_size = p_block->i_buffer;
        }
    }

    block_Release( p_block );
    *pp_block = NULL;

    return p_spu;
}
