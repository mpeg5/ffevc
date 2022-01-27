/*
 * libxevd decoder
 *
 * Copyright (c) 2021 Dawid Kozinski
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

#if defined(_MSC_VER)
#define XEVD_API_IMPORTS 1
#endif

#include <xevd.h>

#include <float.h>
#include <stdlib.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"

// #define USE_EXP_GOLOMB_STUFF
#ifdef USE_EXP_GOLOMB_STUFF 
#include "golomb.h"
#endif

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"

#define UNUSED(x) (void)(x)

#define XEVD_PARAM_BAD_NAME -1
#define XEVD_PARAM_BAD_VALUE -2

/**
 * The structure stores all the state associated with the instance of Xeve MPEG-5 EVC decoder
 * The first field is a pointer to an AVClass struct (@see https://ffmpeg.org/doxygen/trunk/structAVClass.html#details).
 */
typedef struct XevdContext {
    const AVClass *class;

    XEVD id;        // XEVD instance identifier @see xevd.h
    XEVD_CDSC cdsc; // decoding parameters @see xevd.h
    XEVD_OPL opl;   // @see xevd.h

    int decod_frames; // number of decoded frames
    int packet_count; // number of packets created by decoder

    // configuration parameters
    AVDictionary *xevd_opts; // xevd configuration read from a :-separated list of key=value parameters

} XevdContext;

static int  op_threads = 1; // Default value

#ifdef USE_EXP_GOLOMB_STUFF 
static int get_nalu_type(const uint8_t *bs, int bs_size)
{
    GetBitContext gb;
    int fzb, nut;
    init_get_bits(&gb, bs, bs_size * 8);
    fzb = get_bits1(&gb);
    if(fzb != 0) {
        av_log(NULL, AV_LOG_DEBUG, "forbidden_zero_bit is not clear\n");
    }
    nut = get_bits(&gb, 6); /* nal_unit_type_plus1 */
    return nut - 1;
}
#endif

#ifdef PRINT_NALU_INFO
static void print_nalu_info(XEVD_STAT * stat)
{
    if(stat->nalu_type < XEVD_NUT_SPS) {
        av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_SPS \n");

        av_log(NULL, AV_LOG_DEBUG, "%c-slice\n", stat->stype == XEVD_ST_I ? 'I' : stat->stype == XEVD_ST_P ? 'P' : 'B');

        av_log(NULL, AV_LOG_DEBUG, " %d bytes\n", stat->read);
        av_log(NULL, AV_LOG_DEBUG, ", poc=%d, tid=%d, ", (int)stat->poc, (int)stat->tid);

        for (int i = 0; i < 2; i++) {
            av_log(NULL, AV_LOG_DEBUG, "[L%d ", i);
            for (int j = 0; j < stat->refpic_num[i]; j++) av_log(NULL, AV_LOG_DEBUG,"%d ", stat->refpic[i][j]);
            av_log(NULL, AV_LOG_DEBUG,"] \n");
        }
    } else if(stat->nalu_type == XEVD_NUT_SPS) {
        av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_SPS \n");
    } else if (stat->nalu_type == XEVD_NUT_PPS) {
        av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_PPS \n");
    } else if (stat->nalu_type == XEVD_NUT_SEI) {
        av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_SEI \n");
    } else {
        av_log(NULL, AV_LOG_DEBUG, "Unknown bitstream !!!! \n");
    }
}
#endif

// @todo consider moving following function to separate file containing helper functions for EVC decoder
#ifdef PRINT_FRAME_INFO
static void print_frame_info(const AVFrame* f)
{
    int level = AV_LOG_DEBUG;
    av_log(NULL, level, "frame->width: %d\n", f->width);
    av_log(NULL, level, "frame->height: %d\n", f->height);

    av_log(NULL, level, "frame->linesize[0]: %d\n", f->linesize[0]);
    av_log(NULL, level, "frame->linesize[1]: %d\n", f->linesize[1]);
    av_log(NULL, level, "frame->linesize[2]: %d\n", f->linesize[2]);
    av_log(NULL, level, "frame->buf[0]: %p\n", f->buf[0]);
    av_log(NULL, level, "frame->buf[1]: %p\n", f->buf[1]);
    av_log(NULL, level, "frame->buf[2]: %p\n", f->buf[2]);
    av_log(NULL, level, "frame->data[0]: %p\n", f->data[0]);
    av_log(NULL, level, "frame->data[1]: %p\n", f->data[1]);
    av_log(NULL, level, "frame->data[2]: %p\n", f->data[2]);
}
#endif

