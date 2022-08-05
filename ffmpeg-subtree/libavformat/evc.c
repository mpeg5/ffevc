/*
 * EVC helper functions for muxers
 * Copyright (c) 2022 Dawid Kozinski <d.kozinski@samsung.com>
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

#include "libavutil/intreadwrite.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "avformat.h"
#include "avio.h"
#include "evc.h"
#include "avio_internal.h"

// The length field that indicates the length in bytes of the following NAL unit is configured to be of 4 bytes
#define EVC_NAL_UNIT_LENGTH_BYTE        (4)  /* byte */
#define EVC_NAL_HEADER_SIZE             (2)  /* byte */

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
typedef struct EVCSPS {
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

    // @note
    // Currently the structure does not reflect the entire SPS RBSP layout.
    // It contains only the fields that are necessary to read from the NAL unit all the values
    // necessary for the correct initialization of EVCDecoderConfigurationRecord

    // @note
    // If necessary, add the missing fields to the structure to reflect
    // the contents of the entire NAL unit of the SPS type

} EVCSPS;

typedef struct EVCNALUnitArray {
    uint8_t  array_completeness;
    uint8_t  NAL_unit_type;
    uint16_t numNalus;
    uint16_t *nalUnitLength;
    uint8_t  **nalUnit;
} EVCNALUnitArray;

/**
 * @brief Specifies the decoder configuration information for ISO/IEC 23094-1 video content.
 * @see ISO/IEC 14496-15:2021 Coding of audio-visual objects — Part 15:
 *      Carriage of network abstraction layer (NAL) unit structured video in the ISO base media file format
 */
typedef struct EVCDecoderConfigurationRecord {
    uint8_t  configurationVersion;          // 8 bits
    uint8_t  profile_idc;                   // 8 bits
    uint8_t  level_idc;                     // 8 bits
    uint32_t toolset_idc_h;                 // 32 bits
    uint32_t toolset_idc_l;                 // 32 bits
    uint8_t  chroma_format_idc;             // 2 bits
    uint8_t  bit_depth_luma_minus8;         // 3 bits
    uint8_t  bit_depth_chroma_minus8;       // 3 bits
    uint16_t pic_width_in_luma_samples;     // 16 bits
    uint16_t pic_height_in_luma_samples;    // 16 bits
    uint8_t  reserved;                      // 6 bits '000000'b
    uint8_t  lengthSizeMinusOne;            // 2 bits
    uint8_t  num_of_arrays;                 // 8 bits
    EVCNALUnitArray *array;
} EVCDecoderConfigurationRecord;

typedef struct NALU {
    int offset;
    uint32_t size;
} NALU;

typedef struct NALUList {
    NALU *nalus;
    unsigned nalus_array_size;
    unsigned nb_nalus;          ///< valid entries in nalus
} NALUList;

static int get_nalu_type(const uint8_t *bits, int bits_size)
{
    int unit_type_plus1 = 0;

    if (bits_size >= EVC_NAL_HEADER_SIZE) {
        unsigned char *p = (unsigned char *)bits;
        // forbidden_zero_bit
        if ((p[0] & 0x80) != 0)
            return -1;

        // nal_unit_type
        unit_type_plus1 = (p[0] >> 1) & 0x3F;
    }

    return unit_type_plus1 - 1;
}

static uint32_t read_nal_unit_length(const uint8_t *bits, int bits_size)
{
    uint32_t nalu_len = 0;

    if (bits_size >= EVC_NAL_UNIT_LENGTH_BYTE) {

        int t = 0;
        unsigned char *p = (unsigned char *)bits;

        for (int i = 0; i < EVC_NAL_UNIT_LENGTH_BYTE; i++)
            t = (t << 8) | p[i];

        nalu_len = t;
        if (nalu_len == 0)
            return 0;
    }

    return nalu_len;
}

