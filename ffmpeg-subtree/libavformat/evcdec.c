/*
 * RAW EVC video demuxer
 *
 * Copyright (c) 2021 Dawid Kozinski <d.kozinski@samsung.com>
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

#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/internal.h"
#include "libavcodec/evc.h"
#include "libavcodec/bsf.h"

#include "libavutil/opt.h"

#include "rawdec.h"
#include "avformat.h"
#include "internal.h"


#define RAW_PACKET_SIZE 1024

// The sturcture reflects SPS RBSP(raw byte sequence payload) layout
// @see ISO_IEC_23094-1 section 7.3.2.1
//
// The following descriptors specify the parsing process of each element
// u(n) - unsigned integer using n bits
// ue(v) - unsigned integer 0-th order Exp_Golomb-coded syntax element with the left bit first
typedef struct EVCParserSPS {
    int sps_seq_parameter_set_id;      // ue(v)
    int profile_idc;                   // u(8)

    // int level_idc;                  // u(8)
    // int toolset_idc_h;              // u(32)
    // int toolset_idc_l;              // u(32)

    int chroma_format_idc;             // ue(v)

    // int pic_width_in_luma_samples;  // ue(v)
    // int pic_height_in_luma_samples; // ue(v)
    // int bit_depth_luma_minus8;      // ue(v)
    // int bit_depth_chroma_minus8;    // ue(v)

    int sps_btt_flag;                              // u(1)
    // int log2_ctu_size_minus5;                   // ue(v)
    // int log2_min_cb_size_minus2;                // ue(v)
    // int log2_diff_ctu_max_14_cb_size;           // ue(v)
    // int log2_diff_ctu_max_tt_cb_size;           // ue(v)
    // int log2_diff_min_cb_min_tt_cb_size_minus2; // ue(v)

    int sps_suco_flag;                          // u(1)
    // int log2_diff_ctu_size_max_suco_cb_size; // ue(v)
    // int log2_diff_max_suco_min_suco_cb_size; // ue(v)

    int sps_admvp_flag;         // u(1)
    // int sps_affine_flag;     // u(1)
    // int sps_amvr_flag;       // u(1)
    // int sps_dmvr_flag;       // u(1)
    int sps_mmvd_flag;          // u(1)
    // int sps_hmvp_flag;       // u(1)

    int sps_eipd_flag;                      // u(1)
    int sps_ibc_flag;                       // u(1)
    // int log2_max_ibc_cand_size_minus2;   // ue(v)

    int sps_cm_init_flag; // u(1)
    int sps_adcc_flag;    // u(1)

    int sps_iqt_flag;       // u(1)
    // int sps_ats_flag;    // u(1)

    // int sps_addb_flag;   // u(1)
    int sps_alf_flag;       // u(1)
    // int sps_htdf_flag;   // u(1)
    int sps_rpl_flag;       // u(1)
    int sps_pocs_flag;      // u(1)
    // int sps_dquant_flag; // u(1)
    // int sps_dra_flag;    // u(1)

    int log2_max_pic_order_cnt_lsb_minus4; // ue(v)
    int log2_sub_gop_length;               // ue(v)

    // int log2_ref_pic_gap_length;           // ue(v)

    // int max_num_tid0_ref_pics; // ue(v)

    // int sps_max_dec_pic_buffering_minus1; // ue(v)
    // int long_term_ref_pic_flag;           // u(1)
    // int rpl1_same_as_rpl0_flag;           // u(1)
    // int num_ref_pic_list_in_sps[2];       // ue(v)
    // struct RefPicListStruct rpls[2][EVC_MAX_NUM_RPLS];

    // int picture_cropping_flag;      // u(1)
    // int picture_crop_left_offset;   // ue(v)
    // int picture_crop_right_offset;  // ue(v)
    // int picture_crop_top_offset;    // ue(v)
    // int picture_crop_bottom_offset; // ue(v)

    // struct ChromaQpTable chroma_qp_table_struct;

    // int vui_parameters_present_flag;    // u(1)

    // struct VUIParameters vui_parameters;
} EVCParserSPS;

typedef struct EVCParserPPS {
    int pps_pic_parameter_set_id;                                   // ue(v)
    int pps_seq_parameter_set_id;                                   // ue(v)
    // int num_ref_idx_default_active_minus1[2];                    // ue(v)
    // int additional_lt_poc_lsb_len;                               // ue(v)
    // int rpl1_idx_present_flag;                                   // u(1)
    int single_tile_in_pic_flag;                                    // u(1)
    int num_tile_columns_minus1;                                    // ue(v)
    int num_tile_rows_minus1;                                       // ue(v)
    int uniform_tile_spacing_flag;                                  // u(1)
    // int tile_column_width_minus1[EVC_MAX_TILE_ROWS];             // ue(v)
    // int tile_row_height_minus1[EVC_MAX_TILE_COLUMNS];            // ue(v)
    int loop_filter_across_tiles_enabled_flag;                      // u(1)
    // int tile_offset_len_minus1;                                  // ue(v)
    int tile_id_len_minus1;                                         // ue(v)
    int explicit_tile_id_flag;                                      // u(1)
    // int tile_id_val[EVC_MAX_TILE_ROWS][EVC_MAX_TILE_COLUMNS];    // u(v)
    int pic_dra_enabled_flag;                                       // u(1)
    int pic_dra_aps_id;                                             // u(5)
    int arbitrary_slice_present_flag;                               // u(1)
    // int constrained_intra_pred_flag;                             // u(1)
    // int cu_qp_delta_enabled_flag;                                // u(1)
    // int log2_cu_qp_delta_area_minus6;                            // ue(v)

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
    int slice_pic_parameter_set_id;                                         // ue(v)
    int single_tile_in_slice_flag;                                          // u(1)
    // int first_tile_id;                                                   // u(v)
    int arbitrary_slice_flag;                                               // u(1)
    // int last_tile_id;                                                    // u(v)
    int num_remaining_tiles_in_slice_minus1;                                // ue(v)
    // int delta_tile_id_minus1[EVC_MAX_TILE_ROWS * EVC_MAX_TILE_COLUMNS];  // ue(v)

    int slice_type;                                                         // ue(v)
    // int no_output_of_prior_pics_flag;                                    // u(1)
    // int mmvd_group_enable_flag;                                          // u(1)
    int slice_alf_enabled_flag;                                             // u(1)

    int slice_alf_luma_aps_id;                                              // u(5)
    int slice_alf_map_flag;                                                 // u(1)
    int slice_alf_chroma_idc;                                               // u(2)
    int slice_alf_chroma_aps_id;                                            // u(5)
    // int slice_alf_chroma_map_flag;                                       // u(1)
    // int slice_alf_chroma2_aps_id;                                        // u(5)
    // int slice_alf_chroma2_map_flag;                                      // u(1)
    int slice_pic_order_cnt_lsb;                                            // u(v) OK

    // @note
    // Currently the structure does not reflect the entire Slice Header RBSP layout.
    // It contains only the fields that are necessary to read from the NAL unit all the values
    // necessary for the correct initialization of the AVCodecContext structure.

    // @note
    // If necessary, add the missing fields to the structure to reflect
    // the contents of the entire NAL unit of the SPS type

} EVCParserSliceHeader;


typedef struct EVCParserPoc {
    int PicOrderCntVal;     // current picture order count value
    int prevPicOrderCntVal; // the picture order count of the previous Tid0 picture
    int DocOffset;          // the decoding order count of the previous picture
} EVCParserPoc;

typedef struct EVCParserContext {
    int got_sps;
    int got_pps;
    int got_idr;
    int got_nonidr;

} EVCParserContext;

typedef struct EVCDemuxContext {
    const AVClass *class;
    AVRational framerate;

    int profile;

    EVCParserSPS *sps[EVC_MAX_SPS_COUNT];
    EVCParserPPS *pps[EVC_MAX_PPS_COUNT];
    EVCParserSliceHeader *slice_header[EVC_MAX_PPS_COUNT];
    EVCParserPoc poc;
    int nalu_type;                  // the current NALU type
    int nalu_size;                  // the current NALU size
    int key_frame;

} EVCDemuxContext;

#define DEC AV_OPT_FLAG_DECODING_PARAM
#define OFFSET(x) offsetof(EVCDemuxContext, x)
static const AVOption evc_options[] = {
    { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

static const AVClass evc_demuxer_class = {
    .class_name = "EVC Annex B demuxer",
    .item_name  = av_default_item_name,
    .option     = evc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

// nuh_temporal_id specifies a temporal identifier for the NAL unit
static int get_temporal_id(const uint8_t *bits, int bits_size)
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

static int get_nalu_type(const uint8_t *bits, int bits_size)
{
    int unit_type_plus1 = 0;

    if (bits_size >= EVC_NALU_HEADER_SIZE) {
        unsigned char *p = (unsigned char *)bits;
        // forbidden_zero_bit
        if ((p[0] & 0x80) != 0)   // Cannot get bitstream information. Malformed bitstream.
            return -1;

        // nal_unit_type
        unit_type_plus1 = (p[0] >> 1) & 0x3F;
    }

    return unit_type_plus1 - 1;
}

static uint32_t read_nal_unit_length(const uint8_t *bits, int bits_size)
{
    uint32_t nalu_len = 0;

    if (bits_size >= EVC_NALU_LENGTH_PREFIX_SIZE) {

        int t = 0;
        unsigned char *p = (unsigned char *)bits;

        for (int i = 0; i < EVC_NALU_LENGTH_PREFIX_SIZE; i++)
            t = (t << 8) | p[i];

        nalu_len = t;
        if (nalu_len == 0)   // Invalid bitstream size
            return 0;
    }

    return nalu_len;
}

static int end_of_access_unit_found(const uint8_t *bits, int bits_size, AVFormatContext *s)
{
    EVCDemuxContext *ctx = s->priv_data;

    if (ctx->profile == 0) { // BASELINE profile
        if (ctx->nalu_type == EVC_NOIDR_NUT || ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    } else { // MAIN profile
        if (ctx->nalu_type == EVC_NOIDR_NUT) {
            if (ctx->poc.PicOrderCntVal != ctx->poc.prevPicOrderCntVal)
                return 1;
        } else if (ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    }
    return 0;
}

// @see ISO_IEC_23094-1 (7.3.2.1 SPS RBSP syntax)
static EVCParserSPS *parse_sps(const uint8_t *bs, int bs_size, EVCDemuxContext *ev)
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

    skip_bits_long(&gb, 8); // sps->level_idc = get_bits(&gb, 8);

    skip_bits_long(&gb, 32); /* skip toolset_idc_h */
    skip_bits_long(&gb, 32); /* skip toolset_idc_l */

    // 0 - monochrome
    // 1 - 4:2:0
    // 2 - 4:2:2
    // 3 - 4:4:4
    sps->chroma_format_idc = get_ue_golomb(&gb);

    get_ue_golomb(&gb); /* skip pic_width_in_luma_samples */
    get_ue_golomb(&gb); /* skip pic_height_in_luma_samples */

    get_ue_golomb(&gb); /* skip bit_depth_luma_minus8 = */
    get_ue_golomb(&gb); /* skip bit_depth_chroma_minus8 = */

    sps->sps_btt_flag = get_bits(&gb, 1);
    if (sps->sps_btt_flag) {
        get_ue_golomb(&gb); /* skip log2_ctu_size_minus5 */
        get_ue_golomb(&gb); /* skip log2_min_cb_size_minus2 */
        get_ue_golomb(&gb); /* skip log2_diff_ctu_max_14_cb_size */
        get_ue_golomb(&gb); /* skip log2_diff_ctu_max_tt_cb_size */
        get_ue_golomb(&gb); /* skip log2_diff_min_cb_min_tt_cb_size_minus2 */
    }

    sps->sps_suco_flag = get_bits(&gb, 1);
    if (sps->sps_suco_flag) {
        get_ue_golomb(&gb); /* skip log2_diff_ctu_size_max_suco_cb_size = */
        get_ue_golomb(&gb); /* skip log2_diff_max_suco_min_suco_cb_size = */
    }

    sps->sps_admvp_flag = get_bits(&gb, 1);
    if (sps->sps_admvp_flag) {
        skip_bits_long(&gb, 1); /* skip sps_affine_flag */
        skip_bits_long(&gb, 1); /* skip sps_amvr_flag */
        skip_bits_long(&gb, 1); /* skip sps_dmvr_flag */

        sps->sps_mmvd_flag = get_bits(&gb, 1);

        skip_bits_long(&gb, 1); /* skip sps_hmvp_flag */
    }

    sps->sps_eipd_flag =  get_bits(&gb, 1);
    if (sps->sps_eipd_flag) {
        sps->sps_ibc_flag = get_bits(&gb, 1);
        if (sps->sps_ibc_flag)
            get_ue_golomb(&gb); /* skip log2_max_ibc_cand_size_minus2 */
    }

    sps->sps_cm_init_flag = get_bits(&gb, 1);
    if (sps->sps_cm_init_flag)
        sps->sps_adcc_flag = get_bits(&gb, 1);

    sps->sps_iqt_flag = get_bits(&gb, 1);
    if (sps->sps_iqt_flag)
        skip_bits_long(&gb, 1); /* skip sps_ats_flag */

    skip_bits_long(&gb, 1); /* skip sps_addb_flag */

    sps->sps_alf_flag = get_bits(&gb, 1);

    skip_bits_long(&gb, 1); /* skip sps->sps_htdf_flag */

    sps->sps_rpl_flag = get_bits(&gb, 1);
    sps->sps_pocs_flag = get_bits(&gb, 1);

    skip_bits_long(&gb, 1); /* skip sps_dquant_flag */
    skip_bits_long(&gb, 1); /* skip sps_dra_flag */

    if (sps->sps_pocs_flag)
        sps->log2_max_pic_order_cnt_lsb_minus4 = get_ue_golomb(&gb);

    if (!sps->sps_pocs_flag || !sps->sps_rpl_flag)
        sps->log2_sub_gop_length = get_ue_golomb(&gb);

    return sps;
}

