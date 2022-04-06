/*
 * EVC AVC format parser
 *
 * Copyright (C) 2021 Dawid Kozinski <d.kozinski@samsung.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>

#include <xevd.h>

#include "libavutil/common.h"

#include "parser.h"
#include "golomb.h"

#define EVC_NAL_HEADER_SIZE   2 /* byte */
#define MAX_SPS_CNT  16 /* defined value in EVC standard */

typedef struct _EVCParserSPS {
    int sps_id;
    int profile_idc;
    int level_idc;
    int chroma_format_idc;
    int pic_width_in_luma_samples;
    int pic_height_in_luma_samples;
    int bit_depth_luma;
    int bit_depth_chroma;

    int picture_cropping_flag;
    int picture_crop_left_offset;
    int picture_crop_right_offset;
    int picture_crop_top_offset;
    int picture_crop_bottom_offset;
} EVCParserSPS;

typedef struct EVCParserContext {
    ParseContext pc;
    EVCParserSPS sps[MAX_SPS_CNT];
    int is_avc;
    int nal_length_size;
    int to_read;
    int incomplete_nalu_prefix_read; // The flag is set to 1 when incomplete NAL unit prefix has been read

    int got_sps;
    int got_pps;
    int got_sei;
    int got_slice;
} EVCParserContext;


#ifdef NOT_USE_XEVD_API

static av_unused int get_nalu_type(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    GetBitContext gb;
    int fzb, nut;
    int ret;

    if((ret = init_get_bits8(&gb, bs, bs_size)) < 0)
        return ret;

    fzb = get_bits1(&gb);
    if(fzb != 0)
        av_log(avctx, AV_LOG_DEBUG, "forbidden_zero_bit is not clear\n");
    nut = get_bits(&gb, 6); /* nal_unit_type_plus1 */
    return nut - 1;
}

#else

static int get_nalu_type(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    int nalu_type = 0;
    XEVD_INFO info;
    int ret;

    if(bs_size >= EVC_NAL_HEADER_SIZE) {
        ret = xevd_info((void *)bs, EVC_NAL_HEADER_SIZE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return -1;
        }
        nalu_type = info.nalu_type;

    }
    return nalu_type - 1;
}

#endif

static uint32_t read_nal_unit_length(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    uint32_t len = 0;
    XEVD_INFO info;
    int ret;

    if(bs_size >= XEVD_NAL_UNIT_LENGTH_BYTE) {
        ret = xevd_info((void *)bs, XEVD_NAL_UNIT_LENGTH_BYTE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return 0;
        }
        len = info.nalu_len;
        if(len == 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid bitstream size! 1 [%d] [%d]\n", len, bs_size);
            return 0;
        }
    }
    return len;
}

static EVCParserSPS *parse_sps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserSPS *sps;
    int sps_id;

    if(init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    sps_id = get_ue_golomb(&gb);
    if(sps_id >= MAX_SPS_CNT) goto ERR;
    sps = &ev->sps[sps_id];
    sps->sps_id = sps_id;
    
    sps->profile_idc = get_bits(&gb, 8);
    sps->level_idc = get_bits(&gb, 8);

    skip_bits_long(&gb, 32); /* skip toolset_idc_h */
    skip_bits_long(&gb, 32); /* skip toolset_idc_l */

    sps->chroma_format_idc = get_ue_golomb(&gb);
    sps->pic_width_in_luma_samples = get_ue_golomb(&gb);
    sps->pic_height_in_luma_samples = get_ue_golomb(&gb);

    sps->bit_depth_luma = get_ue_golomb(&gb);
    sps->bit_depth_chroma = get_ue_golomb(&gb);

    // @todo parse crop and vui information here

    return sps;

ERR:
    return NULL;
}