// @see ISO_IEC_23094-1 (7.3.2.1 SPS RBSP syntax)
static int evcc_parse_sps(const uint8_t *bs, int bs_size, EVCDecoderConfigurationRecord *evcc)
{
    GetBitContext gb;
    int sps_seq_parameter_set_id;
    EVCSPS sps;

    if (init_get_bits8(&gb, bs, bs_size) < 0)
        return 0;

    sps.sps_seq_parameter_set_id = get_ue_golomb(&gb);

    if (sps_seq_parameter_set_id >= EVC_MAX_SPS_COUNT)
        return 0;

    // the Baseline profile is indicated by profile_idc eqal to 0
    // the Main profile is indicated by profile_idc eqal to 1
    sps.profile_idc = get_bits(&gb, 8);

    sps.level_idc = get_bits(&gb, 8);

    sps.toolset_idc_h = get_bits(&gb, 32);
    sps.toolset_idc_l = get_bits(&gb, 32);

    // 0 - monochrome
    // 1 - 4:2:0
    // 2 - 4:2:2
    // 3 - 4:4:4
    sps.chroma_format_idc = get_ue_golomb(&gb);

    sps.pic_width_in_luma_samples = get_ue_golomb(&gb);
    sps.pic_height_in_luma_samples = get_ue_golomb(&gb);

    sps.bit_depth_luma_minus8 = get_ue_golomb(&gb);
    sps.bit_depth_chroma_minus8 = get_ue_golomb(&gb);

    evcc->profile_idc = sps.profile_idc;
    evcc->level_idc = sps.level_idc;
    evcc->toolset_idc_h = sps.toolset_idc_h;
    evcc->toolset_idc_l = sps.toolset_idc_l;
    evcc->chroma_format_idc = sps.chroma_format_idc;
    evcc->bit_depth_luma_minus8 = sps.bit_depth_luma_minus8;
    evcc->bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;
    evcc->pic_width_in_luma_samples = sps.pic_width_in_luma_samples;
    evcc->pic_height_in_luma_samples = sps.pic_height_in_luma_samples;

    return 0;
}

static int evcc_array_add_nal_unit(uint8_t *nal_buf, uint32_t nal_size,
                                   uint8_t nal_type, int ps_array_completeness,
                                   EVCDecoderConfigurationRecord *evcc)
{
    int ret;
    uint8_t index;
    uint16_t numNalus;
    EVCNALUnitArray *array;

    for (index = 0; index < evcc->num_of_arrays; index++)
        if (evcc->array[index].NAL_unit_type == nal_type)
            break;

    if (index >= evcc->num_of_arrays) {
        uint8_t i;

        ret = av_reallocp_array(&evcc->array, index + 1, sizeof(EVCNALUnitArray));
        if (ret < 0)
            return ret;

        for (i = evcc->num_of_arrays; i <= index; i++)
            memset(&evcc->array[i], 0, sizeof(EVCNALUnitArray));
        evcc->num_of_arrays = index + 1;
    }

    array    = &evcc->array[index];
    numNalus = array->numNalus;

    ret = av_reallocp_array(&array->nalUnit, numNalus + 1, sizeof(uint8_t *));
    if (ret < 0)
        return ret;

    ret = av_reallocp_array(&array->nalUnitLength, numNalus + 1, sizeof(uint16_t));
    if (ret < 0)
        return ret;

    array->nalUnit      [numNalus] = nal_buf;
    array->nalUnitLength[numNalus] = nal_size;
    array->NAL_unit_type           = nal_type;
    array->numNalus++;

    /*
     * When the sample entry name is 'evc1', the default and mandatory value of
     * array_completeness is 1 for arrays of all types of parameter sets, and 0
     * for all other arrays.
     */
    if (nal_type == EVC_SPS_NUT || nal_type == EVC_PPS_NUT)
        array->array_completeness = ps_array_completeness;

    return 0;
}

static void evcc_init(EVCDecoderConfigurationRecord *evcc)
{
    memset(evcc, 0, sizeof(EVCDecoderConfigurationRecord));
    evcc->configurationVersion = 1;
    evcc->lengthSizeMinusOne   = 3; // 4 bytes
}

