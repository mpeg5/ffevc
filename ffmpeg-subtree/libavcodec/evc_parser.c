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

#define EVC_MAX_QP_TABLE_SIZE   58

#define EXTENDED_SAR            255
#define NUM_CPB                 32

// rpl structure
typedef struct RefPicListStruct {
    int poc;
    int tid;
    int ref_pic_num;
    int ref_pic_active_num;
    int ref_pics[EVC_MAX_NUM_REF_PICS];
    char pic_type;

} RefPicListStruct;

// chromaQP table structure to be signalled in SPS
typedef struct ChromaQpTable {
    int chroma_qp_table_present_flag;       // u(1)
    int same_qp_table_for_chroma;           // u(1)
    int global_offset_flag;                 // u(1)
    int num_points_in_qp_table_minus1[2];   // ue(v)
    int delta_qp_in_val_minus1[2][EVC_MAX_QP_TABLE_SIZE];   // u(6)
    int delta_qp_out_val[2][EVC_MAX_QP_TABLE_SIZE];         // se(v)
} ChromaQpTable;

// Hypothetical Reference Decoder (HRD) parameters, part of VUI
typedef struct HRDParameters {
    int cpb_cnt_minus1;                             // ue(v)
    int bit_rate_scale;                             // u(4)
    int cpb_size_scale;                             // u(4)
    int bit_rate_value_minus1[NUM_CPB];             // ue(v)
    int cpb_size_value_minus1[NUM_CPB];             // ue(v)
    int cbr_flag[NUM_CPB];                          // u(1)
    int initial_cpb_removal_delay_length_minus1;    // u(5)
    int cpb_removal_delay_length_minus1;            // u(5)
    int dpb_output_delay_length_minus1;             // u(5)
    int time_offset_length;                         // u(5)
} HRDParameters;

// video usability information (VUI) part of SPS
typedef struct VUIParameters {
    int aspect_ratio_info_present_flag;             // u(1)
    int aspect_ratio_idc;                           // u(8)
    int sar_width;                                  // u(16)
    int sar_height;                                 // u(16)
    int overscan_info_present_flag;                 // u(1)
    int overscan_appropriate_flag;                  // u(1)
    int video_signal_type_present_flag;             // u(1)
    int video_format;                               // u(3)
    int video_full_range_flag;                      // u(1)
    int colour_description_present_flag;            // u(1)
    int colour_primaries;                           // u(8)
    int transfer_characteristics;                   // u(8)
    int matrix_coefficients;                        // u(8)
    int chroma_loc_info_present_flag;               // u(1)
    int chroma_sample_loc_type_top_field;           // ue(v)
    int chroma_sample_loc_type_bottom_field;        // ue(v)
    int neutral_chroma_indication_flag;             // u(1)
    int field_seq_flag;                             // u(1)
    int timing_info_present_flag;                   // u(1)
    int num_units_in_tick;                          // u(32)
    int time_scale;                                 // u(32)
    int fixed_pic_rate_flag;                        // u(1)
    int nal_hrd_parameters_present_flag;            // u(1)
    int vcl_hrd_parameters_present_flag;            // u(1)
    int low_delay_hrd_flag;                         // u(1)
    int pic_struct_present_flag;                    // u(1)
    int bitstream_restriction_flag;                 // u(1)
    int motion_vectors_over_pic_boundaries_flag;    // u(1)
    int max_bytes_per_pic_denom;                    // ue(v)
    int max_bits_per_mb_denom;                      // ue(v)
    int log2_max_mv_length_horizontal;              // ue(v)
    int log2_max_mv_length_vertical;                // ue(v)
    int num_reorder_pics;                           // ue(v)
    int max_dec_pic_buffering;                      // ue(v)

    HRDParameters hrd_parameters;
} VUIParameters;

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
    struct RefPicListStruct rpls[2][EVC_MAX_NUM_RPLS];

    int picture_cropping_flag;      // u(1)
    int picture_crop_left_offset;   // ue(v)
    int picture_crop_right_offset;  // ue(v)
    int picture_crop_top_offset;    // ue(v)
    int picture_crop_bottom_offset; // ue(v)

    struct ChromaQpTable chroma_qp_table_struct;

    int vui_parameters_present_flag;    // u(1)

    struct VUIParameters vui_parameters;

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
// u(n)  - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
// u(n)  - unsigned integer using n bits.
//         When n is "v" in the syntax table, the number of bits varies in a manner dependent on the value of other syntax elements.
typedef struct EVCParserSliceHeader {
    int slice_pic_parameter_set_id;                                     // ue(v)
    int single_tile_in_slice_flag;                                      // u(1)
    int first_tile_id;                                                  // u(v)
    int arbitrary_slice_flag;                                           // u(1)
    int last_tile_id;                                                   // u(v)
    int num_remaining_tiles_in_slice_minus1;                            // ue(v)
    int delta_tile_id_minus1[EVC_MAX_TILE_ROWS * EVC_MAX_TILE_COLUMNS]; // ue(v)

    int slice_type;                                                     // ue(v)
    int no_output_of_prior_pics_flag;                                   // u(1)
    int mmvd_group_enable_flag;                                         // u(1)
    int slice_alf_enabled_flag;                                         // u(1)

    int slice_alf_luma_aps_id;                                          // u(5)
    int slice_alf_map_flag;                                             // u(1)
    int slice_alf_chroma_idc;                                           // u(2)
    int slice_alf_chroma_aps_id;                                        // u(5)
    int slice_alf_chroma_map_flag;                                      // u(1)
    int slice_alf_chroma2_aps_id;                                       // u(5)
    int slice_alf_chroma2_map_flag;                                     // u(1)
    int slice_pic_order_cnt_lsb;                                        // u(v)

    // @note
    // Currently the structure does not reflect the entire Slice Header RBSP layout.
    // It contains only the fields that are necessary to read from the NAL unit all the values
    // necessary for the correct initialization of the AVCodecContext structure.

    // @note
    // If necessary, add the missing fields to the structure to reflect
    // the contents of the entire NAL unit of the SPS type

} EVCParserSliceHeader;

