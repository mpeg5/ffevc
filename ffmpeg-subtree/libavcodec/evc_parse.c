/*
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

#include "bytestream.h"
#include "get_bits.h"
#include "golomb.h"
#include "evc.h"
#include "evc_parse.h"
#include "mpegutils.h"

// #define EVC_MAX_QP_TABLE_SIZE   58

#define EXTENDED_SAR            255
// #define NUM_CPB                 32

#define NUM_CHROMA_FORMATS      4   // @see ISO_IEC_23094-1 section 6.2 table 2

int ff_evc_get_nalu_type(const uint8_t *bits, int bits_size, void *logctx)
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

uint32_t ff_evc_read_nal_unit_length(const uint8_t *bits, int bits_size, void *logctx)
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
int ff_evc_get_temporal_id(const uint8_t *bits, int bits_size, void *logctx)
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
EVCParserSPS *ff_evc_parse_sps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserSPS *sps;
    int sps_seq_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    sps_seq_parameter_set_id = get_ue_golomb(&gb);

    if (sps_seq_parameter_set_id >= EVC_MAX_SPS_COUNT)
        return NULL;

    if(!ev->sps[sps_seq_parameter_set_id]) {
        if((ev->sps[sps_seq_parameter_set_id] = av_malloc(sizeof(EVCParserSPS))) == NULL)
            return NULL;
    }

    sps = ev->sps[sps_seq_parameter_set_id];
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
EVCParserPPS *ff_evc_parse_pps(const uint8_t *bs, int bs_size, EVCParserContext *ev)
{
    GetBitContext gb;
    EVCParserPPS *pps;

    int pps_pic_parameter_set_id;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return NULL;

    pps_pic_parameter_set_id = get_ue_golomb(&gb);
    if (pps_pic_parameter_set_id > EVC_MAX_PPS_COUNT)
        return NULL;

    if(!ev->pps[pps_pic_parameter_set_id]) {
        if((ev->pps[pps_pic_parameter_set_id] = av_malloc(sizeof(EVCParserSPS))) == NULL)
            return NULL;
    }

    pps = ev->pps[pps_pic_parameter_set_id];

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
EVCParserSliceHeader *ff_evc_parse_slice_header(const uint8_t *bs, int bs_size, EVCParserContext *ev)
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

    if(!ev->slice_header[slice_pic_parameter_set_id]) {
        if((ev->slice_header[slice_pic_parameter_set_id] = av_malloc(sizeof(EVCParserSliceHeader))) == NULL)
            return NULL;
    }

    sh = ev->slice_header[slice_pic_parameter_set_id];

    pps = ev->pps[slice_pic_parameter_set_id];
    if(!pps)
        return NULL;

    sps = ev->sps[slice_pic_parameter_set_id];
    if(!sps)
        return NULL;

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