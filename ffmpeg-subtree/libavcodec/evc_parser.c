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

// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
typedef struct EVCParserSPS {
    int sps_seq_parameter_set_id; // ue(v)
    int profile_idc; // u(8)
    int level_idc; // u(8)
    // toolset_idc_h u(32)
    // toolset_idc_l u(32)
    int chroma_format_idc;  // ue(v)
    int pic_width_in_luma_samples; // ue(v)
    int pic_height_in_luma_samples; // ue(v)
    int bit_depth_luma_minus8;
    int bit_depth_chroma_minus8;
    
    int sps_btt_flag; // u(1)
    int log2_ctu_size_minus5; // ue(v)
    int log2_min_cb_size_minus2; // ue(v)
    int log2_diff_ctu_max_14_cb_size; // ue(v)
    int log2_diff_ctu_max_tt_cb_size; // ue(v)
    int log2_diff_min_cb_min_tt_cb_size_minus2; // ue(v)

    int sps_suco_flag; // u(1)
    int log2_diff_ctu_size_max_suco_cb_size; // ue(v)
    int log2_diff_max_suco_min_suco_cb_size; // ue(v)

    int sps_admvp_flag; // u(1)
    int sps_affine_flag; // u(1)
    int sps_amvr_flag; // u(1)
    int sps_dmvr_flag; // u(1)
    int sps_mmvd_flag; // u(1)
    int sps_hmvp_flag; // u(1)

    int sps_eipd_flag; // u(1)
    int sps_ibc_flag; // u(1)
    int log2_max_ibc_cand_size_minus2; // ue(v)

    int sps_cm_init_flag; // u(1)
    int sps_adcc_flag; // u(1)

    int sps_iqt_flag; // u(1)
    int sps_ats_flag; // u(1)

    int sps_addb_flag; // u(1)
    int sps_alf_flag; // u(1)
    int sps_htdf_flag; // u(1)
    int sps_rpl_flag; // u(1)
    int sps_pocs_flag; // u(1)
    int sps_dquant_flag; // u(1)
    int sps_dra_flag; // u(1)

    int log2_max_pic_order_cnt_lsb_minus4; // ue(v)
    int log2_sub_gop_length; // ue(v)
    int log2_ref_pic_gap_length; // ue(v)

    int max_num_tid0_ref_pics; // ue(v)

    int sps_max_dec_pic_buffering_minus1; // ue(v)
    int long_term_ref_pic_flag; // u(1)
    int rpl1_same_as_rpl0_flag; // u(1)
    int num_ref_pic_list_in_sps[2]; // ue(v)

    int picture_cropping_flag; // u(1)
    int picture_crop_left_offset; // ue(v)
    int picture_crop_right_offset; // ue(v)
    int picture_crop_top_offset; // ue(v)
    int picture_crop_bottom_offset; // ue(v)

} EVCParserSPS;

typedef struct EVCParserContext {
    ParseContext pc;
    EVCParserSPS sps[MAX_SPS_CNT];
    int is_avc;
    int nal_length_size;
    int to_read;
    int incomplete_nalu_prefix_read; // The flag is set to 1 when an incomplete NAL unit prefix has been read

    int got_sps;
    int got_pps;
    int got_sei;
    int got_slice;
} EVCParserContext;

static int get_nalu_type(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    int unit_type_plus1 = 0;
    XEVD_INFO info;
    int ret;

    if(bs_size >= EVC_NAL_HEADER_SIZE) {
        ret = xevd_info((void *)bs, EVC_NAL_HEADER_SIZE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return -1;
        }
        unit_type_plus1 = info.nalu_type;

    }

    return unit_type_plus1 - 1;
}

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
            av_log(avctx, AV_LOG_ERROR, "Invalid bitstream size!\n");
            return 0;
        }
    }
    
    return len;
}