// @todo consider moving following function to separate file containing helper functions for EVC decoder
#ifdef PRINT_XEVD_IMGB_INFO
static void print_xevd_imgb_info(const XEVD_IMGB* imgb)
{
    av_log(NULL, AV_LOG_DEBUG, "imgb->np: %d\n", imgb->np);
    av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[0]: %d\n", imgb->bsize[0]);
    av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[1]: %d\n", imgb->bsize[1]);
    av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[2]: %d\n", imgb->bsize[2]);
}
#endif

// @todo consider moving following function to separate file containing helper functions for EVC decoder
#ifdef PRINT_AVCTX
static void print_avctx(const AVCodecContext *avctx)
{
    if( AVMEDIA_TYPE_UNKNOWN == avctx->codec_type) {
        av_log(NULL, AV_LOG_DEBUG, "avctx->codec_type: AVMEDIA_TYPE_UNKNOWN\n");
    } else if(AVMEDIA_TYPE_VIDEO  == avctx->codec_type)
        av_log(NULL, AV_LOG_DEBUG, "avctx->codec_type: AVMEDIA_TYPE_VIDEO \n");
    else {
        av_log(NULL, AV_LOG_DEBUG, "avctx->codec_type: AVMEDIA_TYPE_UNKNOWN\n");
    }

    av_log(NULL, AV_LOG_DEBUG, "avctx->codec_id: %s\n",avcodec_get_name(avctx->codec_id));
    av_log(NULL, AV_LOG_DEBUG, "avctx->width: %d\n", avctx->width);
    av_log(NULL, AV_LOG_DEBUG, "avctx->height: %d\n", avctx->height);
    av_log(NULL, AV_LOG_DEBUG, "avctx->pix_fmt: %d\n", avctx->pix_fmt);
}
#endif

/**
 * Read options
 *
 * @param avctx codec context
 * @return 0 on success
 */
static int read_options(const AVCodecContext* avctx)
{

    op_threads = (avctx->thread_count>0)?avctx->thread_count:1;

    return 0;
}

/**
 * Parse :-separated list of key=value parameters
 *
 * @param key
 * @param value
 *
 * @return 0 on success, negative value on failure
 *
 * @todo Consider removing the function
 */
static int xevd_params_parse(const char* key, const char* value)
{
    if(!key) {
        av_log(NULL, AV_LOG_ERROR, "Ivalid argument: key string is NULL\n");
        return XEVD_ERR_INVALID_ARGUMENT;
    }
    if(!value) {
        av_log(NULL, AV_LOG_ERROR, "Ivalid argument: value string is NULL\n");
        return XEVD_ERR_INVALID_ARGUMENT;
    }

    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown xevd codec option: %s\n", key);
        return XEVD_PARAM_BAD_NAME;
    }
    return 0;
}