static int parse_nal_units(AVCodecParserContext *s, const uint8_t *bs,
                           int bs_size, AVCodecContext *avctx)
{
    EVCParserContext *ev = s->priv_data;
    int nalu_type, nalu_size;
    unsigned char *bits = (unsigned char *)bs;
    int bits_size = bs_size;

    avctx->codec_id = AV_CODEC_ID_EVC;

    nalu_size = read_nal_unit_length(bits, bits_size, avctx);
    if(nalu_size == 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit size: (%d)\n", nalu_size);
        return -1;
    }

    bits += XEVD_NAL_UNIT_LENGTH_BYTE;
    bits_size -= XEVD_NAL_UNIT_LENGTH_BYTE;

    nalu_type = get_nalu_type(bits, bits_size, avctx);

    bits += EVC_NAL_HEADER_SIZE;
    bits_size -= EVC_NAL_HEADER_SIZE;

    if (nalu_type == XEVD_NUT_SPS) {
        EVCParserSPS *sps;

        av_log(avctx, AV_LOG_DEBUG, "NAL Unit type: SPS (Sequence Parameter Set)\n");

        sps = parse_sps(bits, bits_size, ev);

        avctx->coded_width         = sps->pic_width_in_luma_samples;
        avctx->coded_height        = sps->pic_height_in_luma_samples;
        avctx->width               = sps->pic_width_in_luma_samples;
        avctx->height              = sps->pic_height_in_luma_samples;

        if(sps->profile_idc == 0) avctx->profile = FF_PROFILE_EVC_BASELINE;
        else if (sps->profile_idc == 1) avctx->profile = FF_PROFILE_EVC_MAIN;
        else {
            av_log(avctx, AV_LOG_ERROR, "Not supported profile (%d)\n", sps->profile_idc);
            return -1;
        }

        // Currently XEVD decoder supports ony YCBCR420_10LE chroma format for EVC stream
        switch(sps->chroma_format_idc) {
        case 0: /* YCBCR400_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR400_10LE: Not supported chroma format\n");
            avctx->pix_fmt = AV_PIX_FMT_GRAY10LE;
            return -1;
        case 1: /* YCBCR420_10LE */
            avctx->pix_fmt = AV_PIX_FMT_YUV420P10LE;
            break;
        case 2: /* YCBCR422_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR422_10LE: Not supported chroma format\n");
            avctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
            return -1;
        case 3: /* YCBCR444_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR444_10LE: Not supported chroma format\n");
            avctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
            return -1;
        default:
            avctx->pix_fmt = AV_PIX_FMT_NONE;
            av_log(avctx, AV_LOG_ERROR, "Unknown supported chroma format\n");
            return -1;
        }

        //avctx->has_b_frames = 1; // @todo FIX-ME

        ev->got_sps = 1;

    } else if (nalu_type == XEVD_NUT_PPS) {
        av_log(avctx, AV_LOG_DEBUG, "NAL Unit tpe: PPS (Video Parameter Set)\n");
        ev->got_pps = 1;
    } else if(nalu_type == XEVD_NUT_SEI) {
        av_log(avctx, AV_LOG_DEBUG, "NAL unit type: SEI (Supplemental Enhancement Information) \n");
        ev->got_sei = 1;
    } else if (nalu_type == XEVD_NUT_IDR || nalu_type == XEVD_NUT_NONIDR) {
        av_log(avctx, AV_LOG_DEBUG, "NAL Unit type: Coded slice of a IDR or non-IDR picture\n");
        ev->got_slice++;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit type: %d\n", nalu_type);
        return -1;
    }
    return 0;
}

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or END_NOT_FOUND
 */
static int evc_find_frame_end(AVCodecParserContext *s, const uint8_t *buf,
                              int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ev = s->priv_data;

    if(!ev->to_read) {
        int nal_unit_size = 0;
        int next = END_NOT_FOUND;

        // This is the case when buffer size is not enough for buffer to store NAL unit length
        if(buf_size < XEVD_NAL_UNIT_LENGTH_BYTE) {
            ev->to_read = XEVD_NAL_UNIT_LENGTH_BYTE;
            ev->nal_length_size = buf_size;
            ev->incomplete_nalu_prefix_read  = 1;

            return END_NOT_FOUND;
        }

        nal_unit_size = read_nal_unit_length(buf, buf_size, avctx);
        av_log(avctx, AV_LOG_DEBUG, "nal_unit_size: %d | buf_size: %d \n", nal_unit_size, buf_size);
        ev->nal_length_size = XEVD_NAL_UNIT_LENGTH_BYTE;

        next = nal_unit_size + XEVD_NAL_UNIT_LENGTH_BYTE;
        ev->to_read = next;
        if(next < buf_size)
            return next;
        else
            return END_NOT_FOUND;
    } else if(ev->to_read > buf_size) {
        /// @todo Consider handling the following case
        // if(ev->incomplete_nalu_prefix_read  == 1) {
        // }
        return END_NOT_FOUND;
    } else  {
        if(ev->incomplete_nalu_prefix_read  == 1) {
            EVCParserContext *ev = s->priv_data;
            ParseContext *pc = &ev->pc;
            uint8_t nalu_len[XEVD_NAL_UNIT_LENGTH_BYTE] = {0};
            int nal_unit_size = 0;

            // 1. pc->buffer contains previously read bytes of NALU prefix
            // 2. buf contains the rest of NAL unit prefix bytes
            //
            // ~~~~~~~
            // EXAMPLE
            // ~~~~~~~
            //
            // In the following example we assumed that the number of already read NAL Unit prefix bytes is equal 1
            //
            // ----------
            // pc->buffer -> conatins already read bytes
            // ----------
            //              __ pc->index == 1
            //             |
            //             V
            // -------------------------------------------------------
            // |   0   |   1   |   2   |   3   |   4   | ... |   N   |
            // -------------------------------------------------------
            // |  0x00 |  0xXX |  0xXX |  0xXX |  0xXX | ... |  0xXX |
            // -------------------------------------------------------
            //
            // ----------
            // buf -> contains newly read bytes
            // ----------
            // -------------------------------------------------------
            // |   0   |   1   |   2   |   3   |   4   | ... |   N   |
            // -------------------------------------------------------
            // |  0x00 |  0x00 |  0x3C |  0xXX |  0xXX | ... |  0xXX |
            // -------------------------------------------------------
            //
            for(int i = 0; i < XEVD_NAL_UNIT_LENGTH_BYTE; i++) {
                if(i < pc->index)
                    nalu_len[i] = pc->buffer[i];
                else
                    nalu_len[i] = buf[i - pc->index];
            }

            // ----------
            // nalu_len
            // ----------
            // ---------------------------------
            // |   0   |   1   |   2   |   3   |
            // ---------------------------------
            // |  0x00 |  0x00 |  0x00 |  0x3C |
            // ---------------------------------
            // | NALU LENGTH                   |
            // ---------------------------------
            // NAL Unit lenght =  60 (0x0000003C)

            nal_unit_size = read_nal_unit_length(nalu_len, XEVD_NAL_UNIT_LENGTH_BYTE, avctx);
            av_log(avctx, AV_LOG_DEBUG, "nal_unit_size: %d | buf_size: %d \n", nal_unit_size, buf_size);

            ev->to_read = nal_unit_size + XEVD_NAL_UNIT_LENGTH_BYTE - pc->index;

            ev->incomplete_nalu_prefix_read = 0;

            if(ev->to_read > buf_size)
                return END_NOT_FOUND;
            else
                return ev->to_read;
        }
        return ev->to_read;
    }

    return END_NOT_FOUND;
}

static int evc_parser_init(AVCodecParserContext *s)
{
    EVCParserContext *ev = s->priv_data;
    
    ev->got_sps = 0;
    ev->got_pps = 0;
    ev->got_sei = 0;
    ev->got_slice = 0;
    ev->nal_length_size = XEVD_NAL_UNIT_LENGTH_BYTE;
    ev->incomplete_nalu_prefix_read = 0;

    return 0;
}

static int evc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    int next;
    EVCParserContext *ev = s->priv_data;
    ParseContext *pc = &ev->pc;
    int is_dummy_buf = !buf_size;
    const uint8_t *dummy_buf = buf;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES)
        next = buf_size;
    else {
        next = evc_find_frame_end(s, buf, buf_size, avctx);
        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            ev->to_read -= buf_size;
            return buf_size;
        }
    }

    is_dummy_buf &= (dummy_buf == buf);

    if (!is_dummy_buf)
        parse_nal_units(s, buf, buf_size, avctx);

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    ev->to_read -= next;

    return next;
}

AVCodecParser ff_evc_parser = {
    .codec_ids      = { AV_CODEC_ID_EVC },
    .priv_data_size = sizeof(EVCParserContext),
    .parser_init    = evc_parser_init,
    .parser_parse   = evc_parse,
    .parser_close   = ff_parse_close,
};
