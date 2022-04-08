/*
 * libxevd decoder
 * EVC (MPEG-5 Essential Video Coding) decoding using XEVD MPEG-5 EVC decoder library
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

#if defined(_MSC_VER)
#define XEVD_API_IMPORTS 1
#endif

#include <float.h>
#include <stdlib.h>

#include <xevd.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"

#define UNUSED(x) (void)(x)

#define XEVD_PARAM_BAD_NAME -1
#define XEVD_PARAM_BAD_VALUE -2

#define EVC_NAL_HEADER_SIZE 2 /* byte */

/**
 * The structure stores all the state associated with the instance of Xeve MPEG-5 EVC decoder
 * The first field is a pointer to an AVClass struct (@see https://ffmpeg.org/doxygen/trunk/structAVClass.html#details).
 */
typedef struct XevdContext {
    const AVClass *class;

    XEVD id;            // XEVD instance identifier @see xevd.h
    XEVD_CDSC cdsc;     // decoding parameters @see xevd.h

    int decod_frames;   // number of decoded frames
    int packet_count;   // number of packets created by decoder

    // configuration parameters
    AVDictionary *xevd_opts; // xevd configuration read from a :-separated list of key=value parameters

} XevdContext;

/**
 * The function returns a pointer to variable of type XEVD_CDSC.
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
 * @param[in] avctx codec context
 * @param[out] cdsc contains all encoder parameters that should be initialized before its use.
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(AVCodecContext *avctx, XEVD_CDSC *cdsc)
{
    int cpu_count = av_cpu_count();

    /* clear XEVS_CDSC structure */
    memset(cdsc, 0, sizeof(XEVD_CDSC));

    /* init XEVD_CDSC */
    if(avctx->thread_count <= 0)
        cdsc->threads = (cpu_count < XEVD_MAX_TASK_CNT) ? cpu_count : XEVD_MAX_TASK_CNT;
    else if(avctx->thread_count > XEVD_MAX_TASK_CNT)
        cdsc->threads = XEVD_MAX_TASK_CNT;
    else
        cdsc->threads = avctx->thread_count;

    return XEVD_OK;
}

/**
 * Read NAL unit length
 * @param bs input data (bitstream)
 * @return the lenghth of NAL unit on success, 0 value on failure
 */
static uint32_t read_nal_unit_length(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    uint32_t len = 0;
    XEVD_INFO info;
    int ret;

    if(bs_size == XEVD_NAL_UNIT_LENGTH_BYTE) {
        ret = xevd_info((void *)bs, XEVD_NAL_UNIT_LENGTH_BYTE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return 0;
        }
        len = info.nalu_len;
        if(len == 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid bitstream size! [%d]\n", bs_size);
            return 0;
        }
    }

    return len;
}

/**
 * @param avctx codec context
 * @param xectx the structure that stores all the state associated with the instance of Xeve MPEG-5 EVC decoder
 * @return 0 on success, negative value on failure
 */
static int export_stream_params(AVCodecContext *avctx, const XevdContext *xectx)
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
    ret = xevd_config(xectx->id, XEVD_CFG_GET_CODED_WIDTH, &avctx->coded_width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get coded_width\n");
        return -1;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_CODED_HEIGHT, &avctx->coded_height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get coded_height\n");
        return -1;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_WIDTH, &avctx->width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get width\n");
        return -1;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_HEIGHT, &avctx->height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get height\n");
        return -1;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_COLOR_SPACE, &color_space, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get color_space\n");
        return -1;
    }
    switch(color_space) {
    case XEVD_CS_YCBCR400_10LE:
        avctx->pix_fmt = AV_PIX_FMT_GRAY10LE;
        break;
    case XEVD_CS_YCBCR420_10LE:
        avctx->pix_fmt = AV_PIX_FMT_YUV420P10LE;
        break;
    case XEVD_CS_YCBCR422_10LE:
        avctx->pix_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    case XEVD_CS_YCBCR444_10LE:
        avctx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "unknown color space\n");
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
        s->sei.alternative_transfer.preferred_transfer_characteristics != AVCOL_TRC_UNSPECIFIED)
        avctx->color_trc = s->sei.alternative_transfer.preferred_transfer_characteristics;
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
    XevdContext *xectx = avctx->priv_data;
    int val = 0;
    XEVD_CDSC *cdsc = &(xectx->cdsc);

    /* read configurations and set values for created descriptor (XEVD_CDSC) */
    val = get_conf(avctx, cdsc);
    if (val != XEVD_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot get configuration\n");
        return -1;
    }

    /* create decoder */
    xectx->id = xevd_create(&(xectx->cdsc), NULL);
    if(xectx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "cannot create XEVD encoder\n");
        return -1;
    }

    xectx->packet_count = 0;
    xectx->decod_frames = 0;

    return 0;
}