/**
 *  The function returns a pointer to variable of type XEVD_CDSC.
 * XEVD_CDSC contains all decoder parameters that should be initialized before its use.
 *
 * The field values of the XEVD_CDSC structure are populated based on:
 * - the corresponding field values of the AvCodecConetxt structure,
 * - the xevd decoder specific option values,
 *   (the full list of options available for xevd encoder is displayed after executing the command ./ffmpeg --help decoder = libxevd)
 * - and the xevd encoder options specified as a list of key value pairs following xevd-params option
 *
 * Order of input processing and populating the XEVD_CDSC structure
 * 1. first, the corresponding fields of the AVCodecContext structure are processed, (i.e -threads 4)
 * 2. then xevd-specific options added as AVOption to the xevd AVCodec implementation (i.e -threads 3)
 * 3. finally, the options specified after the xevd-params option as the parameter list of type key value are processed (i.e -xevd-params "m=2")
 *
 * There are options that can be set in different ways. In this case, please follow the above-mentioned order of processing.
 * The most recent assignments overwrite the previous values.
 *
 * @param avctx codec context
 * @param cdsc contains all encoder parameters that should be initialized before its use.
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(const AVCodecContext* avctx, XEVD_CDSC* cdsc)
{
    int cpu_count = av_cpu_count();

    /* read options from AVCodecContext & from XEVD_CDSC */
    read_options(avctx);

    /* parse :-separated list of key=value parameters and set values for created descriptor (XEVD_CDSC) */
    {
        XevdContext *ctx = avctx->priv_data;
        AVDictionaryEntry *en = NULL;
        while ((en = av_dict_get(ctx->xevd_opts, "", en, AV_DICT_IGNORE_SUFFIX))) {
            int parse_ret = xevd_params_parse(en->key, en->value);

            switch (parse_ret) {
            case XEVD_PARAM_BAD_NAME:
                av_log((AVCodecContext*)avctx, AV_LOG_WARNING,
                       "Unknown option: %s.\n", en->key);
                break;
            case XEVD_PARAM_BAD_VALUE:
                av_log((AVCodecContext*)avctx, AV_LOG_WARNING,
                       "Invalid value for %s: %s.\n", en->key, en->value);
                break;
            default:
                break;
            }
        }
    }

    /* clear XEVS_CDSC structure */
    memset(cdsc, 0, sizeof(XEVD_CDSC));

    /* init XEVD_CDSC */
    if(avctx->thread_count <= 0) {
        cdsc->threads = (cpu_count<XEVD_MAX_TASK_CNT)?cpu_count:XEVD_MAX_TASK_CNT;
    } else if(avctx->thread_count > XEVD_MAX_TASK_CNT) {
        cdsc->threads = XEVD_MAX_TASK_CNT;
    } else {
        cdsc->threads = avctx->thread_count;
    }

    return XEVD_OK;
}

/**
 * Read NAL unit size
 * @param data input data
 * @return size of NAL unit on success, negative value on failure
 */
static int read_nal_unit_size(void * data)
{
    int nalu_size;
    memcpy(&nalu_size, data, XEVD_NAL_UNIT_LENGTH_BYTE);
    if(nalu_size <= 0) {
        return -1;
    }
    return nalu_size;
}

/**
 * @param avctx codec context
 * @param ctx the structure that stores all the state associated with the instance of Xeve MPEG-5 EVC decoder
 * @return 0 on success, negative value on failure
 */
