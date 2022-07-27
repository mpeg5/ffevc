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

#include <float.h>
#include <stdlib.h>

#include <xevd.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "libavutil/cpu.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"
#include "codec_internal.h"

#define XEVD_PARAM_BAD_NAME -1
#define XEVD_PARAM_BAD_VALUE -2

#define EVC_NAL_HEADER_SIZE 2 /* byte */

/**
 * The structure stores all the states associated with the instance of Xeve MPEG-5 EVC decoder
 */
typedef struct XevdContext {
    const AVClass *class;

    XEVD id;            // XEVD instance identifier @see xevd.h
    XEVD_CDSC cdsc;     // decoding parameters @see xevd.h

    int decod_frames;   // number of decoded frames
    int packet_count;   // number of packets created by decoder
} XevdContext;

/**
 * The function returns a pointer to a variable of type XEVD_CDSC.
 * XEVD_CDSC contains all decoder parameters that should be initialized before its use.
 *
 * The field values of the XEVD_CDSC structure are populated based on:
 * - the corresponding field values of the AvCodecConetxt structure,
 * - the xevd decoder specific option values,
 *   (the full list of options available for the xevd encoder is displayed after executing the command ./ffmpeg --help decoder = libxevd)
 * - and the xevd encoder options specified as a list of key-value pairs following the xevd-params option
 *
 * Order of input processing and populating the XEVD_CDSC structure
 * 1. first, the corresponding fields of the AVCodecContext structure are processed, (i.e -threads 4)
 * 2. then xevd-specific options are added as AVOption to the xevd AVCodec implementation (i.e -threads 3)
 * 3. finally, the options specified after the xevd-params option as the parameter list of type key value are processed (i.e -xevd-params "m=2")
 *
 * Some options can be set in different ways. In this case, please follow the above-mentioned order of processing.
 * The most recent assignments overwrite the previous values.
 *
 * @param[in] avctx codec context
 * @param[out] cdsc contains all encoder parameters that should be initialized before its use
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(AVCodecContext *avctx, XEVD_CDSC *cdsc)
{
    int cpu_count = av_cpu_count();

    /* clear XEVS_CDSC structure */
    memset(cdsc, 0, sizeof(XEVD_CDSC));

    /* init XEVD_CDSC */
    if (avctx->thread_count <= 0)
        cdsc->threads = (cpu_count < XEVD_MAX_TASK_CNT) ? cpu_count : XEVD_MAX_TASK_CNT;
    else if (avctx->thread_count > XEVD_MAX_TASK_CNT)
        cdsc->threads = XEVD_MAX_TASK_CNT;
    else
        cdsc->threads = avctx->thread_count;

    return 0;
}

/**
 * Read NAL unit length
 * @param bs input data (bitstream)
 * @return the length of NAL unit on success, 0 value on failure
 */
static uint32_t read_nal_unit_length(const uint8_t *bs, int bs_size, AVCodecContext *avctx)
{
    uint32_t len = 0;
    XEVD_INFO info;
    int ret;

    if (bs_size == XEVD_NAL_UNIT_LENGTH_BYTE) {
        ret = xevd_info((void *)bs, XEVD_NAL_UNIT_LENGTH_BYTE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return 0;
        }
        len = info.nalu_len;
        if (len == 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid bitstream size! [%d]\n", bs_size);
            return 0;
        }
    }

    return len;
}

/**
 * @param[in] xectx the structure that stores all the state associated with the instance of Xeve MPEG-5 EVC decoder
 * @param[out] avctx codec context
 * @return 0 on success, negative value on failure
 */