/**
  * Decode picture
  *
  * @param avctx codec context
  * @param[out] data decoded frame
  * @param[out] got_frame decoder sets to 0 or 1 to indicate that a
  *                       non-empty frame or subtitle was returned in
  *                       outdata.
  * @param[in] pkt AVPacket containing encoded data to be decoded
  * 
  * @return amount of bytes read from the packet on success, negative error
  *         code on failure
  */
static int libxevd_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *pkt)
{
    AVFrame *frame = data;
    XevdContext *xectx = NULL;
    XEVD_IMGB *imgb = NULL;
    XEVD_STAT stat;
    XEVD_BITB bitb;
    int ret, nalu_size, bs_read_pos;

    if(avctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid input parameter: AVCodecContext\n");
        return -1;
    }
    xectx = avctx->priv_data;
    if(xectx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid XEVD context\n");
        return -1;
    }

    if(pkt->size > 0) {
        bs_read_pos = 0;
        imgb = NULL;
        while(pkt->size > (bs_read_pos + XEVD_NAL_UNIT_LENGTH_BYTE)) {

            memset(&stat, 0, sizeof(XEVD_STAT));
            memset(&bitb, 0, sizeof(XEVD_BITB));

            nalu_size = read_nal_unit_length(pkt->data + bs_read_pos, XEVD_NAL_UNIT_LENGTH_BYTE, avctx);
            if(nalu_size == 0) {
                av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                goto ERR;
            }
            bs_read_pos += XEVD_NAL_UNIT_LENGTH_BYTE;

            bitb.addr = pkt->data + bs_read_pos;
            bitb.ssize = nalu_size;

            /* main decoding block */
            ret = xevd_decode(xectx->id, &bitb, &stat);
            if(XEVD_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR, "failed to decode bitstream\n");
                goto ERR;
            }

            bs_read_pos += nalu_size;

            if(stat.nalu_type == XEVD_NUT_SPS) { // EVC stream parameters changed
                if(export_stream_params(avctx, xectx) != 0)
                    goto ERR;
            }

            if(stat.read != nalu_size)
                av_log(avctx, AV_LOG_INFO, "different reading of bitstream (in:%d, read:%d)\n,", nalu_size, stat.read);
            if(stat.fnum >= 0) {
                if (imgb) { /* already has a decoded image */
                    imgb->release(imgb);
                    imgb = NULL;
                }
                ret = xevd_pull(xectx->id, &imgb);
                if(XEVD_FAILED(ret)) {
                    av_log(avctx, AV_LOG_ERROR, "failed to pull the decoded image (xevd error code: %d, frame#=%d)\n", ret, stat.fnum);
                    goto ERR;
                } else if (ret == XEVD_OK_FRM_DELAYED) {
                    if(imgb) {
                        imgb->release(imgb);
                        imgb = NULL;
                    }
                }
            }
        }
    } else { // bumping
        ret = xevd_pull(xectx->id, &(imgb));
        if(ret == XEVD_ERR_UNEXPECTED) { // bumping process completed
            *got_frame = 0;
            return 0;
        } else if(XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "failed to pull the decoded image (xevd error code: %d)\n", ret);
            goto ERR;
        }
    }

    if(imgb) {
        // @todo supports other color space and bit depth
        if(imgb->cs != XEVD_CS_YCBCR420_10LE) {
            av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
            goto ERR;
        }

        if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) { // stream resolution changed
            if(ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
                av_log(avctx, AV_LOG_ERROR, "cannot set new dimension\n");
                goto ERR;
            }
        }

        frame->coded_picture_number++;
        frame->display_picture_number++;
        frame->format = AV_PIX_FMT_YUV420P10LE;

        if (ff_get_buffer(avctx, frame, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "cannot get AV buffer\n");
            goto ERR;
        }

        frame->pts = pkt->pts;

        av_image_copy(frame->data, frame->linesize, (const uint8_t **)imgb->a,
                      imgb->s, avctx->pix_fmt,
                      imgb->w[0], imgb->h[0]);

        xectx->decod_frames++;
        *got_frame = 1;

        imgb->release(imgb);
        imgb = NULL;
    } else
        *got_frame = 0;

    xectx->packet_count++;
    
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
    XevdContext *xectx = avctx->priv_data;
    if(xectx->id) {
        xevd_delete(xectx->id);
        xectx->id = NULL;
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