// @see ISO_IEC_23094-1 (7.3.2.1 SPS RBSP syntax)
static EVCParserSPS *parse_sps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserSPS *sps;
    int sps_seq_parameter_set_id;

    if(init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    sps_seq_parameter_set_id = get_ue_golomb(&gb);

    if(sps_seq_parameter_set_id >= MAX_SPS_CNT)
        return NULL;

    sps = &ev->sps[sps_seq_parameter_set_id];
    sps->sps_seq_parameter_set_id = sps_seq_parameter_set_id;
    
    // the Baseline profile is indicated by profile_idc eqal to 0
    // the Main profile is indicated by profile_idc eqal to 1
    sps->profile_idc = get_bits(&gb, 8);
    
    sps->level_idc = get_bits(&gb, 8);

    skip_bits_long(&gb, 32); /* skip toolset_idc_h */
    skip_bits_long(&gb, 32); /* skip toolset_idc_l */

    // 0 - monochrome
    // 1 - 4:2:0
    // 2 - 4:2:2
    // 3 - 4:4:4
    sps->chroma_format_idc = get_ue_golomb(&gb);

    sps->pic_width_in_luma_samples = get_ue_golomb(&gb);
    sps->pic_height_in_luma_samples = get_ue_golomb(&gb);

    sps->bit_depth_luma_minus8 = get_ue_golomb(&gb);
    sps->bit_depth_chroma_minus8 = get_ue_golomb(&gb);

    sps->sps_btt_flag = get_bits(&gb, 1);
    if(sps->sps_btt_flag) {
        sps->log2_ctu_size_minus5 = get_ue_golomb(&gb);
        sps->log2_min_cb_size_minus2 = get_ue_golomb(&gb);
        sps->log2_diff_ctu_max_14_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_ctu_max_tt_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_min_cb_min_tt_cb_size_minus2 = get_ue_golomb(&gb);
    }

    sps->sps_suco_flag = get_bits(&gb, 1);
    if(sps->sps_suco_flag) {
        sps->log2_diff_ctu_size_max_suco_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_max_suco_min_suco_cb_size = get_ue_golomb(&gb);
    }

    sps->sps_admvp_flag = get_bits(&gb, 1);
    if(sps->sps_admvp_flag) {
        sps->sps_affine_flag = get_bits(&gb, 1);
        sps->sps_amvr_flag = get_bits(&gb, 1);
        sps->sps_dmvr_flag = get_bits(&gb, 1);
        sps->sps_mmvd_flag = get_bits(&gb, 1);
        sps->sps_hmvp_flag = get_bits(&gb, 1);
    }

    sps->sps_eipd_flag =  get_bits(&gb, 1);
    if(sps->sps_eipd_flag) {
        sps->sps_ibc_flag = get_bits(&gb, 1);
        if(sps->sps_ibc_flag)
            sps->log2_max_ibc_cand_size_minus2 = get_ue_golomb(&gb); 
    }

    sps->sps_cm_init_flag = get_bits(&gb, 1);
    if(sps->sps_cm_init_flag)
        sps->sps_adcc_flag = get_bits(&gb, 1);
    
    sps->sps_iqt_flag = get_bits(&gb, 1);
    if(sps->sps_iqt_flag)
        sps->sps_ats_flag = get_bits(&gb, 1);

    sps->sps_addb_flag = get_bits(&gb, 1);
    sps->sps_alf_flag = get_bits(&gb, 1);
    sps->sps_htdf_flag = get_bits(&gb, 1);
    sps->sps_rpl_flag = get_bits(&gb, 1);
    sps->sps_pocs_flag = get_bits(&gb, 1);
    sps->sps_dquant_flag = get_bits(&gb, 1);
    sps->sps_dra_flag = get_bits(&gb, 1);

    if(sps->sps_pocs_flag)
        sps->log2_max_pic_order_cnt_lsb_minus4 = get_ue_golomb(&gb);

    if(!sps->sps_pocs_flag || !sps->sps_rpl_flag) {
        sps->log2_sub_gop_length = get_ue_golomb(&gb);
        if(sps->log2_sub_gop_length == 0)
            sps->log2_ref_pic_gap_length = get_ue_golomb(&gb);
    }

    return sps;
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

    if (nalu_type == XEVD_NUT_SPS) { // NAL Unit type: SPS (Sequence Parameter Set)
        EVCParserSPS *sps;

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

        // if (sps && sps->bitstream_restriction_flag && avctx->has_b_frames < sps->num_reorder_frames) {
        //     avctx->has_b_frames = sps->num_reorder_frames;
        // }

        ev->got_sps = 1;

    } else if (nalu_type == XEVD_NUT_PPS) // NAL Unit type: PPS (Video Parameter Set)
        ev->got_pps = 1;
    else if(nalu_type == XEVD_NUT_SEI) // NAL unit type: SEI (Supplemental Enhancement Information)
        ev->got_sei = 1;
    else if (nalu_type == XEVD_NUT_IDR || nalu_type == XEVD_NUT_NONIDR) // NAL Unit type: Coded slice of a IDR or non-IDR picture
        ev->got_slice++;
    else {
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
        ev->nal_length_size = XEVD_NAL_UNIT_LENGTH_BYTE;

        next = nal_unit_size + XEVD_NAL_UNIT_LENGTH_BYTE;
        ev->to_read = next;
        if(next < buf_size)
            return next;
        else
            return END_NOT_FOUND;
    } else if(ev->to_read > buf_size) {
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