// picture order count of the current picture
typedef struct EVCParserPoc {
    int PicOrderCntVal;     // current picture order count value
    int prevPicOrderCntVal; // the picture order count of the previous Tid0 picture
    int DocOffset;          // the decoding order count of the previous picture
} EVCParserPoc;

typedef struct EVCParserContext {
    ParseContext pc;
    EVCParserSPS sps[EVC_MAX_SPS_COUNT];
    EVCParserPPS pps[EVC_MAX_PPS_COUNT];
    EVCParserSliceHeader slice_header[EVC_MAX_PPS_COUNT];

    int to_read;    // number of bytes of NAL Unit that do not fit into the current input data chunk and must be read from the new chunk(s)
    int bytes_read; // number of bytes of the current Access Unit that already has been read

    int incomplete_nalu_prefix_read; // the flag is set to 1 when the current input data chunk contains an incomplete NAL unit prefix
    int incomplete_nalu_read;        // the flag is set to 1 when the current input data chunk contains an incomplete NAL unit (more input data is needed to read complete NAL unit)

    int nuh_temporal_id;            // the value of TemporalId (shall be the same for all VCL NAL units of an Access Unit)

    int nalu_prefix_assembled;      // the flag is set to when NALU prefix has been assembled from last chunk and current chunk of incoming data
    int nalu_type;                  // the current NALU type
    int nalu_size;                  // the current NALU size
    int time_base;

    EVCParserPoc poc;

    int parsed_extradata;

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

// nuh_temporal_id specifies a temporal identifier for the NAL unit
static int get_temporal_id(const uint8_t *bits, int bits_size, AVCodecContext *avctx)
{
    int temporal_id = 0;
    short t = 0;

    if (bits_size >= EVC_NALU_HEADER_SIZE) {
        unsigned char *p = (unsigned char *)bits;
        // forbidden_zero_bit
        if ((p[0] & 0x80) != 0)
            return -1;

        for (int i = 0; i < EVC_NALU_HEADER_SIZE; i++)
            t = (t << 8) | p[i];

        temporal_id = (t >> 6) & 0x0007;
    }

    return temporal_id;
}

// @see ISO_IEC_23094-1 (7.3.7 Reference picture list structure syntax)
static int ref_pic_list_struct(GetBitContext *gb, RefPicListStruct *rpl)
{
    uint32_t delta_poc_st, strp_entry_sign_flag = 0;
    rpl->ref_pic_num = get_ue_golomb(gb);
    if (rpl->ref_pic_num > 0) {
        delta_poc_st = get_ue_golomb(gb);

        rpl->ref_pics[0] = delta_poc_st;
        if (rpl->ref_pics[0] != 0) {
            strp_entry_sign_flag = get_bits(gb, 1);

            rpl->ref_pics[0] *= 1 - (strp_entry_sign_flag << 1);
        }
    }

    for (int i = 1; i < rpl->ref_pic_num; ++i) {
        delta_poc_st = get_ue_golomb(gb);
        if (delta_poc_st != 0)
            strp_entry_sign_flag = get_bits(gb, 1);
        rpl->ref_pics[i] = rpl->ref_pics[i - 1] + delta_poc_st * (1 - (strp_entry_sign_flag << 1));
    }

    return 0;
}

// @see  ISO_IEC_23094-1 (E.2.2 HRD parameters syntax)
static int hrd_parameters(GetBitContext *gb, HRDParameters *hrd)
{
    hrd->cpb_cnt_minus1 = get_ue_golomb(gb);
    hrd->bit_rate_scale = get_bits(gb, 4);
    hrd->cpb_size_scale = get_bits(gb, 4);
    for (int SchedSelIdx = 0; SchedSelIdx <= hrd->cpb_cnt_minus1; SchedSelIdx++) {
        hrd->bit_rate_value_minus1[SchedSelIdx] = get_ue_golomb(gb);
        hrd->cpb_size_value_minus1[SchedSelIdx] = get_ue_golomb(gb);
        hrd->cbr_flag[SchedSelIdx] = get_bits(gb, 1);
    }
    hrd->initial_cpb_removal_delay_length_minus1 = get_bits(gb, 5);
    hrd->cpb_removal_delay_length_minus1 = get_bits(gb, 5);
    hrd->cpb_removal_delay_length_minus1 = get_bits(gb, 5);
    hrd->time_offset_length = get_bits(gb, 5);

    return 0;
}

// @see  ISO_IEC_23094-1 (E.2.1 VUI parameters syntax)
static int vui_parameters(GetBitContext *gb, VUIParameters *vui)
{
    vui->aspect_ratio_info_present_flag = get_bits(gb, 1);
    if (vui->aspect_ratio_info_present_flag) {
        vui->aspect_ratio_idc = get_bits(gb, 8);
        if (vui->aspect_ratio_idc == EXTENDED_SAR) {
            vui->sar_width = get_bits(gb, 16);
            vui->sar_height = get_bits(gb, 16);
        }
    }
    vui->overscan_info_present_flag = get_bits(gb, 1);
    if (vui->overscan_info_present_flag)
        vui->overscan_appropriate_flag = get_bits(gb, 1);
    vui->video_signal_type_present_flag = get_bits(gb, 1);
    if (vui->video_signal_type_present_flag) {
        vui->video_format = get_bits(gb, 3);
        vui->video_full_range_flag = get_bits(gb, 1);
        vui->colour_description_present_flag = get_bits(gb, 1);
        if (vui->colour_description_present_flag) {
            vui->colour_primaries = get_bits(gb, 8);
            vui->transfer_characteristics = get_bits(gb, 8);
            vui->matrix_coefficients = get_bits(gb, 8);
        }
    }
    vui->chroma_loc_info_present_flag = get_bits(gb, 1);
    if (vui->chroma_loc_info_present_flag) {
        vui->chroma_sample_loc_type_top_field = get_ue_golomb(gb);
        vui->chroma_sample_loc_type_bottom_field = get_ue_golomb(gb);
    }
    vui->neutral_chroma_indication_flag = get_bits(gb, 1);

    vui->field_seq_flag = get_bits(gb, 1);

    vui->timing_info_present_flag = get_bits(gb, 1);
    if (vui->timing_info_present_flag) {
        vui->num_units_in_tick = get_bits(gb, 32);
        vui->time_scale = get_bits(gb, 32);
        vui->fixed_pic_rate_flag = get_bits(gb, 1);
    }
    vui->nal_hrd_parameters_present_flag = get_bits(gb, 1);
    if (vui->nal_hrd_parameters_present_flag)
        hrd_parameters(gb, &vui->hrd_parameters);
    vui->vcl_hrd_parameters_present_flag = get_bits(gb, 1);
    if (vui->vcl_hrd_parameters_present_flag)
        hrd_parameters(gb, &vui->hrd_parameters);
    if (vui->nal_hrd_parameters_present_flag || vui->vcl_hrd_parameters_present_flag)
        vui->low_delay_hrd_flag = get_bits(gb, 1);
    vui->pic_struct_present_flag = get_bits(gb, 1);
    vui->bitstream_restriction_flag = get_bits(gb, 1);
    if (vui->bitstream_restriction_flag) {
        vui->motion_vectors_over_pic_boundaries_flag = get_bits(gb, 1);
        vui->max_bytes_per_pic_denom = get_ue_golomb(gb);
        vui->max_bits_per_mb_denom = get_ue_golomb(gb);
        vui->log2_max_mv_length_horizontal = get_ue_golomb(gb);
        vui->log2_max_mv_length_vertical = get_ue_golomb(gb);
        vui->num_reorder_pics = get_ue_golomb(gb);
        vui->max_dec_pic_buffering = get_ue_golomb(gb);
    }

    return 0;
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

    if (!sps->sps_rpl_flag)
        sps->max_num_tid0_ref_pics = get_ue_golomb(&gb);
    else {
        sps->sps_max_dec_pic_buffering_minus1 = get_ue_golomb(&gb);
        sps->long_term_ref_pic_flag = get_bits(&gb, 1);
        sps->rpl1_same_as_rpl0_flag = get_bits(&gb, 1);
        sps->num_ref_pic_list_in_sps[0] = get_ue_golomb(&gb);

        for (int i = 0; i < sps->num_ref_pic_list_in_sps[0]; ++i)
            ref_pic_list_struct(&gb, &sps->rpls[0][i]);

        if (!sps->rpl1_same_as_rpl0_flag) {
            sps->num_ref_pic_list_in_sps[1] = get_ue_golomb(&gb);
            for (int i = 0; i < sps->num_ref_pic_list_in_sps[1]; ++i)
                ref_pic_list_struct(&gb, &sps->rpls[1][i]);
        }
    }

    sps->picture_cropping_flag = get_bits(&gb, 1);

    if (sps->picture_cropping_flag) {
        sps->picture_crop_left_offset = get_ue_golomb(&gb);
        sps->picture_crop_right_offset = get_ue_golomb(&gb);
        sps->picture_crop_top_offset = get_ue_golomb(&gb);
        sps->picture_crop_bottom_offset = get_ue_golomb(&gb);
    }

    if (sps->chroma_format_idc != 0) {
        sps->chroma_qp_table_struct.chroma_qp_table_present_flag = get_bits(&gb, 1);

        if (sps->chroma_qp_table_struct.chroma_qp_table_present_flag) {
            sps->chroma_qp_table_struct.same_qp_table_for_chroma = get_bits(&gb, 1);
            sps->chroma_qp_table_struct.global_offset_flag = get_bits(&gb, 1);
            for (int i = 0; i < (sps->chroma_qp_table_struct.same_qp_table_for_chroma ? 1 : 2); i++) {
                sps->chroma_qp_table_struct.num_points_in_qp_table_minus1[i] = get_ue_golomb(&gb);;
                for (int j = 0; j <= sps->chroma_qp_table_struct.num_points_in_qp_table_minus1[i]; j++) {
                    sps->chroma_qp_table_struct.delta_qp_in_val_minus1[i][j] = get_bits(&gb, 6);
                    sps->chroma_qp_table_struct.delta_qp_out_val[i][j] = get_se_golomb(&gb);
                }
            }
        }
    }

    sps->vui_parameters_present_flag = get_bits(&gb, 1);
    if (sps->vui_parameters_present_flag)
        vui_parameters(&gb, &(sps->vui_parameters));

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
    EVCParserSPS *sps;

    int num_tiles_in_slice = 0;
    int slice_pic_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    slice_pic_parameter_set_id = get_ue_golomb(&gb);

    if (slice_pic_parameter_set_id < 0 || slice_pic_parameter_set_id >= EVC_MAX_PPS_COUNT)
        return NULL;

    sh = &ev->slice_header[slice_pic_parameter_set_id];
    pps = &ev->pps[slice_pic_parameter_set_id];
    sps = &ev->sps[slice_pic_parameter_set_id];

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

    if (ev->nalu_type == EVC_IDR_NUT)
        sh->no_output_of_prior_pics_flag = get_bits(&gb, 1);

    if (sps->sps_mmvd_flag && ((sh->slice_type == EVC_SLICE_TYPE_B) || (sh->slice_type == EVC_SLICE_TYPE_P)))
        sh->mmvd_group_enable_flag = get_bits(&gb, 1);
    else
        sh->mmvd_group_enable_flag = 0;

    if (sps->sps_alf_flag) {
        int ChromaArrayType = sps->chroma_format_idc;

        sh->slice_alf_enabled_flag = get_bits(&gb, 1);

        if (sh->slice_alf_enabled_flag) {
            sh->slice_alf_luma_aps_id = get_bits(&gb, 5);
            sh->slice_alf_map_flag = get_bits(&gb, 1);
            sh->slice_alf_chroma_idc = get_bits(&gb, 2);

            if ((ChromaArrayType == 1 || ChromaArrayType == 2) && sh->slice_alf_chroma_idc > 0)
                sh->slice_alf_chroma_aps_id =  get_bits(&gb, 5);
        }
        if (ChromaArrayType == 3) {
            int sliceChromaAlfEnabledFlag = 0;
            int sliceChroma2AlfEnabledFlag = 0;

            if (sh->slice_alf_chroma_idc == 1) { // @see ISO_IEC_23094-1 (7.4.5)
                sliceChromaAlfEnabledFlag = 1;
                sliceChroma2AlfEnabledFlag = 0;
            } else if (sh->slice_alf_chroma_idc == 2) {
                sliceChromaAlfEnabledFlag = 0;
                sliceChroma2AlfEnabledFlag = 1;
            } else if (sh->slice_alf_chroma_idc == 3) {
                sliceChromaAlfEnabledFlag = 1;
                sliceChroma2AlfEnabledFlag = 1;
            } else {
                sliceChromaAlfEnabledFlag = 0;
                sliceChroma2AlfEnabledFlag = 0;
            }

            if (!sh->slice_alf_enabled_flag)
                sh->slice_alf_chroma_idc = get_bits(&gb, 2);

            if (sliceChromaAlfEnabledFlag) {
                sh->slice_alf_chroma_aps_id = get_bits(&gb, 5);
                sh->slice_alf_chroma_map_flag = get_bits(&gb, 1);
            }

            if (sliceChroma2AlfEnabledFlag) {
                sh->slice_alf_chroma2_aps_id = get_bits(&gb, 5);
                sh->slice_alf_chroma2_map_flag = get_bits(&gb, 1);
            }
        }
    }

    if (ev->nalu_type != EVC_IDR_NUT) {
        if (sps->sps_pocs_flag)
            sh->slice_pic_order_cnt_lsb = get_bits(&gb, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    // @note
    // If necessary, add the missing fields to the EVCParserSliceHeader structure
    // and then extend parser implementation

    return sh;
}

static int parse_nal_unit(AVCodecParserContext *s, const uint8_t *buf,
                          int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ev = s->priv_data;
    int nalu_type, nalu_size;
    int tid;
    const uint8_t *data = buf;
    int data_size = buf_size;

    s->picture_structure = AV_PICTURE_STRUCTURE_FRAME;
    s->key_frame = -1;


    nalu_size = buf_size;
    if (nalu_size <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit size: (%d)\n", nalu_size);
        return AVERROR_INVALIDDATA;
    }

    // @see ISO_IEC_23094-1_2020, 7.4.2.2 NAL unit header semantic (Table 4 - NAL unit type codes and NAL unit type classes)
    // @see enum EVCNALUnitType in evc.h
    nalu_type = get_nalu_type(data, data_size, avctx);
    if (nalu_type < EVC_NOIDR_NUT || nalu_type > EVC_UNSPEC_NUT62) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit type: (%d)\n", nalu_type);
        return AVERROR_INVALIDDATA;
    }
    ev->nalu_type = nalu_type;

    tid = get_temporal_id(data, data_size, avctx);
    if (tid < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid temporial id: (%d)\n", tid);
        return AVERROR_INVALIDDATA;
    }
    ev->nuh_temporal_id = tid;

    if (data_size < nalu_size) {
        av_log(avctx, AV_LOG_ERROR, "NAL unit does not fit in the data buffer\n");
        return AVERROR_INVALIDDATA;
    }

    data += EVC_NALU_HEADER_SIZE;
    data_size -= EVC_NALU_HEADER_SIZE;

    if (nalu_type == EVC_SPS_NUT) {
        EVCParserSPS *sps;
        int SubGopLength;

        sps = parse_sps(data, nalu_size, ev);
        if (!sps) {
            av_log(avctx, AV_LOG_ERROR, "SPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        s->coded_width         = sps->pic_width_in_luma_samples;
        s->coded_height        = sps->pic_height_in_luma_samples;
        s->width               = sps->pic_width_in_luma_samples  - sps->picture_crop_left_offset - sps->picture_crop_right_offset;
        s->height              = sps->pic_height_in_luma_samples - sps->picture_crop_top_offset  - sps->picture_crop_bottom_offset;

        SubGopLength = (int)pow(2.0, sps->log2_sub_gop_length);
        avctx->gop_size = SubGopLength;

        avctx->delay = (sps->sps_max_dec_pic_buffering_minus1) ? sps->sps_max_dec_pic_buffering_minus1 - 1 : SubGopLength + sps->max_num_tid0_ref_pics - 1;

        if (sps->profile_idc == 1) avctx->profile = FF_PROFILE_EVC_MAIN;
        else avctx->profile = FF_PROFILE_EVC_BASELINE;

        ev->time_base = avctx->time_base.den;

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

        pps = parse_pps(data, nalu_size, ev);
        if (!pps) {
            av_log(avctx, AV_LOG_ERROR, "PPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }
    } else if (nalu_type == EVC_SEI_NUT)  // Supplemental Enhancement Information
        return 0;
    else if (nalu_type == EVC_APS_NUT)   // Adaptation parameter set
        return 0;
    else if (nalu_type == EVC_FD_NUT)   /* Filler data */
        return 0;
    else if (nalu_type == EVC_IDR_NUT || nalu_type == EVC_NOIDR_NUT) { // Coded slice of a IDR or non-IDR picture
        EVCParserSliceHeader *sh;
        EVCParserSPS *sps;
        int slice_pic_parameter_set_id;

        sh = parse_slice_header(data, nalu_size, ev);
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

        // POC (picture order count of the current picture) derivation
        // @see ISO/IEC 23094-1:2020(E) 8.3.1 Decoding process for picture order count
        slice_pic_parameter_set_id = sh->slice_pic_parameter_set_id;
        sps = &ev->sps[slice_pic_parameter_set_id];

        if (sps->sps_pocs_flag) {

            int PicOrderCntMsb = 0;
            ev->poc.prevPicOrderCntVal = ev->poc.PicOrderCntVal;

            if (nalu_type == EVC_IDR_NUT)
                PicOrderCntMsb = 0;
            else {
                int MaxPicOrderCntLsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);

                int prevPicOrderCntLsb = ev->poc.PicOrderCntVal & (MaxPicOrderCntLsb - 1);
                int prevPicOrderCntMsb = ev->poc.PicOrderCntVal - prevPicOrderCntLsb;


                if ((sh->slice_pic_order_cnt_lsb < prevPicOrderCntLsb) &&
                    ((prevPicOrderCntLsb - sh->slice_pic_order_cnt_lsb) >= (MaxPicOrderCntLsb / 2)))

                    PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;

                else if ((sh->slice_pic_order_cnt_lsb > prevPicOrderCntLsb) &&
                         ((sh->slice_pic_order_cnt_lsb - prevPicOrderCntLsb) > (MaxPicOrderCntLsb / 2)))

                    PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;

                else
                    PicOrderCntMsb = prevPicOrderCntMsb;
            }
            ev->poc.PicOrderCntVal = PicOrderCntMsb + sh->slice_pic_order_cnt_lsb;

        } else {
            if (nalu_type == EVC_IDR_NUT) {
                ev->poc.PicOrderCntVal = 0;
                ev->poc.DocOffset = -1;
            } else {
                int SubGopLength = (int)pow(2.0, sps->log2_sub_gop_length);
                if (tid == 0) {
                    ev->poc.PicOrderCntVal = ev->poc.prevPicOrderCntVal + SubGopLength;
                    ev->poc.DocOffset = 0;
                    ev->poc.prevPicOrderCntVal = ev->poc.PicOrderCntVal;
                } else {
                    int ExpectedTemporalId;
                    int PocOffset;
                    int prevDocOffset = ev->poc.DocOffset;

                    ev->poc.DocOffset = (prevDocOffset + 1) % SubGopLength;
                    if (ev->poc.DocOffset == 0) {
                        ev->poc.prevPicOrderCntVal += SubGopLength;
                        ExpectedTemporalId = 0;
                    } else
                        ExpectedTemporalId = 1 + (int)log2(ev->poc.DocOffset);
                    while (tid != ExpectedTemporalId) {
                        ev->poc.DocOffset = (ev->poc.DocOffset + 1) % SubGopLength;
                        if (ev->poc.DocOffset == 0)
                            ExpectedTemporalId = 0;
                        else
                            ExpectedTemporalId = 1 + (int)log2(ev->poc.DocOffset);
                    }
                    PocOffset = (int)(SubGopLength * ((2.0 * ev->poc.DocOffset + 1) / (int)pow(2.0, tid) - 2));
                    ev->poc.PicOrderCntVal = ev->poc.prevPicOrderCntVal + PocOffset;
                }
            }
        }

        s->output_picture_number = ev->poc.PicOrderCntVal;
        s->key_frame = (nalu_type == EVC_IDR_NUT) ? 1 : 0;

        return 0;
    }
    data += (nalu_size - EVC_NALU_HEADER_SIZE);
    data_size -= (nalu_size - EVC_NALU_HEADER_SIZE);

    return 0;
}

// Reconstruct NAL Unit from incomplete data
//
// Assemble the NALU prefix storing NALU length if it has been split between 2 subsequent buffers (input chunks) incoming to the parser.
// This is the case when the buffer size is not enough for the buffer to store the whole NAL unit prefix.
// In this case, we have to get part of the prefix from the previous buffer and assemble it with the rest from the current buffer.
// Then we'll be able to read NAL unit size.
static int evc_assemble_nalu_prefix(AVCodecParserContext *s, const uint8_t *data, int data_size,
                                    uint8_t *nalu_prefix, AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;
    ParseContext     *pc = &ctx->pc;

    // 1. pc->buffer contains previously read bytes of NALU prefix
    // 2. buf contains the rest of NAL unit prefix bytes

    for (int i = 0; i < EVC_NALU_LENGTH_PREFIX_SIZE; i++) {
        if (i < pc->index)
            nalu_prefix[i] = pc->buffer[i];
        else
            nalu_prefix[i] = data[i - pc->index];
    }

    return 0;
}

// Reconstruct NALU from incomplete data
// Assemble NALU if it is split between multiple buffers
//
// This is the case when the buffer size is not enough to store the entire NAL unit.
// In this scenario, we must retrieve parts of the NALU from the previous buffers stored in pc->buffer and assemble them with the remainder from the current buffer.
static int evc_assemble_nalu(AVCodecParserContext *s, const uint8_t *data, int data_size,
                             uint8_t *nalu, int nalu_size,
                             AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;
    ParseContext     *pc = &ctx->pc;

    // 1. pc->buffer contains previously read bytes of the current NALU and previous NALUs that belong to the current Access Unit.
    //
    //    - previously read bytes are data that came with the previous incoming data chunks.
    //
    //    - pc->buffer contains bytes of the current NALU that have already been read while processing previous chunks of incoming data,
    //      as well as already read bytes of previous NALUs belonging to the same Access Unit.
    //
    //    - ctx->bytes_read is the index of the the first element of the current NALU int the pc->buffer.
    //    - The pc->index is the index of the element located right next to the last element of the current NALU in the pc->buffer.
    //    - The elements of pc->buffer located before ctx->bytes_read index contain previously read NALUs of the current Access Unit.
    //
    // 2. buf contains the rest of the NAL unit bytestime_base
    //
    //    - ctx->to_read number of bytes to read from buf (the index of the element right next to the last element to read)

    uint8_t *prev_data = pc->buffer + ctx->bytes_read;
    int prev_data_size = pc->index - ctx->bytes_read;

    memcpy(nalu, prev_data, prev_data_size);
    memcpy(nalu + prev_data_size, data, data_size);

    return 0;
}

static int end_of_access_unit_found(AVCodecParserContext *s, AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;

    if (avctx->profile == 0) { // BASELINE profile
        if (ctx->nalu_type == EVC_NOIDR_NUT || ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    } else { // MAIN profile
        EVCParserContext *ev = s->priv_data;
        if (ctx->nalu_type == EVC_NOIDR_NUT) {
            if (ev->poc.PicOrderCntVal != ev->poc.prevPicOrderCntVal)
                return 1;
        } else if (ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    }
    return 0;
}

// Find the end of the current frame in the bitstream.
// The end of frame is the end of Access Unit.
// Function returns the position of the first byte of the next frame, or END_NOT_FOUND
static int evc_find_frame_end(AVCodecParserContext *s, const uint8_t *buf,
                              int buf_size, AVCodecContext *avctx)
{
    EVCParserContext *ctx = s->priv_data;

    const uint8_t *data = buf;
    int data_size = buf_size;

    while (data_size > 0) {

        if (ctx->to_read == 0) {
            // Nothing must be read and appended to the data from previous chunks.
            // The previous chunk of data provided the complete NALU prefix or provided the complete NALU.

            if (ctx->nalu_prefix_assembled)   // NALU prefix has been assembled from previous and current chunks of incoming data
                ctx->nalu_prefix_assembled = 0;
            else { // Buffer size is not enough for buffer to store NAL unit 4-bytes prefix (length)
                if (data_size < EVC_NALU_LENGTH_PREFIX_SIZE) {
                    ctx->to_read = EVC_NALU_LENGTH_PREFIX_SIZE - data_size;
                    ctx->incomplete_nalu_prefix_read = 1;
                    return END_NOT_FOUND;
                }

                ctx->nalu_size = read_nal_unit_length(data, data_size, avctx);
                ctx->bytes_read += EVC_NALU_LENGTH_PREFIX_SIZE;

                data += EVC_NALU_LENGTH_PREFIX_SIZE;
                data_size -= EVC_NALU_LENGTH_PREFIX_SIZE;
            }

            if (data_size < ctx->nalu_size) {

                ctx->to_read = ctx->nalu_size - data_size;
                ctx->incomplete_nalu_read = 1;
                return END_NOT_FOUND;
            }

            // the entire NALU can be read
            if (parse_nal_unit(s, data, ctx->nalu_size, avctx) != 0) {
                av_log(avctx, AV_LOG_ERROR, "Parsing of NAL unit failed\n");
                return AVERROR_INVALIDDATA;
            }

            data += ctx->nalu_size;
            data_size -= ctx->nalu_size;

            ctx->bytes_read += ctx->nalu_size;

            if (end_of_access_unit_found(s, avctx)) {

                // parser should return buffer that contains complete AU
                int read_bytes = ctx->bytes_read;
                ctx->bytes_read = 0;
                return read_bytes;
            }

            // go to the next iteration
            continue;

        } else {
            // The previous chunk of input data did not contain the complete valid NALU prefix or did not contain the complete NALU.
            //
            // Missing data must be read from the current data chunk and merged with the data from the previous data chunk
            // to assemble a complete  NALU or complete NALU prefix.
            //
            // The data from the previous data chunk are stored in pc->buf

            if (ctx->to_read < data_size) {

                if (ctx->incomplete_nalu_prefix_read == 1) {

                    uint8_t nalu_prefix[EVC_NALU_LENGTH_PREFIX_SIZE];
                    evc_assemble_nalu_prefix(s, data, data_size, nalu_prefix, avctx);

                    ctx->nalu_size = read_nal_unit_length(nalu_prefix, EVC_NALU_LENGTH_PREFIX_SIZE, avctx);

                    // update variable storing amout of read bytes for teh current AU
                    ctx->bytes_read += ctx->to_read;

                    // update data pointer and data size
                    data += ctx->to_read;
                    data_size -= ctx->to_read;

                    // reset variable storing amount of bytes to read from the new data chunk
                    ctx->to_read = 0;

                    ctx->incomplete_nalu_prefix_read = 0;
                    ctx->nalu_prefix_assembled = 1;

                    continue;
                }
                if (ctx->incomplete_nalu_read == 1) {

                    uint8_t *nalu = (uint8_t *)av_malloc(ctx->nalu_size);

                    // assemble NAL unit using data from previous data chunks (pc->buffer) and the current one (data)
                    evc_assemble_nalu(s, data, ctx->to_read, nalu, ctx->nalu_size, avctx);

                    if (parse_nal_unit(s, nalu, ctx->nalu_size, avctx) != 0) {
                        av_log(avctx, AV_LOG_ERROR, "Parsing of NAL unit failed\n");
                        return AVERROR_INVALIDDATA;
                    }
                    av_free(nalu);

                    // update variable storing amout of read bytes for teh current AU
                    ctx->bytes_read += ctx->nalu_size;

                    // update data pointer and data size
                    data += ctx->to_read;
                    data_size -= ctx->to_read;

                    ctx->incomplete_nalu_read = 0;

                    if (end_of_access_unit_found(s, avctx)) {

                        // parser should return buffer that contains complete AU
                        int read_bytes = ctx->to_read;

                        ctx->to_read = 0;
                        ctx->bytes_read = 0;

                        return read_bytes;
                    }

                    // reset variable storing amount of bytes to read from the new data chunk
                    ctx->to_read = 0;

                    continue;
                }
            } else {
                // needed more input data to assemble complete valid NAL Unit
                ctx->to_read = ctx->to_read - data_size;
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

    // poutbuf contains just one Access Unit
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