static int export_stream_params(const XevdContext *xectx, AVCodecContext *avctx)
{
    int ret;
    int size;
    int color_space;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P10;

    size = 4;
    ret = xevd_config(xectx->id, XEVD_CFG_GET_CODED_WIDTH, &avctx->coded_width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get coded_width\n");
        return AVERROR_EXTERNAL;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_CODED_HEIGHT, &avctx->coded_height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get coded_height\n");
        return AVERROR_EXTERNAL;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_WIDTH, &avctx->width, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get width\n");
        return AVERROR_EXTERNAL;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_HEIGHT, &avctx->height, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get height\n");
        return AVERROR_EXTERNAL;
    }

    ret = xevd_config(xectx->id, XEVD_CFG_GET_COLOR_SPACE, &color_space, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get color_space\n");
        return AVERROR_EXTERNAL;
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
        av_log(avctx, AV_LOG_ERROR, "Unknown color space\n");
        avctx->pix_fmt = AV_PIX_FMT_NONE;
        return AVERROR_INVALIDDATA;
    }

    // the function returns sps->num_reorder_pics
    ret = xevd_config(xectx->id, XEVD_CFG_GET_MAX_CODING_DELAY, &avctx->max_b_frames, &size);
    if (XEVD_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "Failed to get max_coding_delay\n");
        return AVERROR_EXTERNAL;
    }

    avctx->has_b_frames = (avctx->max_b_frames) ? 1 : 0;

    avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
    avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
    avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;

    return 0;
}

/**
 * Initialize decoder
 * Create a decoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libxevd_init(AVCodecContext *avctx)
{
    XevdContext *xectx = avctx->priv_data;
    int ret = 0;
    XEVD_CDSC *cdsc = &(xectx->cdsc);

    /* read configurations and set values for created descriptor (XEVD_CDSC) */
    ret = get_conf(avctx, cdsc);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Cannot get configuration\n");
        return AVERROR_INVALIDDATA;
    }

    /* create decoder */
    xectx->id = xevd_create(&(xectx->cdsc), NULL);
    if (xectx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create XEVD encoder\n");
        return AVERROR_EXTERNAL;
    }

    xectx->packet_count = 0;
    xectx->decod_frames = 0;

    return 0;
}

/**
  * Decode picture
  *
  * @param avctx codec context
  * @param[out] frame decoded frame
  * @param[out] got_frame_ptr decoder sets to 0 or 1 to indicate that a
  *                       non-empty frame or subtitle was returned in
  *                       outdata.
  * @param[in] avpkt AVPacket containing encoded data to be decoded
  *
  * @return amount of bytes read from the packet on success, negative error
  *         code on failure
  */
