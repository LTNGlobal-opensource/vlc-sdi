/*****************************************************************************
 * scte35.c: SCTE-35 Decoder
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

#include <libklscte35/scte35.h>
#include <libklvanc/vanc.h>

#define ENABLE_TEXT N_("Enable SCTE-35 decoder")
#define ENABLE_LONGTEXT N_("Enable processing of SCTE-35 messages for output as VANC" )

struct subpicture_updater_sys_t {
    uint8_t *buf;
    size_t buf_size;
};

static int SubpictureTextValidateScte35(subpicture_t *subpic,
                                  bool has_src_changed, const video_format_t *fmt_src,
                                  bool has_dst_changed, const video_format_t *fmt_dst,
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

static void SubpictureTextUpdateScte35(subpicture_t *subpic,
                                       const video_format_t *fmt_src,
                                       const video_format_t *fmt_dst,
                                       mtime_t ts)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
    int ret;
    VLC_UNUSED(fmt_src); VLC_UNUSED(ts);

    if (fmt_dst->i_sar_num <= 0 || fmt_dst->i_sar_den <= 0)
        return;

    subpic->i_original_picture_width  = fmt_dst->i_width * fmt_dst->i_sar_num / fmt_dst->i_sar_den;
    subpic->i_original_picture_height = fmt_dst->i_height;

#if 0
    fprintf(stderr, "SCTE104 formatted message : ");
    hexdump(buf, byteCount, 32);
#endif

    /* Convert a SCTE104 message into a standard VANC line. */

    /* Take an array of payload, create a fully formed VANC message,
     * including parity bits, header signatures, message type,
     * trailing checksum.
     * bitDepth of 10 is the only valid input value.
     * did: 0x41 + sdid: 0x07 = SCTE104
     */
    uint16_t *vancWords = NULL;
    uint16_t vancWordCount;
    ret = klvanc_sdi_create_payload(0x07, 0x41, sys->buf, sys->buf_size, &vancWords, &vancWordCount, 10);
    if (ret != 0) {
        fprintf(stderr, "Error creating VANC message, ret = %d\n", ret);
        return;
    }

#if 0
    printf("SCTE104 in VANC: ");
    for (int i = 0; i < vancWordCount; i++)
        printf("%03x ", *(vancWords + i));
    printf("\n");
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
        return;

    r->i_align = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
    r->i_x = 0;
    r->i_y = 12; /* FIXME: make configurable */
    memcpy(r->p_picture->Y_PIXELS, vancWords, vancWordCount * 2);
    r->p_next = subpic->p_region;
    subpic->p_region = r;

    free(vancWords); /* Free the allocated resource */
}
static void SubpictureTextDestroyScte35(subpicture_t *subpic)
{
    subpicture_updater_sys_t *sys = subpic->updater.p_sys;
    free(sys);
}

static inline subpicture_t *decoder_NewSubpictureScte35(decoder_t *decoder)
{
    subpicture_updater_sys_t *sys = calloc(1, sizeof(*sys));
    subpicture_updater_t updater = {
        .pf_validate = SubpictureTextValidateScte35,
        .pf_update   = SubpictureTextUpdateScte35,
        .pf_destroy  = SubpictureTextDestroyScte35,
        .p_sys       = sys,
    };

    subpicture_t *subpic = decoder_NewSubpicture(decoder, &updater);
    if (!subpic)
    {
        free(sys);
    }
    return subpic;
}

/*****************************************************************************
 * Module descriptor.
 *****************************************************************************/
static int  Open( vlc_object_t * );
static void Close( vlc_object_t * );
static subpicture_t *Decode( decoder_t *, block_t ** );

vlc_module_begin ()
#define SCTE35_CFG_PREFIX "scte35-"
    set_description( N_("SCTE-35 decoder") )
    set_shortname( N_("SCTE-35 Digital Program Insertion Cueing") )
    set_capability( "decoder", 50 )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_SCODEC )
    set_callbacks( Open, Close )
    add_bool( SCTE35_CFG_PREFIX "enable", true, ENABLE_TEXT, ENABLE_LONGTEXT, false )
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
    bool          enabled;

    if( p_dec->fmt_in.i_codec != VLC_CODEC_SCTE_35 )
    {
        return VLC_EGENERIC;
    }

    enabled = var_InheritBool( p_dec, SCTE35_CFG_PREFIX "enable" );
    if (!enabled)
        return VLC_EGENERIC;

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
    struct scte35_splice_info_section_s *s;

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

    p_spu = decoder_NewSubpictureScte35( p_dec );
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

        s = scte35_splice_info_section_parse(p_block->p_buffer,
                                             p_block->i_buffer);
        if (s == NULL) {
            fprintf(stderr, "Failed to splice section \n");
            return NULL;
        }

        /* Convert the SCTE35 message into a SCTE104 command */
        uint8_t *buf;
        uint16_t byteCount;
        int ret = scte35_create_scte104_message(s, &buf, &byteCount, p_block->i_pts * 9 / 100);
        if (ret != 0) {
            fprintf(stderr, "Unable to convert SCTE35 to SCTE104, ret = %d\n", ret);
            scte35_splice_info_section_free(s);
            return NULL;
        }

        /* Free the allocated resource */
        scte35_splice_info_section_free(s);
        
        p_spu_sys->buf = calloc(1, byteCount);
        if (p_spu_sys->buf != NULL) {
            memcpy(p_spu_sys->buf, buf, byteCount);
            p_spu_sys->buf_size = byteCount;
        }
        free(buf); /* Free the allocated resource */
    }

    block_Release( p_block );
    *pp_block = NULL;

    return p_spu;
}