// @see ISO_IEC_23094-1 (7.3.2.2 SPS RBSP syntax)
//
// @note
// The current implementation of parse_sps function doesn't handle VUI parameters parsing.
// If it will be needed, parse_sps function could be extended to handle VUI parameters parsing
// to initialize fields of the AVCodecContex i.e. color_primaries, color_trc,color_range
//
static EVCParserPPS *parse_pps(const uint8_t *bs, int bs_size, EVCDemuxContext *ev)
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

    get_ue_golomb(&gb); /* skip num_ref_idx_default_active_minus1[0] */
    get_ue_golomb(&gb); /* skip num_ref_idx_default_active_minus1[1] */
    get_ue_golomb(&gb); /* skip additional_lt_poc_lsb_len */
    skip_bits_long(&gb, 1); /* skip rpl1_idx_present_flag */
    pps->single_tile_in_pic_flag = get_bits(&gb, 1);

    if (!pps->single_tile_in_pic_flag) {
        pps->num_tile_columns_minus1 = get_ue_golomb(&gb);
        pps->num_tile_rows_minus1 = get_ue_golomb(&gb);
        pps->uniform_tile_spacing_flag = get_bits(&gb, 1);

        if (!pps->uniform_tile_spacing_flag) {
            for (int i = 0; i < pps->num_tile_columns_minus1; i++)
                get_ue_golomb(&gb); /* skip tile_column_width_minus1[i] */

            for (int i = 0; i < pps->num_tile_rows_minus1; i++)
                get_ue_golomb(&gb); /* skip tile_row_height_minus1[i] */
        }
        skip_bits_long(&gb, 1);     /* skip loop_filter_across_tiles_enabled_flag */
        get_ue_golomb(&gb);         /* skip tile_offset_len_minus1 */
    }

    pps->tile_id_len_minus1 = get_ue_golomb(&gb);
    pps->explicit_tile_id_flag = get_bits(&gb, 1);

    if (pps->explicit_tile_id_flag) {
        for (int i = 0; i <= pps->num_tile_rows_minus1; i++) {
            for (int j = 0; j <= pps->num_tile_columns_minus1; j++)
                skip_bits_long(&gb, pps->tile_id_len_minus1 + 1); /* skip tile_id_val[i][j] */;
        }
    }

    pps->pic_dra_enabled_flag = 0;
    pps->pic_dra_enabled_flag = get_bits(&gb, 1);

    if (pps->pic_dra_enabled_flag)
        skip_bits_long(&gb, 5); /* skip pic_dra_aps_id */

    pps->arbitrary_slice_present_flag = get_bits(&gb, 1);

    return pps;
}

