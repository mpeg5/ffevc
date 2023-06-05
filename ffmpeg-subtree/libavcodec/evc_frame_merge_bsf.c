/*
 * Copyright (c) 2019 James Almer <jamrial@gmail.com>
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
#include "get_bits.h"
#include "golomb.h"
#include "bsf.h"
#include "bsf_internal.h"
#include "avcodec.h"

#include "evc.h"
#include "evc_parse.h"

#define INIT_AU_BUF_CAPACITY 1024

// Access unit data
typedef struct AccessUnitBuffer {
    uint8_t *data;      // the data buffer
    size_t data_size;   // size of data in bytes
    size_t capacity;    // buffer capacity
} AccessUnitBuffer;

typedef struct EVCMergeContext {
    AVPacket *in;
    EVCParserContext pc;

    // the Baseline profile is indicated by profile eqal to 0
    // the Main profile is indicated by profile eqal to 1
    int profile;
    int key_frame;

    AccessUnitBuffer au_buffer;

} EVCMergeContext;

static int end_of_access_unit_found(AVBSFContext *s)
{
    EVCMergeContext *evc_merge_ctx = s->priv_data;
    EVCParserContext *evc_parse_ctx = &evc_merge_ctx->pc;

    if (evc_merge_ctx->profile == 0) { // BASELINE profile
        if (evc_parse_ctx->nalu_type == EVC_NOIDR_NUT || evc_parse_ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    } else { // MAIN profile
        if (evc_parse_ctx->nalu_type == EVC_NOIDR_NUT) {
            if (evc_parse_ctx->poc.PicOrderCntVal != evc_parse_ctx->poc.prevPicOrderCntVal)
                return 1;
        } else if (evc_parse_ctx->nalu_type == EVC_IDR_NUT)
            return 1;
    }
    return 0;
}

static int parse_nal_unit(const uint8_t *buf, size_t buf_size, AVBSFContext *s)
{
    int nalu_type, nalu_size;
    int tid;
    const uint8_t *data = buf;
    int data_size = buf_size;
    EVCMergeContext *evc_merge_ctx = s->priv_data;
    EVCParserContext *evc_parse_ctx = &evc_merge_ctx->pc;

    nalu_size = buf_size;
    if (nalu_size <= 0) {
        av_log(s, AV_LOG_ERROR, "Invalid NAL unit size: (%d)\n", nalu_size);
        return AVERROR_INVALIDDATA;
    }

    // @see ISO_IEC_23094-1_2020, 7.4.2.2 NAL unit header semantic (Table 4 - NAL unit type codes and NAL unit type classes)
    // @see enum EVCNALUnitType in evc.h
    nalu_type = ff_evc_get_nalu_type(data, data_size, s);
    if (nalu_type < EVC_NOIDR_NUT || nalu_type > EVC_UNSPEC_NUT62) {
        av_log(s, AV_LOG_ERROR, "Invalid NAL unit type: (%d)\n", nalu_type);
        return AVERROR_INVALIDDATA;
    }
    evc_parse_ctx->nalu_type = nalu_type;

    tid = ff_evc_get_temporal_id(data, data_size, s);
    if (tid < 0) {
        av_log(s, AV_LOG_ERROR, "Invalid temporial id: (%d)\n", tid);
        return AVERROR_INVALIDDATA;
    }

    data += EVC_NALU_HEADER_SIZE;
    data_size -= EVC_NALU_HEADER_SIZE;

    switch(nalu_type) {
    case EVC_SPS_NUT: {
        EVCParserSPS *sps;

        sps = ff_evc_parse_sps(data, nalu_size, evc_parse_ctx);
        if (!sps) {
            av_log(s, AV_LOG_ERROR, "SPS parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        if (sps->profile_idc == 1) evc_merge_ctx->profile = FF_PROFILE_EVC_MAIN;
        else evc_merge_ctx->profile = FF_PROFILE_EVC_BASELINE;

        break;
    }
    case EVC_PPS_NUT: {
        EVCParserPPS *pps;

        pps = ff_evc_parse_pps(data, nalu_size, evc_parse_ctx);
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

        sh = ff_evc_parse_slice_header(data, nalu_size, evc_parse_ctx);
        if (!sh) {
            av_log(s, AV_LOG_ERROR, "Slice header parsing error\n");
            return AVERROR_INVALIDDATA;
        }

        evc_merge_ctx->key_frame = (nalu_type == EVC_IDR_NUT) ? 1 : 0;

        // POC (picture order count of the current picture) derivation
        // @see ISO/IEC 23094-1:2020(E) 8.3.1 Decoding process for picture order count
        slice_pic_parameter_set_id = sh->slice_pic_parameter_set_id;
        sps = evc_parse_ctx->sps[slice_pic_parameter_set_id];

        if (sps && sps->sps_pocs_flag) {

            int PicOrderCntMsb = 0;
            evc_parse_ctx->poc.prevPicOrderCntVal = evc_parse_ctx->poc.PicOrderCntVal;

            if (nalu_type == EVC_IDR_NUT)
                PicOrderCntMsb = 0;
            else {
                int MaxPicOrderCntLsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);

                int prevPicOrderCntLsb = evc_parse_ctx->poc.PicOrderCntVal & (MaxPicOrderCntLsb - 1);
                int prevPicOrderCntMsb = evc_parse_ctx->poc.PicOrderCntVal - prevPicOrderCntLsb;


                if ((sh->slice_pic_order_cnt_lsb < prevPicOrderCntLsb) &&
                    ((prevPicOrderCntLsb - sh->slice_pic_order_cnt_lsb) >= (MaxPicOrderCntLsb / 2)))

                    PicOrderCntMsb = prevPicOrderCntMsb + MaxPicOrderCntLsb;

                else if ((sh->slice_pic_order_cnt_lsb > prevPicOrderCntLsb) &&
                         ((sh->slice_pic_order_cnt_lsb - prevPicOrderCntLsb) > (MaxPicOrderCntLsb / 2)))

                    PicOrderCntMsb = prevPicOrderCntMsb - MaxPicOrderCntLsb;

                else
                    PicOrderCntMsb = prevPicOrderCntMsb;
            }
            evc_parse_ctx->poc.PicOrderCntVal = PicOrderCntMsb + sh->slice_pic_order_cnt_lsb;

        } else {
            if (nalu_type == EVC_IDR_NUT) {
                evc_parse_ctx->poc.PicOrderCntVal = 0;
                evc_parse_ctx->poc.DocOffset = -1;
            } else {
                int SubGopLength = (int)pow(2.0, sps->log2_sub_gop_length);
                if (tid == 0) {
                    evc_parse_ctx->poc.PicOrderCntVal = evc_parse_ctx->poc.prevPicOrderCntVal + SubGopLength;
                    evc_parse_ctx->poc.DocOffset = 0;
                    evc_parse_ctx->poc.prevPicOrderCntVal = evc_parse_ctx->poc.PicOrderCntVal;
                } else {
                    int ExpectedTemporalId;
                    int PocOffset;
                    int prevDocOffset = evc_parse_ctx->poc.DocOffset;

                    evc_parse_ctx->poc.DocOffset = (prevDocOffset + 1) % SubGopLength;
                    if (evc_parse_ctx->poc.DocOffset == 0) {
                        evc_parse_ctx->poc.prevPicOrderCntVal += SubGopLength;
                        ExpectedTemporalId = 0;
                    } else
                        ExpectedTemporalId = 1 + (int)log2(evc_parse_ctx->poc.DocOffset);
                    while (tid != ExpectedTemporalId) {
                        evc_parse_ctx->poc.DocOffset = (evc_parse_ctx->poc.DocOffset + 1) % SubGopLength;
                        if (evc_parse_ctx->poc.DocOffset == 0)
                            ExpectedTemporalId = 0;
                        else
                            ExpectedTemporalId = 1 + (int)log2(evc_parse_ctx->poc.DocOffset);
                    }
                    PocOffset = (int)(SubGopLength * ((2.0 * evc_parse_ctx->poc.DocOffset + 1) / (int)pow(2.0, tid) - 2));
                    evc_parse_ctx->poc.PicOrderCntVal = evc_parse_ctx->poc.prevPicOrderCntVal + PocOffset;
                }
            }
        }

        break;
    }
    }

    return 0;
}

static void evc_frame_merge_flush(AVBSFContext *bsf)
{
    EVCMergeContext *ctx = bsf->priv_data;

    av_packet_unref(ctx->in);
}

static int evc_frame_merge_filter(AVBSFContext *bsf, AVPacket *out)
{
    EVCMergeContext *ctx = bsf->priv_data;
    AVPacket *in = ctx->in;

    int free_space = 0;
    size_t  nalu_size = 0;
    uint8_t *nalu = NULL;
    int au_end_found = 0;
    int err;

    err = ff_bsf_get_packet_ref(bsf, in);
    if (err < 0)
        return err;

    nalu_size = ff_evc_read_nal_unit_length(in->data, EVC_NALU_LENGTH_PREFIX_SIZE, bsf);
    if(nalu_size <= 0) {
        av_packet_unref(in);
        return AVERROR_INVALIDDATA;
    }

    nalu = in->data + EVC_NALU_LENGTH_PREFIX_SIZE;
    nalu_size = in->size - EVC_NALU_LENGTH_PREFIX_SIZE;

    // NAL unit parsing needed to determine if end of AU was found
    err = parse_nal_unit(nalu, nalu_size, bsf);
    if (err < 0) {
        av_log(bsf, AV_LOG_ERROR, "NAL Unit parsing error\n");
        av_packet_unref(in);

        return err;
    }

    au_end_found = end_of_access_unit_found(bsf);

    free_space = ctx->au_buffer.capacity - ctx->au_buffer.data_size;
    while( free_space < in->size ) {
        ctx->au_buffer.capacity *= 2;
        free_space = ctx->au_buffer.capacity - ctx->au_buffer.data_size;

        if(free_space >= in->size) {
            ctx->au_buffer.data = av_realloc(ctx->au_buffer.data, ctx->au_buffer.capacity);
        }
    }

    memcpy(ctx->au_buffer.data + ctx->au_buffer.data_size, in->data, in->size);

    ctx->au_buffer.data_size += in->size;

    av_packet_unref(in);

    if(au_end_found) {
        uint8_t *data = av_memdup(ctx->au_buffer.data, ctx->au_buffer.data_size);
        err = av_packet_from_data(out, data, ctx->au_buffer.data_size);

        ctx->au_buffer.data_size = 0;
    } else
        err = AVERROR(EAGAIN);

    if (err < 0 && err != AVERROR(EAGAIN))
        evc_frame_merge_flush(bsf);

    return err;
}

static int evc_frame_merge_init(AVBSFContext *bsf)
{
    EVCMergeContext *ctx = bsf->priv_data;

    ctx->in  = av_packet_alloc();
    if (!ctx->in)
        return AVERROR(ENOMEM);

    ctx->au_buffer.capacity = INIT_AU_BUF_CAPACITY;
    ctx->au_buffer.data = av_malloc(INIT_AU_BUF_CAPACITY);
    ctx->au_buffer.data_size = 0;

    return 0;
}

static void evc_frame_merge_close(AVBSFContext *bsf)
{
    EVCMergeContext *ctx = bsf->priv_data;

    av_packet_free(&ctx->in);

    ctx->au_buffer.capacity = 0;
    av_freep(&ctx->au_buffer.data);
    ctx->au_buffer.data_size = 0;
}

static const enum AVCodecID evc_frame_merge_codec_ids[] = {
    AV_CODEC_ID_EVC, AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_evc_frame_merge_bsf = {
    .p.name         = "evc_frame_merge",
    .p.codec_ids    = evc_frame_merge_codec_ids,
    .priv_data_size = sizeof(EVCMergeContext),
    .init           = evc_frame_merge_init,
    .flush          = evc_frame_merge_flush,
    .close          = evc_frame_merge_close,
    .filter         = evc_frame_merge_filter,
};