static int export_stream_params(AVCodecContext *avctx, const XevdContext *ctx)
{
    // unsigned int num = 0, den = 0;
    // @todo support other formats

    int ret;
    int size;
    int color_space;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P10;

    // @todo The AVCodecContext should be initialized here using data from the object of XEVD_SPS type.
    //
    // It seems to be impossible right now since XEVD API limitation.
    // The extension for the XEVD API is needed.
    // To be more precise, what we need is access to the object of XEVD_SPS type being a part of XEVD_CTX object.
    // The object of XEVD_CTX type is created while the function xevd_create() being a part of public API is called.
    //
    // @todo remove the following hardoced has_b_frames; consider using sps->num_reorder_pics value instead
    //
    // avctx->has_b_frames        = 1; // (sps->num_reorder_pics)?1:0;
    size = 4;
    ret = xevd_config(ctx->id, XEVD_CFG_GET_CODED_WIDTH, &avctx->coded_width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(NULL, AV_LOG_ERROR, "failed to get coded_width\n");
        return -1;
    }

    ret = xevd_config(ctx->id, XEVD_CFG_GET_CODED_HEIGHT, &avctx->coded_height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(NULL, AV_LOG_ERROR, "failed to get coded_height\n");
        return -1;
    }

    ret = xevd_config(ctx->id, XEVD_CFG_GET_WIDTH, &avctx->width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(NULL, AV_LOG_ERROR, "failed to get width\n");
        return -1;
    }

    ret = xevd_config(ctx->id, XEVD_CFG_GET_HEIGHT, &avctx->height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(NULL, AV_LOG_ERROR, "failed to get height\n");
        return -1;
    }

    ret = xevd_config(ctx->id, XEVD_CFG_GET_COLOR_SPACE, &color_space, &size);
    if (XEVD_FAILED(ret)) {
        av_log(NULL, AV_LOG_ERROR, "failed to get color_space\n");
        return -1;
    }
    switch(color_space) {
    case XEVD_CS_YCBCR400_10LE:
        av_log(NULL, AV_LOG_DEBUG, "color_space = XEVD_CS_YCBCR400_10LE\n");
        avctx->pix_fmt = AV_PIX_FMT_GRAY10LE;
        break;
    case XEVD_CS_YCBCR420_10LE:
        av_log(NULL, AV_LOG_DEBUG, "color_space = XEVD_CS_YCBCR420_10LE\n");
        avctx->pix_fmt = AV_PIX_FMT_YUV420P10LE;
        break;
    case XEVD_CS_YCBCR422_10LE:
        av_log(NULL, AV_LOG_DEBUG, "color_space = XEVD_CS_YCBCR422_10LE\n");
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    case XEVD_CS_YCBCR444_10LE:
        av_log(NULL, AV_LOG_DEBUG, "color_space = XEVD_CS_YCBCR444_10LE\n");
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
    default:
        av_log(NULL, AV_LOG_ERROR, "unknown color space\n");
        avctx->pix_fmt = AV_PIX_FMT_NONE;
        return -1;
    }

// @todo Use _XEVD_SPS fields to initialize AVCodecContext when it is possible
#ifdef USE_XEVD_SPS_FIELDS
    avctx->profile = sps->profile_idc;
    avctx->level = sps->level_idc;

    ff_set_sar(avctx, sps->vui_parameters.sar);

    if (sps->vui_parametersvui.video_signal_type_present_flag)
        avctx->color_range = sps->vui_parameters.video_full_range_flag ? AVCOL_RANGE_JPEG
                             : AVCOL_RANGE_MPEG;
    else
        avctx->color_range = AVCOL_RANGE_MPEG;

    if (sps->vui_parameters.colour_description_present_flag) {
        avctx->color_primaries = sps->vui_parameters.colour_primaries;
        avctx->color_trc       = sps->vui_parameters.transfer_characteristic;
        avctx->colorspace      = sps->vui_parameters.matrix_coeffs;
    } else {
        avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
        avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
        avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
    }

    if (sps->vui_parameters.timing_info_present_flag) {
        num = sps->vui_parameters.num_units_in_tick;
        den = sps->vui_parameters.time_scale;
    }

    if (s->sei.alternative_transfer.present &&
            av_color_transfer_name(s->sei.alternative_transfer.preferred_transfer_characteristics) &&
            s->sei.alternative_transfer.preferred_transfer_characteristics != AVCOL_TRC_UNSPECIFIED) {
        avctx->color_trc = s->sei.alternative_transfer.preferred_transfer_characteristics;
    }
#else
    avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
    avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
    avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;

#endif
    return 0;
}

/**
 * Initialize decoder static data
 *
 * @todo Consider removing unused function
 */
static av_cold void libxevd_init_static_data(AVCodec *codec)
{
    UNUSED(codec);
}

/**
 * Initialize decoder
 * Create decoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libxevd_init(AVCodecContext *avctx)
{
    XevdContext *ctx = avctx->priv_data;
    int val = 0;
    XEVD_CDSC *cdsc = &(ctx->cdsc);

    av_log(NULL, AV_LOG_DEBUG, "eXtra-fast Essential Video Decoder\n");
#ifdef PRINT_AVCTX
    print_avctx(avctx);
#endif

    /* read configurations and set values for created descriptor (XEVD_CDSC) */
    val = get_conf(avctx, cdsc);
    if (val != XEVD_OK) {
        av_log(NULL, AV_LOG_ERROR,"Cannot get configuration\n");
        return -1;
    }

    /* create decoder */
    ctx->id = xevd_create(&(ctx->cdsc), NULL);
    if(ctx->id == NULL) {
        av_log(NULL, AV_LOG_ERROR, "cannot create XEVD encoder\n");
        return -1;
    }

    ctx->packet_count = 0;
    ctx->decod_frames = 0;
    return 0;
}

/**
  * Dncode picture
  *
  * @param      avctx          codec context
  * @param      data           codec type dependent output struct
  * @param[out] got_frame      decoder sets to 0 or 1 to indicate that a
  *                            non-empty frame or subtitle was returned in
  *                            outdata.
  * @param[in]  pkt            AVPacket containing the data to be decoded
  * @return amount of bytes read from the packet on success, negative error
  *         code on failure
  */