// @see ISO_IEC_23094-1 (7.3.2.6 Slice layer RBSP syntax)
static EVCParserSliceHeader *parse_slice_header(const uint8_t *bs, int bs_size, EVCDemuxContext *ev)
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
        skip_bits_long(&gb, pps->tile_id_len_minus1 + 1); /* skip first_tile_id */
    } else
        sh->single_tile_in_slice_flag = 1;

    if (!sh->single_tile_in_slice_flag) {
        if (pps->arbitrary_slice_present_flag)
            sh->arbitrary_slice_flag = get_bits(&gb, 1);

        if (!sh->arbitrary_slice_flag)
            skip_bits_long(&gb, pps->tile_id_len_minus1 + 1); /* skip last_tile_id */
        else {
            sh->num_remaining_tiles_in_slice_minus1 = get_ue_golomb(&gb);
            num_tiles_in_slice = sh->num_remaining_tiles_in_slice_minus1 + 2;
            for (int i = 0; i < num_tiles_in_slice - 1; ++i)
                get_ue_golomb(&gb); /* skip delta_tile_id_minus1[i] */
        }
    }

    sh->slice_type = get_ue_golomb(&gb);

    if (ev->nalu_type == EVC_IDR_NUT)
        skip_bits_long(&gb, 1);     /* skip no_output_of_prior_pics_flag */

    if (sps->sps_mmvd_flag && ((sh->slice_type == EVC_SLICE_TYPE_B) || (sh->slice_type == EVC_SLICE_TYPE_P)))
        skip_bits_long(&gb, 1);     /* skip mmvd_group_enable_flag */
    // else
    //    sh->mmvd_group_enable_flag = 0;

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
                skip_bits_long(&gb, 5); /* skip slice_alf_chroma_aps_id */
                skip_bits_long(&gb, 1); /* skip slice_alf_chroma_map_flag */
            }

            if (sliceChroma2AlfEnabledFlag) {
                skip_bits_long(&gb, 5); /* skip slice_alf_chroma2_aps_id */
                skip_bits_long(&gb, 1); /* skip slice_alf_chroma2_map_flag */
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

static int parse_nal_units(const AVProbeData *p, EVCParserContext *ev)
{
    int nalu_type;
    size_t nalu_size;
    unsigned char *bits = (unsigned char *)p->buf;
    int bytes_to_read = p->buf_size;

    while (bytes_to_read > EVC_NALU_LENGTH_PREFIX_SIZE) {

        nalu_size = read_nal_unit_length(bits, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (nalu_size == 0) break;

        bits += EVC_NALU_LENGTH_PREFIX_SIZE;
        bytes_to_read -= EVC_NALU_LENGTH_PREFIX_SIZE;

        if(bytes_to_read < nalu_size) break;

        nalu_type = get_nalu_type(bits, bytes_to_read);

        if (nalu_type == EVC_SPS_NUT)
            ev->got_sps++;
        else if (nalu_type == EVC_PPS_NUT)
            ev->got_pps++;
        else if (nalu_type == EVC_IDR_NUT )
            ev->got_idr++;
        else if (nalu_type == EVC_NOIDR_NUT)
            ev->got_nonidr++;

        bits += nalu_size;
        bytes_to_read -= nalu_size;
    }

    return 0;
}

static int annexb_probe(const AVProbeData *p)
{
    EVCParserContext ev = {0};
    int ret = parse_nal_units(p, &ev);

    if (ret == 0 && ev.got_sps && ev.got_pps && (ev.got_idr || ev.got_nonidr > 3))
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg

    return 0;
}

static int annexb_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;
    EVCDemuxContext *c = s->priv_data;
    int ret = 0;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    sti = ffstream(st);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_EVC;
    sti->need_parsing = AVSTREAM_PARSE_FULL_RAW;

    st->avg_frame_rate = c->framerate;
    sti->avctx->framerate = c->framerate;

    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

fail:
    return ret;
}

static int evc_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;
    EVCDemuxContext *c = s->priv_data;
    int ret = 0;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    sti = ffstream(st);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_EVC;

    // This causes sending to the parser full frames, not chunks of data
    // The flag PARSER_FLAG_COMPLETE_FRAMES will be set in demux.c (demux.c: 1316)
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    st->avg_frame_rate = c->framerate;
    sti->avctx->framerate = c->framerate;

    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

fail:
    return ret;
}