static void evcc_close(EVCDecoderConfigurationRecord *evcc)
{
    uint8_t i;

    for (i = 0; i < evcc->num_of_arrays; i++) {
        evcc->array[i].numNalus = 0;
        av_freep(&evcc->array[i].nalUnit);
        av_freep(&evcc->array[i].nalUnitLength);
    }

    evcc->num_of_arrays = 0;
    av_freep(&evcc->array);
}

static int evcc_write(AVIOContext *pb, EVCDecoderConfigurationRecord *evcc)
{
    uint8_t i;
    uint16_t j, aps_count = 0, sps_count = 0, pps_count = 0;

    av_log(NULL, AV_LOG_TRACE,  "configurationVersion:                %"PRIu8"\n",
           evcc->configurationVersion);
    av_log(NULL, AV_LOG_TRACE,  "profile_idc:                         %"PRIu8"\n",
           evcc->profile_idc);
    av_log(NULL, AV_LOG_TRACE,  "level_idc:                           %"PRIu8"\n",
           evcc->level_idc);
    av_log(NULL, AV_LOG_TRACE,  "toolset_idc_h:                       %"PRIu32"\n",
           evcc->toolset_idc_h);
    av_log(NULL, AV_LOG_TRACE, "toolset_idc_l:                        %"PRIu32"\n",
           evcc->toolset_idc_l);
    av_log(NULL, AV_LOG_TRACE, "chroma_format_idc:                    %"PRIu8"\n",
           evcc->chroma_format_idc);
    av_log(NULL, AV_LOG_TRACE,  "bit_depth_luma_minus8:               %"PRIu8"\n",
           evcc->bit_depth_luma_minus8);
    av_log(NULL, AV_LOG_TRACE,  "bit_depth_chroma_minus8:             %"PRIu8"\n",
           evcc->bit_depth_chroma_minus8);
    av_log(NULL, AV_LOG_TRACE,  "pic_width_in_luma_samples:           %"PRIu16"\n",
           evcc->pic_width_in_luma_samples);
    av_log(NULL, AV_LOG_TRACE,  "pic_height_in_luma_samples:          %"PRIu16"\n",
           evcc->pic_height_in_luma_samples);
    av_log(NULL, AV_LOG_TRACE,  "lengthSizeMinusOne:                  %"PRIu8"\n",
           evcc->lengthSizeMinusOne);
    av_log(NULL, AV_LOG_TRACE,  "num_of_arrays:                       %"PRIu8"\n",
           evcc->num_of_arrays);
    for (i = 0; i < evcc->num_of_arrays; i++) {
        av_log(NULL, AV_LOG_TRACE, "array_completeness[%"PRIu8"]:               %"PRIu8"\n",
               i, evcc->array[i].array_completeness);
        av_log(NULL, AV_LOG_TRACE, "NAL_unit_type[%"PRIu8"]:                    %"PRIu8"\n",
               i, evcc->array[i].NAL_unit_type);
        av_log(NULL, AV_LOG_TRACE, "numNalus[%"PRIu8"]:                         %"PRIu16"\n",
               i, evcc->array[i].numNalus);
        for (j = 0; j < evcc->array[i].numNalus; j++)
            av_log(NULL, AV_LOG_TRACE,
                   "nalUnitLength[%"PRIu8"][%"PRIu16"]:                 %"PRIu16"\n",
                   i, j, evcc->array[i].nalUnitLength[j]);
    }

    /*
     * We need at least one SPS.
     */
    for (i = 0; i < evcc->num_of_arrays; i++)
        switch (evcc->array[i].NAL_unit_type) {
        case EVC_APS_NUT:
            aps_count += evcc->array[i].numNalus;
            break;
        case EVC_SPS_NUT:
            sps_count += evcc->array[i].numNalus;
            break;
        case EVC_PPS_NUT:
            pps_count += evcc->array[i].numNalus;
            break;
        default:
            break;
        }
    if (!sps_count || sps_count > EVC_MAX_SPS_COUNT)
        return AVERROR_INVALIDDATA;

    /* unsigned int(8) configurationVersion = 1; */
    avio_w8(pb, evcc->configurationVersion);

    /* unsigned int(8) profile_idc */
    avio_w8(pb, evcc->profile_idc);

    /* unsigned int(8) profile_idc */
    avio_w8(pb, evcc->level_idc);

    /* unsigned int(32) toolset_idc_h */
    avio_wb32(pb, evcc->toolset_idc_h);

    /* unsigned int(32) toolset_idc_l */
    avio_wb32(pb, evcc->toolset_idc_l);

    /*
     * unsigned int(2) chroma_format_idc;
     * unsigned int(3) bit_depth_luma_minus8;
     * unsigned int(3) bit_depth_chroma_minus8;
     */
    avio_w8(pb, evcc->chroma_format_idc << 6 |
            evcc->bit_depth_luma_minus8  << 3 |
            evcc->bit_depth_chroma_minus8);

    /* unsigned int(16) pic_width_in_luma_samples; */
    avio_wb16(pb, evcc->pic_width_in_luma_samples);

    /* unsigned int(16) pic_width_in_luma_samples; */
    avio_wb16(pb, evcc->pic_height_in_luma_samples);

    /*
     * bit(6) reserved = '111111'b;
     * unsigned int(2) chromaFormat;
     */
    avio_w8(pb, evcc->lengthSizeMinusOne | 0xfc);

    /* unsigned int(8) numOfArrays; */
    avio_w8(pb, evcc->num_of_arrays);

    for (i = 0; i < evcc->num_of_arrays; i++) {
        /*
         * bit(1) array_completeness;
         * unsigned int(1) reserved = 0;
         * unsigned int(6) NAL_unit_type;
         */
        avio_w8(pb, evcc->array[i].array_completeness << 7 |
                evcc->array[i].NAL_unit_type & 0x3f);

        /* unsigned int(16) numNalus; */
        avio_wb16(pb, evcc->array[i].numNalus);

        for (j = 0; j < evcc->array[i].numNalus; j++) {
            /* unsigned int(16) nalUnitLength; */
            avio_wb16(pb, evcc->array[i].nalUnitLength[j]);

            /* bit(8*nalUnitLength) nalUnit; */
            avio_write(pb, evcc->array[i].nalUnit[j],
                       evcc->array[i].nalUnitLength[j]);
        }
    }

    return 0;
}