static int libxevd_decode(struct AVCodecContext *avctx, struct AVFrame *frame, int *got_frame_ptr, AVPacket *avpkt)
{
    XevdContext *xectx = NULL;
    XEVD_IMGB *imgb = NULL;
    XEVD_STAT stat;
    XEVD_BITB bitb;
    int xevd_ret, nalu_size, bs_read_pos;
    int ret = 0;

    if (avctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid input parameter: AVCodecContext\n");
        return AVERROR(EINVAL);
    }
    xectx = avctx->priv_data;
    if (xectx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid XEVD context\n");
        return AVERROR(EINVAL);
    }

    if (avpkt->size > 0) {
        bs_read_pos = 0;
        imgb = NULL;
        while(avpkt->size > (bs_read_pos + XEVD_NAL_UNIT_LENGTH_BYTE)) {

            memset(&stat, 0, sizeof(XEVD_STAT));
            memset(&bitb, 0, sizeof(XEVD_BITB));

            nalu_size = read_nal_unit_length(avpkt->data + bs_read_pos, XEVD_NAL_UNIT_LENGTH_BYTE, avctx);
            if (nalu_size == 0) {
                av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                ret = AVERROR_INVALIDDATA;
                goto ERR;
            }
            bs_read_pos += XEVD_NAL_UNIT_LENGTH_BYTE;

            bitb.addr = avpkt->data + bs_read_pos;
            bitb.ssize = nalu_size;

            /* main decoding block */
            xevd_ret = xevd_decode(xectx->id, &bitb, &stat);
            if (XEVD_FAILED(xevd_ret)) {
                av_log(avctx, AV_LOG_ERROR, "Failed to decode bitstream\n");
                ret = AVERROR_EXTERNAL;
                goto ERR;
            }

            bs_read_pos += nalu_size;

            if (stat.nalu_type == XEVD_NUT_SPS) { // EVC stream parameters changed
                if ((ret = export_stream_params(xectx, avctx)) != 0)
                    goto ERR;
            }

            if (stat.read != nalu_size)
                av_log(avctx, AV_LOG_INFO, "Different reading of bitstream (in:%d, read:%d)\n,", nalu_size, stat.read);
            if (stat.fnum >= 0) {
                // already has a decoded image
                if (imgb) {
                    // xevd_pull uses pool of objects of type XEVD_IMGB.
                    // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
                    imgb->release(imgb);
                    imgb = NULL;
                }
                xevd_ret = xevd_pull(xectx->id, &imgb);
                if (XEVD_FAILED(xevd_ret)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to pull the decoded image (xevd error code: %d, frame#=%d)\n", xevd_ret, stat.fnum);
                    ret = AVERROR_EXTERNAL;
                    goto ERR;
                } else if (xevd_ret == XEVD_OK_FRM_DELAYED) {
                    if (imgb) {
                        // xevd_pull uses pool of objects of type XEVD_IMGB.
                        // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
                        imgb->release(imgb);
                        imgb = NULL;
                    }
                }
            }
        }
    } else { // bumping
        xevd_ret = xevd_pull(xectx->id, &(imgb));
        if (xevd_ret == XEVD_ERR_UNEXPECTED) { // bumping process completed
            *got_frame_ptr = 0;
            return 0;
        } else if (XEVD_FAILED(xevd_ret)) {
            av_log(avctx, AV_LOG_ERROR, "Failed to pull the decoded image (xevd error code: %d)\n", xevd_ret);
            ret = AVERROR_EXTERNAL;
            goto ERR;
        }
    }

    if (imgb) {
        // @todo supports other color space and bit depth
        if (imgb->cs != XEVD_CS_YCBCR420_10LE) {
            av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
            ret = AVERROR_INVALIDDATA;
            goto ERR;
        }

        if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) { // stream resolution changed
            if (ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
                av_log(avctx, AV_LOG_ERROR, "Cannot set new dimension\n");
                ret = AVERROR_INVALIDDATA;
                goto ERR;
            }
        }

        frame->coded_picture_number++;
        frame->display_picture_number++;
        frame->format = AV_PIX_FMT_YUV420P10LE;

        if (ff_get_buffer(avctx, frame, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot get AV buffer\n");
            ret = AVERROR(ENOMEM);
            goto ERR;
        }

        frame->pts = avpkt->pts;

        av_image_copy(frame->data, frame->linesize, (const uint8_t **)imgb->a,
                      imgb->s, avctx->pix_fmt,
                      imgb->w[0], imgb->h[0]);

        xectx->decod_frames++;
        *got_frame_ptr = 1;

        // xevd_pull uses pool of objects of type XEVD_IMGB.
        // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
        imgb->release(imgb);
        imgb = NULL;
    } else
        *got_frame_ptr = 0;

    xectx->packet_count++;

    return avpkt->size;

ERR:
    if (imgb) {
        imgb->release(imgb);
        imgb = NULL;
    }
    *got_frame_ptr = 0;

    return ret;
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
    if (xectx->id) {
        xevd_delete(xectx->id);
        xectx->id = NULL;
    }

    return 0;
}

#define OFFSET(x) offsetof(XevdContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVClass libxevd_class = {
    .class_name = "libxevd",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_libxevd_decoder = {
    .p.name             = "evc",
    .p.long_name        = NULL_IF_CONFIG_SMALL("EVC / MPEG-5 Essential Video Coding (EVC)"),
    .p.type             = AVMEDIA_TYPE_VIDEO,
    .p.id               = AV_CODEC_ID_EVC,
    .init               = libxevd_init,
    FF_CODEC_DECODE_CB(libxevd_decode),
    .close              = libxevd_close,
    .priv_data_size     = sizeof(XevdContext),
    .p.priv_class       = &libxevd_class,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DR1,
    .p.wrapper_name     = "libxevd",
};