static int libxevd_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *pkt)
{
    AVFrame *frame = data;
    XevdContext *ctx = NULL;
    XEVD_IMGB * imgb = NULL;
    XEVD_STAT stat;
    XEVD_BITB bitb;
    int ret, nalu_size, bs_read_pos;

    if(avctx == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Invalid input parameter: AVCodecContext\n");
        return -1;
    }
    ctx = avctx->priv_data;
    if(ctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid XEVD context\n");
        return -1;
    }

    if(pkt->size > 0) {
        bs_read_pos = 0;
        imgb = NULL;
        while(pkt->size > (bs_read_pos + XEVD_NAL_UNIT_LENGTH_BYTE)) {
            int nal_type = 0;
            
            memset(&stat, 0, sizeof(XEVD_STAT));
            memset(&bitb, 0, sizeof(XEVD_BITB));

            nalu_size = read_nal_unit_size(pkt->data + bs_read_pos);
            if(nalu_size <= 0) {
                av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                goto ERR;
            }
            bs_read_pos += XEVD_NAL_UNIT_LENGTH_BYTE;

            bitb.addr = pkt->data + bs_read_pos;
            bitb.ssize = nalu_size;

            // Read NAL Unit Type from  NAL Unit Header
            //
            // The structure of NAL Unit Header looks like follows
            //
            // +---------------+---------------+
            // |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |F|   Type    | TID | Reserve |E|
            // +-------------+-----------------+
            //
            // F:       1 bit   - forbidden_zero_bit.  Required to be zero in [EVC].
            // Type:    6 bits  - nal_unit_type_plus1 (This field specifies the NAL unit type as defined in Table 4 of [EVC])
            // TID:     3 bits  - nuh_temporal_id.  This field specifies the temporal identifier of the NAL unit.
            // Reserve: 5 bits  - nuh_reserved_zero_5bits.  This field shall be equal to the version of the [EVC] specification.
            // E:       1 bit   - nuh_extension_flag.  This field shall be equal the version of the [EVC] specification.
            //
            // @see https://datatracker.ietf.org/doc/html/draft-ietf-avtcore-rtp-evc-01#section-1.1.4
            
#ifdef USE_EXP_GOLOMB_STUFF 
            nal_type = get_nalu_type(bitb.addr, 1);
            av_log(avctx, AV_LOG_DEBUG, "NALU Type: %d\n", nal_type);
#else
            memcpy(&nal_type,bitb.addr,1);
            nal_type = nal_type & 0x7E;
            nal_type = nal_type >> 1;
            nal_type -= 1;
            av_log(avctx, AV_LOG_DEBUG, "NALU Type: %d\n", nal_type);
#endif
            // Since XEVD decoder crashes when it receives a stream after changing its parameters,
            // we need to delete the previous XEVD decoder instance and create a new one when 
            // the stream changes.
            if(nal_type==XEVD_NUT_SPS) {
                if(ctx->id) {
                    xevd_delete(ctx->id);
                    ctx->id = NULL;
                }
                /* create decoder */
                ctx->id = xevd_create(&(ctx->cdsc), NULL);
                if(ctx->id == NULL) {
                    av_log(NULL, AV_LOG_ERROR, "cannot create XEVD encoder\n");
                    return -1;
                }
            }

            /* main decoding block */
            ret = xevd_decode(ctx->id, &bitb, &stat);
            if(XEVD_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR, "failed to decode bitstream\n");
                goto ERR;
            }

            bs_read_pos += nalu_size;

#ifdef PRINT_NALU_INFO
            print_nalu_info(ctx);
#endif

            if(stat.nalu_type == XEVD_NUT_SPS) {
                av_log(avctx, AV_LOG_DEBUG, "EVC stream parameters changed\n");

                if(export_stream_params(avctx, ctx)!=0) {
                    goto ERR;
                }
                av_log(avctx, AV_LOG_DEBUG, "width: %d\n",avctx->width);
                av_log(avctx, AV_LOG_DEBUG, "height: %d\n",avctx->height);

            }

            if(stat.read != nalu_size) {
                av_log(avctx, AV_LOG_INFO, "different reading of bitstream (in:%d, read:%d)\n,", nalu_size, stat.read);
            }
            if(stat.fnum >= 0) {
                if (imgb) { /* already has a decoded image */
                    imgb->release(imgb);
                    imgb = NULL;
                }
                ret = xevd_pull(ctx->id, &imgb, &(ctx->opl));
                if(XEVD_FAILED(ret)) {
                    av_log(avctx, AV_LOG_ERROR, "failed to pull the decoded image (err:%d, frame#=%d)\n", ret, stat.fnum);
                    goto ERR;
                } else if (ret == XEVD_OK_FRM_DELAYED) {
                    av_log(avctx, AV_LOG_DEBUG, "delayed frame\n");
                    if(imgb) {
                        imgb->release(imgb);
                        imgb = NULL;
                    }
                }
            }
        }
    } else {
        av_log(NULL, AV_LOG_DEBUG, "bumping ...\n");
        ret = xevd_pull(ctx->id, &(imgb), &(ctx->opl));
        if(ret == XEVD_ERR_UNEXPECTED) {
            av_log(avctx, AV_LOG_DEBUG, "Bumping process completed\n");
            *got_frame = 0;
            return 0;
        } else if(XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "failed to pull the decoded image (err:%d)\n", ret);
            goto ERR;
        } else {
            av_log(avctx, AV_LOG_DEBUG, "bumping success\n");
        }
    }

    if(imgb) {
        /* @todo supports other color space and bit depth */
        if(imgb->cs != XEVD_CS_YCBCR420_10LE) {
            av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
            goto ERR;
        }

        if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) {
            av_log(avctx, AV_LOG_DEBUG, "resolution changed %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, imgb->w[0], imgb->h[0]);
            if(ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
                av_log(avctx, AV_LOG_ERROR, "cannot set new dimension\n");
                goto ERR;
            }
        }

        frame->coded_picture_number++;
        frame->display_picture_number++;
        frame->format = AV_PIX_FMT_YUV420P10LE;

#ifdef PRINT_XEVD_IMGB_INFO
        print_xevd_imgb_info(imgb);
#endif

        if (ff_get_buffer(avctx, frame, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "cannot get AV buffer\n");
            goto ERR;
        }

        frame->pts = pkt->pts;
        av_log(avctx, AV_LOG_DEBUG, "frame->pts = %ld\n", frame->pts);

        av_image_copy(frame->data, frame->linesize, (const uint8_t **)imgb->a,
                      imgb->s, avctx->pix_fmt,
                      imgb->w[0], imgb->h[0]);

        ctx->decod_frames++;
        *got_frame = 1;

#ifdef PRINT_FRAME_INFO
        print_frame_info(frame);
#endif
        imgb->release(imgb);
        imgb = NULL;
    } else {
        *got_frame = 0;
    }
 
    ctx->packet_count++;
    return pkt->size;