int ff_isom_write_evcc(AVIOContext *pb, const uint8_t *data,
                       int size, int ps_array_completeness)
{
    EVCDecoderConfigurationRecord evcc;
    int ret = 0;

    if (size < 8) {
        /* We can't write a valid evcC from the provided data */
        return AVERROR_INVALIDDATA;
    } else if (*data == 1) {
        /* Data is already evcC-formatted */
        avio_write(pb, data, size);
        return 0;
    }

    evcc_init(&evcc);

    int nalu_type;
    size_t nalu_size;
    unsigned char *bits = (unsigned char *)data;
    int bytes_to_read = size;

    while (bytes_to_read > EVC_NAL_UNIT_LENGTH_BYTE) {

        nalu_size = read_nal_unit_length(bits, EVC_NAL_UNIT_LENGTH_BYTE);
        if (nalu_size == 0) break;

        bits += EVC_NAL_UNIT_LENGTH_BYTE;
        bytes_to_read -= EVC_NAL_UNIT_LENGTH_BYTE;

        if (bytes_to_read < nalu_size) break;

        nalu_type = get_nalu_type(bits, bytes_to_read);
        uint8_t *nalu_buf = bits;

        switch (nalu_type) {
        case EVC_APS_NUT:
        case EVC_SPS_NUT:
        case EVC_PPS_NUT:
            ret = evcc_array_add_nal_unit(nalu_buf, nalu_size, nalu_type, ps_array_completeness, &evcc);
            if (ret < 0)
                goto end;
            else if (nalu_type == EVC_SPS_NUT)
                ret = evcc_parse_sps(nalu_buf, nalu_size, &evcc);
            if (ret < 0)
                goto end;
            break;
        default:
            break;
        }

        bits += nalu_size;
        bytes_to_read -= nalu_size;
    }

    ret = evcc_write(pb, &evcc);

end:
    evcc_close(&evcc);
    return ret;
}

