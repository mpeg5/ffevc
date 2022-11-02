/*
 * EVC format parser
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

#include "libavutil/common.h"
#include "parser.h"
#include "golomb.h"
#include "evc.h"

// rpl structure
typedef struct RefPicListStruct {
    int poc;
    int tid;
    int ref_pic_num;
    int ref_pic_active_num;
    int ref_pics[EVC_MAX_NUM_REF_PICS];
    char pic_type;

} RefPicListStruct;

// The sturcture reflects SPS RBSP(raw byte sequence payload) layout
// @see ISO_IEC_23094-1 section 7.3.2.1
//
// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
typedef struct EVCParserSPS {
    int sps_seq_parameter_set_id;   // ue(v)
    int profile_idc;                // u(8)
    int level_idc;                  // u(8)
    int toolset_idc_h;              // u(32)
    int toolset_idc_l;              // u(32)
    int chroma_format_idc;          // ue(v)
    int pic_width_in_luma_samples;  // ue(v)
    int pic_height_in_luma_samples; // ue(v)
    int bit_depth_luma_minus8;      // ue(v)
    int bit_depth_chroma_minus8;    // ue(v)

    int sps_btt_flag;                           // u(1)
    int log2_ctu_size_minus5;                   // ue(v)
    int log2_min_cb_size_minus2;                // ue(v)
    int log2_diff_ctu_max_14_cb_size;           // ue(v)
    int log2_diff_ctu_max_tt_cb_size;           // ue(v)
    int log2_diff_min_cb_min_tt_cb_size_minus2; // ue(v)

    int sps_suco_flag;                       // u(1)
    int log2_diff_ctu_size_max_suco_cb_size; // ue(v)
    int log2_diff_max_suco_min_suco_cb_size; // ue(v)

    int sps_admvp_flag;     // u(1)
    int sps_affine_flag;    // u(1)
    int sps_amvr_flag;      // u(1)
    int sps_dmvr_flag;      // u(1)
    int sps_mmvd_flag;      // u(1)
    int sps_hmvp_flag;      // u(1)

    int sps_eipd_flag;                 // u(1)
    int sps_ibc_flag;                  // u(1)
    int log2_max_ibc_cand_size_minus2; // ue(v)

    int sps_cm_init_flag; // u(1)
    int sps_adcc_flag;    // u(1)

    int sps_iqt_flag; // u(1)
    int sps_ats_flag; // u(1)

    int sps_addb_flag;   // u(1)
    int sps_alf_flag;    // u(1)
    int sps_htdf_flag;   // u(1)
    int sps_rpl_flag;    // u(1)
    int sps_pocs_flag;   // u(1)
    int sps_dquant_flag; // u(1)
    int sps_dra_flag;    // u(1)

    int log2_max_pic_order_cnt_lsb_minus4; // ue(v)
    int log2_sub_gop_length;               // ue(v)
    int log2_ref_pic_gap_length;           // ue(v)

    int max_num_tid0_ref_pics; // ue(v)

    int sps_max_dec_pic_buffering_minus1; // ue(v)
    int long_term_ref_pic_flag;           // u(1)
    int rpl1_same_as_rpl0_flag;           // u(1)
    int num_ref_pic_list_in_sps[2];       // ue(v)
    RefPicListStruct rpls[2][EVC_MAX_NUM_RPLS];

    int picture_cropping_flag;      // u(1)
    int picture_crop_left_offset;   // ue(v)
    int picture_crop_right_offset;  // ue(v)
    int picture_crop_top_offset;    // ue(v)
    int picture_crop_bottom_offset; // ue(v)

    // @note
    // Currently the structure does not reflect the entire SPS RBSP layout.
    // It contains only the fields that are necessary to read from the NAL unit all the values
    // necessary for the correct initialization of the AVCodecContext structure.

    // @note
    // If necessary, add the missing fields to the structure to reflect
    // the contents of the entire NAL unit of the SPS type

} EVCParserSPS;

typedef struct EVCParserPPS {
    int pps_pic_parameter_set_id;                           // ue(v)
    int pps_seq_parameter_set_id;                           // ue(v)
    int num_ref_idx_default_active_minus1[2];               // ue(v)
    int additional_lt_poc_lsb_len;                          // ue(v)
    int rpl1_idx_present_flag;                              // u(1)
    int single_tile_in_pic_flag;                            // u(1)
    int num_tile_columns_minus1;                            // ue(v)
    int num_tile_rows_minus1;                               // ue(v)
    int uniform_tile_spacing_flag;                          // u(1)
    int tile_column_width_minus1[EVC_MAX_TILE_ROWS];        // ue(v)
    int tile_row_height_minus1[EVC_MAX_TILE_COLUMNS];          // ue(v)
    int loop_filter_across_tiles_enabled_flag;              // u(1)
    int tile_offset_len_minus1;                             // ue(v)
    int tile_id_len_minus1;                                 // ue(v)
    int explicit_tile_id_flag;                              // u(1)
    int tile_id_val[EVC_MAX_TILE_ROWS][EVC_MAX_TILE_COLUMNS];  // u(v)
    int pic_dra_enabled_flag;                               // u(1)
    int pic_dra_aps_id;                                     // u(5)
    int arbitrary_slice_present_flag;                       // u(1)
    int constrained_intra_pred_flag;                        // u(1)
    int cu_qp_delta_enabled_flag;                           // u(1)
    int log2_cu_qp_delta_area_minus6;                       // ue(v)

} EVCParserPPS;

// The sturcture reflects Slice Header RBSP(raw byte sequence payload) layout
// @see ISO_IEC_23094-1 section 7.3.2.6
//
// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
// u(n) - unsigned integer using n bits.
//        When n is "v" in the syntax table, the number of bits varies in a manner dependent on the value of other syntax elements.
typedef struct EVCParserSliceHeader {
    int slice_pic_parameter_set_id;                                     // ue(v)
    int single_tile_in_slice_flag;                                      // u(1)
    int first_tile_id;                                                  // u(v)
    int arbitrary_slice_flag;                                           // u(1)
    int last_tile_id;                                                   // u(v)
    int num_remaining_tiles_in_slice_minus1;                            // ue(v)
    int delta_tile_id_minus1[EVC_MAX_TILE_ROWS * EVC_MAX_TILE_COLUMNS];    // ue(v)

    int slice_type;                                                     // ue(v)

    // @note
    // Currently the structure does not reflect the entire Slice Header RBSP layout.
    // It contains only the fields that are necessary to read from the NAL unit all the values
    // necessary for the correct initialization of the AVCodecContext structure.

    // @note
    // If necessary, add the missing fields to the structure to reflect
    // the contents of the entire NAL unit of the SPS type

} EVCParserSliceHeader;

typedef struct EVCParserContext {
    ParseContext pc;
    EVCParserSPS sps[EVC_MAX_SPS_COUNT];
    EVCParserPPS pps[EVC_MAX_PPS_COUNT];
    EVCParserSliceHeader slice_header[EVC_MAX_PPS_COUNT];
    int to_read; // number of bytes of NAL unit that did not fit into the previous buffer and must be read from the new buffer
    int incomplete_nalu_prefix_read; // The flag is set to 1 when an incomplete NAL unit prefix has been read
    int incomplete_nalu_header_read; // The flag is set to 1 when an incomplete NAL unit header has been read
    int nuh_temporal_id; // the value of TemporalId shall be the same for all VCL NAL units of an access unit

} EVCParserContext;

static int get_nalu_type(const uint8_t *bits, int bits_size, AVCodecContext *avctx)
{
    int unit_type_plus1 = 0;

    if (bits_size >= EVC_NALU_HEADER_SIZE) {
        unsigned char *p = (unsigned char *)bits;
        // forbidden_zero_bit
        if ((p[0] & 0x80) != 0)
            return -1;

        // nal_unit_type
        unit_type_plus1 = (p[0] >> 1) & 0x3F;
    }

    return unit_type_plus1 - 1;
}

static uint32_t read_nal_unit_length(const uint8_t *bits, int bits_size, AVCodecContext *avctx)
{
    uint32_t nalu_len = 0;

    if (bits_size >= EVC_NALU_LENGTH_PREFIX_SIZE) {

        int t = 0;
        unsigned char *p = (unsigned char *)bits;

        for (int i = 0; i < EVC_NALU_LENGTH_PREFIX_SIZE; i++)
            t = (t << 8) | p[i];

        nalu_len = t;
        if (nalu_len == 0)
            return 0;
    }

    return nalu_len;
}

// @see ISO_IEC_23094-1 (7.3.2.1 SPS RBSP syntax)
static EVCParserSPS *parse_sps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserSPS *sps;
    int sps_seq_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    sps_seq_parameter_set_id = get_ue_golomb(&gb);

    if (sps_seq_parameter_set_id >= EVC_MAX_SPS_COUNT)
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
    if (sps->sps_btt_flag) {
        sps->log2_ctu_size_minus5 = get_ue_golomb(&gb);
        sps->log2_min_cb_size_minus2 = get_ue_golomb(&gb);
        sps->log2_diff_ctu_max_14_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_ctu_max_tt_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_min_cb_min_tt_cb_size_minus2 = get_ue_golomb(&gb);
    }

    sps->sps_suco_flag = get_bits(&gb, 1);
    if (sps->sps_suco_flag) {
        sps->log2_diff_ctu_size_max_suco_cb_size = get_ue_golomb(&gb);
        sps->log2_diff_max_suco_min_suco_cb_size = get_ue_golomb(&gb);
    }

    sps->sps_admvp_flag = get_bits(&gb, 1);
    if (sps->sps_admvp_flag) {
        sps->sps_affine_flag = get_bits(&gb, 1);
        sps->sps_amvr_flag = get_bits(&gb, 1);
        sps->sps_dmvr_flag = get_bits(&gb, 1);
        sps->sps_mmvd_flag = get_bits(&gb, 1);
        sps->sps_hmvp_flag = get_bits(&gb, 1);
    }

    sps->sps_eipd_flag =  get_bits(&gb, 1);
    if (sps->sps_eipd_flag) {
        sps->sps_ibc_flag = get_bits(&gb, 1);
        if (sps->sps_ibc_flag)
            sps->log2_max_ibc_cand_size_minus2 = get_ue_golomb(&gb);
    }

    sps->sps_cm_init_flag = get_bits(&gb, 1);
    if (sps->sps_cm_init_flag)
        sps->sps_adcc_flag = get_bits(&gb, 1);

    sps->sps_iqt_flag = get_bits(&gb, 1);
    if (sps->sps_iqt_flag)
        sps->sps_ats_flag = get_bits(&gb, 1);

    sps->sps_addb_flag = get_bits(&gb, 1);
    sps->sps_alf_flag = get_bits(&gb, 1);
    sps->sps_htdf_flag = get_bits(&gb, 1);
    sps->sps_rpl_flag = get_bits(&gb, 1);
    sps->sps_pocs_flag = get_bits(&gb, 1);
    sps->sps_dquant_flag = get_bits(&gb, 1);
    sps->sps_dra_flag = get_bits(&gb, 1);

    if (sps->sps_pocs_flag)
        sps->log2_max_pic_order_cnt_lsb_minus4 = get_ue_golomb(&gb);

    if (!sps->sps_pocs_flag || !sps->sps_rpl_flag) {
        sps->log2_sub_gop_length = get_ue_golomb(&gb);
        if (sps->log2_sub_gop_length == 0)
            sps->log2_ref_pic_gap_length = get_ue_golomb(&gb);
    }

    // @note
    // If necessary, add the missing fields to the EVCParserSPS structure
    // and then extend parser implementation

    return sps;
}

// @see ISO_IEC_23094-1 (7.3.2.2 SPS RBSP syntax)
//
// @note
// The current implementation of parse_sps function doesn't handle VUI parameters parsing.
// If it will be needed, parse_sps function could be extended to handle VUI parameters parsing
// to initialize fields of the AVCodecContex i.e. color_primaries, color_trc,color_range
//
static EVCParserPPS *parse_pps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserPPS *pps;

    int pps_pic_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    pps_pic_parameter_set_id = get_ue_golomb(&gb);
    if (pps_pic_parameter_set_id > EVC_MAX_PPS_COUNT)
        return NULL;

    pps = &ev->pps[pps_pic_parameter_set_id];

    pps->pps_pic_parameter_set_id = pps_pic_parameter_set_id;

    pps->pps_seq_parameter_set_id = get_ue_golomb(&gb);
    if (pps->pps_seq_parameter_set_id >= EVC_MAX_SPS_COUNT)
        return NULL;

    pps->num_ref_idx_default_active_minus1[0] = get_ue_golomb(&gb);
    pps->num_ref_idx_default_active_minus1[1] = get_ue_golomb(&gb);
    pps->additional_lt_poc_lsb_len = get_ue_golomb(&gb);
    pps->rpl1_idx_present_flag = get_bits(&gb, 1);
    pps->single_tile_in_pic_flag = get_bits(&gb, 1);

    if (!pps->single_tile_in_pic_flag) {
        pps->num_tile_columns_minus1 = get_ue_golomb(&gb);
        pps->num_tile_rows_minus1 = get_ue_golomb(&gb);
        pps->uniform_tile_spacing_flag = get_bits(&gb, 1);

        if (!pps->uniform_tile_spacing_flag) {
            for (int i = 0; i < pps->num_tile_columns_minus1; i++)
                pps->tile_column_width_minus1[i] = get_ue_golomb(&gb);

            for (int i = 0; i < pps->num_tile_rows_minus1; i++)
                pps->tile_row_height_minus1[i] = get_ue_golomb(&gb);
        }
        pps->loop_filter_across_tiles_enabled_flag = get_bits(&gb, 1);
        pps->tile_offset_len_minus1 = get_ue_golomb(&gb);
    }

    pps->tile_id_len_minus1 = get_ue_golomb(&gb);
    pps->explicit_tile_id_flag = get_bits(&gb, 1);

    if (pps->explicit_tile_id_flag) {
        for (int i = 0; i <= pps->num_tile_rows_minus1; i++) {
            for (int j = 0; j <= pps->num_tile_columns_minus1; j++)
                pps->tile_id_val[i][j] = get_bits(&gb, pps->tile_id_len_minus1 + 1);
        }
    }

    pps->pic_dra_enabled_flag = 0;
    pps->pic_dra_enabled_flag = get_bits(&gb, 1);

    if (pps->pic_dra_enabled_flag)
        pps->pic_dra_aps_id = get_bits(&gb, 5);

    pps->arbitrary_slice_present_flag = get_bits(&gb, 1);
    pps->constrained_intra_pred_flag = get_bits(&gb, 1);
    pps->cu_qp_delta_enabled_flag = get_bits(&gb, 1);

    if (pps->cu_qp_delta_enabled_flag)
        pps->log2_cu_qp_delta_area_minus6 = get_ue_golomb(&gb);

    return pps;
}

// @see ISO_IEC_23094-1 (7.3.2.6 Slice layer RBSP syntax)
static EVCParserSliceHeader *parse_slice_header(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserSliceHeader *sh;
    EVCParserPPS *pps;
    int num_tiles_in_slice = 0;
    int slice_pic_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    slice_pic_parameter_set_id = get_ue_golomb(&gb);

    if (slice_pic_parameter_set_id < 0 || slice_pic_parameter_set_id >= EVC_MAX_PPS_COUNT)
        return NULL;

    sh = &ev->slice_header[slice_pic_parameter_set_id];
    pps = &ev->pps[slice_pic_parameter_set_id];

    sh->slice_pic_parameter_set_id = slice_pic_parameter_set_id;

    if (!pps->single_tile_in_pic_flag) {
        sh->single_tile_in_slice_flag = get_bits(&gb, 1);
        sh->first_tile_id = get_bits(&gb, pps->tile_id_len_minus1 + 1);
    } else
        sh->single_tile_in_slice_flag = 1;

    if (!sh->single_tile_in_slice_flag) {
        if (pps->arbitrary_slice_present_flag)
            sh->arbitrary_slice_flag = get_bits(&gb, 1);

        if (!sh->arbitrary_slice_flag)
            sh->last_tile_id = get_bits(&gb, pps->tile_id_len_minus1 + 1);
        else {
            sh->num_remaining_tiles_in_slice_minus1 = get_ue_golomb(&gb);
            num_tiles_in_slice = sh->num_remaining_tiles_in_slice_minus1 + 2;
            for (int i = 0; i < num_tiles_in_slice - 1; ++i)
                sh->delta_tile_id_minus1[i] = get_ue_golomb(&gb);
        }
    }

    sh->slice_type = get_ue_golomb(&gb);

    // @note
    // If necessary, add the missing fields to the EVCParserSliceHeader structure
    // and then extend parser implementation

    return sh;
}

/**
 * Parse NAL units of found picture and decode some basic information.
 *
 * @param s parser context.
 * @param avctx codec context.
 * @param buf buffer with field/frame data.
 * @param buf_size size of the buffer.
 */
