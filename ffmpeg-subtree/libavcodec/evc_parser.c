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
#include "bytestream.h"
#include "evc.h"
#include "evc_parse.h"

#define EVC_MAX_QP_TABLE_SIZE   58

#define EXTENDED_SAR            255
#define NUM_CPB                 32

#define NUM_CHROMA_FORMATS      4   // @see ISO_IEC_23094-1 section 6.2 table 2

static const enum AVPixelFormat pix_fmts_8bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY8, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P
};

static const enum AVPixelFormat pix_fmts_9bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY9, AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9
};

static const enum AVPixelFormat pix_fmts_10bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY10, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10
};

static const enum AVPixelFormat pix_fmts_12bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY12, AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12
};

static const enum AVPixelFormat pix_fmts_14bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY14, AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14
};

static const enum AVPixelFormat pix_fmts_16bit[NUM_CHROMA_FORMATS] = {
    AV_PIX_FMT_GRAY16, AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16
};

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
    nalu_type = ff_evc_get_nalu_type(data, data_size, avctx);
    if (nalu_type < EVC_NOIDR_NUT || nalu_type > EVC_UNSPEC_NUT62) {
        av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit type: (%d)\n", nalu_type);
        return AVERROR_INVALIDDATA;
    }
    ev->nalu_type = nalu_type;

    tid = ff_evc_get_temporal_id(data, data_size, avctx);
    if (tid < 0) {
        av_log(avctx, AV_LOG_ERROR, "Invalid temporial id: (%d)\n", tid);
        return AVERROR_INVALIDDATA;
    }
    ev->nuh_temporal_id = tid;

    data += EVC_NALU_HEADER_SIZE;
    data_size -= EVC_NALU_HEADER_SIZE;

    switch(nalu_type) {
    case EVC_SPS_NUT: {
        EVCParserSPS *sps;
        int SubGopLength;
        int bit_depth;

        sps = ff_evc_parse_sps(data, nalu_size, ev);
        if (!sps) {
            av_log(avctx, AV_LOG_ERROR, "SPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        s->coded_width         = sps->pic_width_in_luma_samples;
        s->coded_height        = sps->pic_height_in_luma_samples;

        if(sps->picture_cropping_flag) {
            s->width           = sps->pic_width_in_luma_samples  - sps->picture_crop_left_offset - sps->picture_crop_right_offset;
            s->height          = sps->pic_height_in_luma_samples - sps->picture_crop_top_offset  - sps->picture_crop_bottom_offset;
        } else {
            s->width           = sps->pic_width_in_luma_samples;
            s->height          = sps->pic_height_in_luma_samples;
        }

        avctx->coded_width     = s->coded_width;
        avctx->coded_height    = s->coded_height;
        avctx->width           = s->width;
        avctx->height          = s->height;

        SubGopLength = (int)pow(2.0, sps->log2_sub_gop_length);
        avctx->gop_size = SubGopLength;

        avctx->delay = (sps->sps_max_dec_pic_buffering_minus1) ? sps->sps_max_dec_pic_buffering_minus1 - 1 : SubGopLength + sps->max_num_tid0_ref_pics - 1;

        if (sps->profile_idc == 1) avctx->profile = FF_PROFILE_EVC_MAIN;
        else avctx->profile = FF_PROFILE_EVC_BASELINE;

        if (sps->vui_parameters_present_flag) {
            if (sps->vui_parameters.timing_info_present_flag) {
                int64_t num = sps->vui_parameters.num_units_in_tick;
                int64_t den = sps->vui_parameters.time_scale;
                if (num != 0 && den != 0)
                    av_reduce(&avctx->framerate.den, &avctx->framerate.num, num, den, 1 << 30);
            }
        }

        bit_depth = sps->bit_depth_chroma_minus8 + 8;
        s->format = AV_PIX_FMT_NONE;

        switch (bit_depth) {
        case 8:
            s->format = pix_fmts_8bit[sps->chroma_format_idc];
            break;
        case 9:
            s->format = pix_fmts_9bit[sps->chroma_format_idc];
            break;
        case 10:
            s->format = pix_fmts_10bit[sps->chroma_format_idc];
            break;
        case 12:
            s->format = pix_fmts_12bit[sps->chroma_format_idc];
            break;
        case 14:
            s->format = pix_fmts_14bit[sps->chroma_format_idc];
            break;
        case 16:
            s->format = pix_fmts_16bit[sps->chroma_format_idc];
            break;
        }
        av_assert0(s->format != AV_PIX_FMT_NONE);

        break;
    }
    case EVC_PPS_NUT: {
        EVCParserPPS *pps;

        pps = ff_evc_parse_pps(data, nalu_size, ev);
        if (!pps) {
            av_log(avctx, AV_LOG_ERROR, "PPS parsing error\n");
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

        sh = ff_evc_parse_slice_header(data, nalu_size, ev);
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

        s->output_picture_number = ev->poc.PicOrderCntVal;
        s->key_frame = (nalu_type == EVC_IDR_NUT) ? 1 : 0;

        break;
    }
    }

    return 0;
}

static int parse_nal_units(AVCodecParserContext *s, const uint8_t *buf,
                           int buf_size, AVCodecContext *avctx)
{
    const uint8_t *data = buf;
    int data_size = buf_size;
    int bytes_read = 0;
    int nalu_size = 0;

    while (data_size > 0) {

        // Buffer size is not enough for buffer to store NAL unit 4-bytes prefix (length)
        if (data_size < EVC_NALU_LENGTH_PREFIX_SIZE)
            return END_NOT_FOUND;

        nalu_size = ff_evc_read_nal_unit_length(data, data_size, avctx);
        bytes_read += EVC_NALU_LENGTH_PREFIX_SIZE;

        data += EVC_NALU_LENGTH_PREFIX_SIZE;
        data_size -= EVC_NALU_LENGTH_PREFIX_SIZE;

        if (data_size < nalu_size)
            return END_NOT_FOUND;

        if (parse_nal_unit(s, data, nalu_size, avctx) != 0) {
            av_log(avctx, AV_LOG_ERROR, "Parsing of NAL unit failed\n");
            return AVERROR_INVALIDDATA;
        }

        data += nalu_size;
        data_size -= nalu_size;
    }
    return 0;
}

// Decoding nal units from evcC (EVCDecoderConfigurationRecord)
// @see @see ISO/IEC 14496-15:2021 Coding of audio-visual objects - Part 15: section 12.3.3.2
static int decode_extradata(AVCodecParserContext *s, AVCodecContext *avctx, const uint8_t *data, int size)
{
    int ret = 0;
    GetByteContext gb;

    bytestream2_init(&gb, data, size);

    if (!data || size <= 0)
        return -1;

    // extradata is encoded as evcC format.
    if (data[0] == 1) {
        int num_of_arrays;  // indicates the number of arrays of NAL units of the indicated type(s)

        int nalu_length_field_size; // indicates the length in bytes of the NALUnitLenght field in EVC video stream sample in the stream
                                    // The value of this field shall be one of 0, 1, or 3 corresponding to a length encoded with 1, 2, or 4 bytes, respectively.

        if (bytestream2_get_bytes_left(&gb) < 18) {
            av_log(avctx, AV_LOG_ERROR, "evcC %d too short\n", size);
            return AVERROR_INVALIDDATA;
        }

        bytestream2_skip(&gb, 16);

        // @see ISO/IEC 14496-15:2021 Coding of audio-visual objects - Part 15: section 12.3.3.3
        // LengthSizeMinusOne plus 1 indicates the length in bytes of the NALUnitLength field in a EVC video stream sample in the stream to which this configuration record applies. For example, a size of one byte is indicated with a value of 0.
        // The value of this field shall be one of 0, 1, or 3 corresponding to a length encoded with 1, 2, or 4 bytes, respectively.
        nalu_length_field_size = (bytestream2_get_byte(&gb) & 3) + 1;
        if( nalu_length_field_size != 1 &&
            nalu_length_field_size != 2 &&
            nalu_length_field_size != 4 ) {
            av_log(avctx, AV_LOG_ERROR, "The length in bytes of the NALUnitLenght field in a EVC video stream has unsupported value of %d\n", nalu_length_field_size);
            return AVERROR_INVALIDDATA;
        }

        num_of_arrays = bytestream2_get_byte(&gb);

        /* Decode nal units from evcC. */
        for (int i = 0; i < num_of_arrays; i++) {

            // @see ISO/IEC 14496-15:2021 Coding of audio-visual objects - Part 15: section 12.3.3.3
            // NAL_unit_type indicates the type of the NAL units in the following array (which shall be all of that type);
            // - it takes a value as defined in ISO/IEC 23094-1;
            // - it is restricted to take one of the values indicating a SPS, PPS, APS, or SEI NAL unit.
            int nal_unit_type = bytestream2_get_byte(&gb) & 0x3f;
            int num_nalus  = bytestream2_get_be16(&gb);

            for (int j = 0; j < num_nalus; j++) {

                int nal_unit_length = bytestream2_get_be16(&gb);

                if (bytestream2_get_bytes_left(&gb) < nal_unit_length) {
                    av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit size in extradata.\n");
                    return AVERROR_INVALIDDATA;
                }

                if( nal_unit_type == EVC_SPS_NUT ||
                    nal_unit_type == EVC_PPS_NUT ||
                    nal_unit_type == EVC_APS_NUT ||
                    nal_unit_type == EVC_SEI_NUT ) {
                    if (parse_nal_unit(s, gb.buffer, nal_unit_length, avctx) != 0) {
                        av_log(avctx, AV_LOG_ERROR, "Parsing of NAL unit failed\n");
                        return AVERROR_INVALIDDATA;
                    }
                }

                bytestream2_skip(&gb, nal_unit_length);
            }
        }
    } else
        return -1;

    return ret;
}

static int evc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    int next;
    EVCParserContext *ev = s->priv_data;

    if (avctx->extradata && !ev->parsed_extradata) {
        decode_extradata(s, avctx, avctx->extradata, avctx->extradata_size);
        ev->parsed_extradata = 1;
    }

    next = buf_size;

    parse_nal_units(s, buf, buf_size, avctx);

    // poutbuf contains just one Access Unit
    *poutbuf      = buf;
    *poutbuf_size = buf_size;

    return next;
}

static int evc_parser_init(AVCodecParserContext *s)
{
    EVCParserContext *ev = s->priv_data;

    memset(ev->sps, 0, sizeof(EVCParserSPS *)*EVC_MAX_SPS_COUNT);
    memset(ev->pps, 0, sizeof(EVCParserPPS *)*EVC_MAX_PPS_COUNT);
    memset(ev->slice_header, 0, sizeof(EVCParserSliceHeader *)*EVC_MAX_PPS_COUNT);

    return 0;
}

static void evc_parser_close(AVCodecParserContext *s)
{
    EVCParserContext *ev = s->priv_data;

    for(int i = 0; i < EVC_MAX_SPS_COUNT; i++) {
        EVCParserSPS *sps = ev->sps[i];
        av_freep(&sps);
    }

    for(int i = 0; i < EVC_MAX_PPS_COUNT; i++) {
        EVCParserPPS *pps = ev->pps[i];
        EVCParserSliceHeader *sh = ev->slice_header[i];

        av_freep(&pps);
        av_freep(&sh);
    }
}

const AVCodecParser ff_evc_parser = {
    .codec_ids      = { AV_CODEC_ID_EVC },
    .priv_data_size = sizeof(EVCParserContext),
    .parser_init    = evc_parser_init,
    .parser_parse   = evc_parse,
    .parser_close   = evc_parser_close,
};