static int annexb_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, pkt_size;
    int eof;

    pkt_size = RAW_PACKET_SIZE;

    eof = avio_feof (s->pb);
    if(eof) {
        av_packet_unref(pkt);
        return AVERROR_EOF;
    }

    if ((ret = av_new_packet(pkt, pkt_size)) < 0)
        return ret;

    pkt->pos = avio_tell(s->pb);
    pkt->stream_index = 0;
    ret = avio_read_partial(s->pb, pkt->data, pkt_size);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }
    av_shrink_packet(pkt, ret);

    av_log(s, AV_LOG_ERROR, "annexb_read_packet: %d\n", pkt_size);

    return ret;
}

static int parse_nal_unit(const uint8_t *buf, size_t buf_size, AVFormatContext *s)
{
    int nalu_type, nalu_size;
    int tid;
    const uint8_t *data = buf;
    int data_size = buf_size;
    EVCDemuxContext *ev = s->priv_data;

    nalu_size = buf_size;
    if (nalu_size <= 0) {
        av_log(s, AV_LOG_ERROR, "Invalid NAL unit size: (%d)\n", nalu_size);
        return AVERROR_INVALIDDATA;
    }

    // @see ISO_IEC_23094-1_2020, 7.4.2.2 NAL unit header semantic (Table 4 - NAL unit type codes and NAL unit type classes)
    // @see enum EVCNALUnitType in evc.h
    nalu_type = get_nalu_type(data, data_size);
    if (nalu_type < EVC_NOIDR_NUT || nalu_type > EVC_UNSPEC_NUT62) {
        av_log(s, AV_LOG_ERROR, "Invalid NAL unit type: (%d)\n", nalu_type);
        return AVERROR_INVALIDDATA;
    }
    ev->nalu_type = nalu_type;

    tid = get_temporal_id(data, data_size);
    if (tid < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid temporial id: (%d)\n", tid);
        return AVERROR_INVALIDDATA;
    }

    data += EVC_NALU_HEADER_SIZE;
    data_size -= EVC_NALU_HEADER_SIZE;

    switch(nalu_type) {
    case EVC_SPS_NUT: {
        EVCParserSPS *sps;

        sps = parse_sps(data, nalu_size, ev);
        if (!sps) {
            av_log(s, AV_LOG_ERROR, "SPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        if (sps->profile_idc == 1) ev->profile = FF_PROFILE_EVC_MAIN;
        else ev->profile = FF_PROFILE_EVC_BASELINE;

        break;
    }
    case EVC_PPS_NUT: {
        EVCParserPPS *pps;

        pps = parse_pps(data, nalu_size, ev);
        if (!pps) {
            av_log(s, AV_LOG_ERROR, "PPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }
        break;
    }
    case EVC_SEI_NUT:   // Supplemental Enhancement Information
    case EVC_APS_NUT:   // Adaptation parameter set
    case EVC_FD_NUT:    // Filler data
        break;
    case EVC_IDR_NUT:   // Coded slice of a IDR or non-IDR picture
    case EVC_NOIDR_NUT: {
        EVCParserSliceHeader *sh;
        EVCParserSPS *sps;
        int slice_pic_parameter_set_id;

        sh = parse_slice_header(data, nalu_size, ev);
        if (!sh) {
            av_log(s, AV_LOG_ERROR, "Slice header parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        ev->key_frame = (nalu_type == EVC_IDR_NUT) ? 1 : 0;

        // POC (picture order count of the current picture) derivation
        // @see ISO/IEC 23094-1:2020(E) 8.3.1 Decoding process for picture order count
        slice_pic_parameter_set_id = sh->slice_pic_parameter_set_id;
        sps = ev->sps[slice_pic_parameter_set_id];

        if (sps && sps->sps_pocs_flag) {

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

        break;
    }
    }

    return 0;
}

static int evc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    int pkt_size;

    int bytes_read = 0;
    int bytes_left = 0;

    int32_t nalu_size;
    int au_end_found;

    int eof = avio_feof (s->pb);
    if(eof) {
        av_packet_unref(pkt);
        return AVERROR_EOF;
    }

    pkt_size = RAW_PACKET_SIZE;
    if ((ret = av_new_packet(pkt, pkt_size)) < 0)
        return ret;

    pkt->pos = avio_tell(s->pb);
    pkt->stream_index = 0;
    au_end_found = 0;

    while(!au_end_found) {

        bytes_left = pkt_size - bytes_read;
        if( bytes_left < EVC_NALU_LENGTH_PREFIX_SIZE ) {
            int grow_by = pkt_size;
            pkt_size = pkt_size * 2;
            av_grow_packet(pkt, grow_by);
            bytes_left = pkt_size - bytes_read;
            av_log(s, AV_LOG_DEBUG, "Resizing packet size to: %d bytes\n", pkt_size);
        }

        ret = avio_read(s->pb, pkt->data + bytes_read, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }

        nalu_size = read_nal_unit_length(pkt->data + bytes_read, EVC_NALU_LENGTH_PREFIX_SIZE);
        if(nalu_size <= 0) {
            av_packet_unref(pkt);
            return -1;
        }

        bytes_read += ret;

        bytes_left = pkt_size - bytes_read;
        while( bytes_left < nalu_size ) {
            int grow_by = pkt_size;
            pkt_size = pkt_size * 2;
            av_grow_packet(pkt, grow_by);
            bytes_left = pkt_size - bytes_read;
            av_log(s, AV_LOG_DEBUG, "Resizing packet pkt_size to: %d bytes\n", pkt_size);
        }

        ret = avio_read(s->pb, pkt->data + bytes_read, nalu_size);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }

        // parse NAL unit is neede to determine whether we found end of AU
        parse_nal_unit(pkt->data + bytes_read, nalu_size, s);
        au_end_found = end_of_access_unit_found(pkt->data + bytes_read, nalu_size, s);

        bytes_read += nalu_size;
    }
    av_shrink_packet(pkt, bytes_read);

    return ret;
}

static int evc_read_close(AVFormatContext *s)
{
    return 0;
}

const AVInputFormat ff_evc_demuxer = {
    .name           = "evc",
    .long_name      = NULL_IF_CONFIG_SMALL("EVC Annex B"),
    .read_probe     = annexb_probe,
    .read_header    = evc_read_header, // annexb_read_header
    .read_packet    = evc_read_packet, // annexb_read_packet
    .read_close     = evc_read_close,
    .extensions     = "evc",
    .flags          = AVFMT_GENERIC_INDEX,
    .flags_internal = FF_FMT_INIT_CLEANUP,
    .raw_codec_id   = AV_CODEC_ID_EVC,
    .priv_data_size = sizeof(EVCDemuxContext),
    .priv_class     = &evc_demuxer_class,
};