static int parse_nal_units(AVCodecParserContext *s, const uint8_t *buf,
                           int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ev = s->priv_data;
    int nalu_type, nalu_size;

    s->picture_structure = AV_PICTURE_STRUCTURE_FRAME;
    s->key_frame = -1;

    nalu_size = read_nal_unit_length(buf, buf_size, avctx);
    if (nalu_size <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit size: (%d)\n", nalu_size);
        return AVERROR_INVALIDDATA;
    }

    buf += EVC_NALU_LENGTH_PREFIX_SIZE;
    buf_size -= EVC_NALU_LENGTH_PREFIX_SIZE;

    // @see ISO_IEC_23094-1_2020, 7.4.2.2 NAL unit header semantic (Table 4 - NAL unit type codes and NAL unit type classes)
    // @see enum EVCNALUnitType in evc.h
    nalu_type = get_nalu_type(buf, buf_size, avctx);
    if (nalu_type < EVC_NOIDR_NUT || nalu_type > EVC_UNSPEC_NUT62) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit type: (%d)\n", nalu_type);
        return AVERROR_INVALIDDATA;
    }

    buf += EVC_NALU_HEADER_SIZE;
    buf_size -= EVC_NALU_HEADER_SIZE;

    if (nalu_type == EVC_SPS_NUT) {
        EVCParserSPS *sps;

        sps = parse_sps(buf, buf_size, ev);
        if (!sps) {
            av_log(avctx, AV_LOG_ERROR, "SPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        s->coded_width         = sps->pic_width_in_luma_samples;
        s->coded_height        = sps->pic_height_in_luma_samples;
        s->width               = sps->pic_width_in_luma_samples;
        s->height              = sps->pic_height_in_luma_samples;

        if (sps->profile_idc == 1) avctx->profile = FF_PROFILE_EVC_MAIN;
        else avctx->profile = FF_PROFILE_EVC_BASELINE;

        switch (sps->chroma_format_idc) {
        case 0: /* YCBCR400_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR400_10LE: Not supported chroma format\n");
            s->format = AV_PIX_FMT_GRAY10LE;
            return -1;
        case 1: /* YCBCR420_10LE */
            s->format = AV_PIX_FMT_YUV420P10LE;
            break;
        case 2: /* YCBCR422_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR422_10LE: Not supported chroma format\n");
            s->format = AV_PIX_FMT_YUV422P10LE;
            return -1;
        case 3: /* YCBCR444_10LE */
            av_log(avctx, AV_LOG_ERROR, "YCBCR444_10LE: Not supported chroma format\n");
            s->format = AV_PIX_FMT_YUV444P10LE;
            return -1;
        default:
            s->format = AV_PIX_FMT_NONE;
            av_log(avctx, AV_LOG_ERROR, "Unknown supported chroma format\n");
            return -1;
        }
    } else if (nalu_type == EVC_PPS_NUT) {
        EVCParserPPS *pps;

        pps = parse_pps(buf, buf_size, ev);
        if (!pps) {
            av_log(avctx, AV_LOG_ERROR, "PPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }
    } else if (nalu_type == EVC_SEI_NUT) // Supplemental Enhancement Information
        return 0;
    else if (nalu_type == EVC_IDR_NUT || nalu_type == EVC_NOIDR_NUT) { // Coded slice of a IDR or non-IDR picture
        EVCParserSliceHeader *sh;

        sh = parse_slice_header(buf, buf_size, ev);
        if (!sh) {
            av_log(avctx, AV_LOG_ERROR, "Slice header parsing error\n");
            return AVERROR_INVALIDDATA;
        }
        switch (sh->slice_type) {
        case EVC_SLICE_TYPE_B: {
            s->pict_type =  AV_PICTURE_TYPE_B;
            break;
        }
        case EVC_SLICE_TYPE_P: {
            s->pict_type =  AV_PICTURE_TYPE_P;
            break;
        }
        case EVC_SLICE_TYPE_I: {
            s->pict_type =  AV_PICTURE_TYPE_I;
            break;
        }
        default: {
            s->pict_type =  AV_PICTURE_TYPE_NONE;
        }
        }
        s->key_frame = (nalu_type == EVC_IDR_NUT) ? 1 : 0;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unknown NAL unit type: %d\n", nalu_type);
        return 0;
    }

    return 0;
}

/**
 * @brief Reconstruct length of NALU from incomplete data
 * Assemble NALU prefix if it is split between 2 buffers
 *
 * This is the case when buffer size is not enough for the buffer to store NAL unit prefix.
 * In this case, we have to get part of the prefix from the previous buffer and assemble it with the rest from the current buffer.
 * Then we'll be able to read NAL unit size.
 *
 */
static int evc_assemble_nalu_prefix(AVCodecParserContext *s, const uint8_t *buf,
                              int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;
    ParseContext     *pc = &ctx->pc;

    uint8_t nalu_len[EVC_NALU_LENGTH_PREFIX_SIZE] = {0};
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
    for (int i = 0; i < EVC_NALU_LENGTH_PREFIX_SIZE; i++) {
        if (i < pc->index)
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

    nal_unit_size = read_nal_unit_length(nalu_len, EVC_NALU_LENGTH_PREFIX_SIZE, avctx);

    return nal_unit_size;
}

/**
 * Find the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or END_NOT_FOUND
 */
static int evc_find_frame_end(AVCodecParserContext *s, const uint8_t *buf,
                              int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;
    ParseContext     *pc = &ctx->pc;

    int bytes_read = 0;

    while ((buf_size-bytes_read)>0) {

        if(ctx->to_read == 0) {
            int nal_unit_size = 0;

            // This is the case when buffer size is not enough for buffer to store NAL unit 4-bytes prefix (length)
            if ((buf_size-bytes_read) < EVC_NALU_LENGTH_PREFIX_SIZE) {
                ctx->to_read = EVC_NALU_LENGTH_PREFIX_SIZE - (bytes_read - buf_size);
                ctx->incomplete_nalu_prefix_read  = 1;
                return END_NOT_FOUND;
            }

            nal_unit_size = read_nal_unit_length(buf, buf_size, avctx);
            bytes_read = nal_unit_size + EVC_NALU_LENGTH_PREFIX_SIZE;
            if ((buf_size-bytes_read)<0) {
                ctx->to_read = bytes_read - buf_size;
                return END_NOT_FOUND;
            }
            return bytes_read;
        } else {
            if(ctx->to_read < (buf_size - bytes_read)) {
                int next = ctx->to_read;

                if (ctx->incomplete_nalu_prefix_read  == 1) {
                    int nal_unit_size = evc_assemble_nalu_prefix(s, buf, buf_size,avctx);
                    bytes_read = nal_unit_size + EVC_NALU_LENGTH_PREFIX_SIZE - pc->index;

                    ctx->incomplete_nalu_prefix_read = 0;

                    if ((buf_size - bytes_read)<0) {
                        ctx->to_read = bytes_read - buf_size;
                        return END_NOT_FOUND;
                    }
                    else {
                        ctx->to_read = 0;
                        return bytes_read;
                    }
                }

                ctx->to_read = 0;
                return next;
            } else {
                ctx->to_read = ctx->to_read - (buf_size - bytes_read);
                return END_NOT_FOUND;
            }
        }
    }

    return END_NOT_FOUND;
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
            return buf_size;
        }
    }

    is_dummy_buf &= (dummy_buf == buf);

    if (!is_dummy_buf) {
        if (parse_nal_units(s, buf, buf_size, avctx) == AVERROR_INVALIDDATA) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    // poutbuf contains just one NAL unit
    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

static int evc_parser_init(AVCodecParserContext *s)
{
    EVCParserContext *ev = s->priv_data;

    ev->incomplete_nalu_prefix_read = 0;

    return 0;
}

const AVCodecParser ff_evc_parser = {
    .codec_ids      = { AV_CODEC_ID_EVC },
    .priv_data_size = sizeof(EVCParserContext),
    .parser_init    = evc_parser_init,
    .parser_parse   = evc_parse,
    .parser_close   = ff_parse_close,
};