ERR:
    if(imgb) {
        imgb->release(imgb);
        imgb = NULL;
    }
    *got_frame = 0;
    return -1;
}

/**
 * Destroy decoder
 *
 * @param avctx codec context
 * @return 0 on success
 */
static av_cold int libxevd_close(AVCodecContext *avctx)
{
    XevdContext *ctx = avctx->priv_data;
    if(ctx->id) {
        xevd_delete(ctx->id);
        ctx->id = NULL;
    }

    return 0;
}

#define OFFSET(x) offsetof(XevdContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

// @todo consider using following options (./ffmpeg --help decoder=libxevd)
//
static const AVOption options[] = {
    { "xevd-params",                "override the xevd configuration using a :-separated list of key=value parameters", OFFSET(xevd_opts), AV_OPT_TYPE_DICT,   { 0 }, 0, 0, VD },
    { NULL }
};

static const AVClass xevd_class = {
    .class_name = "libxevd",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/// @todo provide implementation
static const AVCodecDefault xevd_defaults[] = {
    { "b", "0" },
    { NULL },
};

AVCodec ff_libxevd_decoder = {
    .name             = "evc",
    .long_name        = NULL_IF_CONFIG_SMALL("EVC / MPEG-5 Essential Video Coding (EVC)"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_EVC,
    .init             = libxevd_init,
    .init_static_data = libxevd_init_static_data,
    .decode           = libxevd_decode,
    .close            = libxevd_close,
    .priv_data_size   = sizeof(XevdContext),
    .priv_class       = &xevd_class,
    .defaults         = xevd_defaults,
    .capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS | AV_CODEC_CAP_AVOID_PROBING,
    .wrapper_name     = "libxevd",
};
