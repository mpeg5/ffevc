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
#include "profiles.h"
#include "decode.h"
#include "libxevd_pq.h"
#include "libxevd_mp.h"

#define XEVD_PARAM_BAD_NAME -1
#define XEVD_PARAM_BAD_VALUE -2

#define EVC_NALU_HEADER_SIZE 2 /* byte */

/**
 * The structure stores all the states associated with the instance of Xeve MPEG-5 EVC decoder
 */
typedef struct XevdContext {
    const AVClass *class;

    XEVD id;            // XEVD instance identifier @see xevd.h
    XEVD_CDSC cdsc;     // decoding parameters @see xevd.h
    int coded_picture_number;
    struct EvcPriorityQueue* pq;
    struct EvcMemPool* mp_fm; // memory pool for FrameMetaData elements
    struct EvcMemPool* mp_pq; // memory pool for Protoity Queue elements
} XevdContext;

typedef struct FrameMetaData {
    int pts;                    // Presentation timestamp in time_base units
    int pkt_pts;                // PTS copied from the AVPacket that was decoded to produce this frame
    int64_t pkt_dts;            // DTS copied from the AVPacket that triggered returning this frame
    int coded_picture_number;   // picture number in bitstream order
    int display_picture_number; // picture number in display order
} FrameMetaData;

/**
 * The function populates the XEVD_CDSC structure.
 * XEVD_CDSC contains all decoder parameters that should be initialized before its use.
 *
 * @param[in] avctx codec context
 * @param[out] cdsc contains all decoder parameters that should be initialized before its use
 *
 */
static void get_conf(AVCodecContext *avctx, XEVD_CDSC *cdsc)
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
 * @brief Copy image in imgb to frame.
 *
 * @param avctx codec context
 * @param[in] imgb
 * @param[out] frame
 * @return 0 on success, negative value on failure
 */
static int libxevd_image_copy(struct AVCodecContext *avctx, XEVD_IMGB *imgb, struct AVFrame *frame)
{
    int ret;
    if (imgb->cs != XEVD_CS_YCBCR420_10LE) {
        av_log(avctx, AV_LOG_ERROR, "Not supported pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
        return AVERROR_INVALIDDATA;
    }

    if (imgb->w[0] != avctx->width || imgb->h[0] != avctx->height) { // stream resolution changed
        if (ff_set_dimensions(avctx, imgb->w[0], imgb->h[0]) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Cannot set new dimension\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (ret = ff_get_buffer(avctx, frame, 0) < 0)
        return ret;

    av_image_copy(frame->data, frame->linesize, (const uint8_t **)imgb->a,
                  imgb->s, avctx->pix_fmt,
                  imgb->w[0], imgb->h[0]);

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
    XEVD_CDSC *cdsc = &(xectx->cdsc);

    /* read configurations and set values for created descriptor (XEVD_CDSC) */
    get_conf(avctx, cdsc);

    /* create decoder */
    xectx->id = xevd_create(&(xectx->cdsc), NULL);
    if (xectx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create XEVD encoder\n");
        return AVERROR_EXTERNAL;
    }

    xectx->pq = evc_pq_create(20);
    xectx->mp_fm = evc_mp_create(20, sizeof(struct FrameMetaData));
    xectx->mp_pq = evc_mp_create(20, sizeof(struct EvcPriorityQueueElement));

    return 0;
}

/**
  * Decode frame with decoupled packet/frame dataflow
  *
  * @param avctx codec context
  * @param[out] frame decoded frame
  *
  * @return 0 on success, negative error code on failure
  */
static int libxevd_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    XevdContext *xectx = NULL;
    int xevd_ret;
    int ret = 0;

    xectx = avctx->priv_data;

    /* poll for new frame */
    {
        XEVD_IMGB *imgb = NULL;

        xevd_ret = xevd_pull(xectx->id, &imgb);
        if (XEVD_SUCCEEDED(ret)) {
            if (imgb) {

                struct FrameMetaData *frame_meta = NULL;
                int ret;
                struct EvcPriorityQueueElement* pq_element = evc_pq_dequeue(xectx->pq);

                frame_meta = ((struct FrameMetaData *)pq_element->value);

                frame->coded_picture_number = frame_meta->coded_picture_number;
                frame->display_picture_number = frame_meta->display_picture_number;
                // frame->pkt_dts = frame_meta->coded_picture_number;
                av_log(avctx, AV_LOG_ERROR, "************** frame->pts %ld \n",frame->pts);
                frame->pts = frame_meta->display_picture_number;

                av_log(avctx, AV_LOG_ERROR, "************** frame->pts %ld | %ld | %d\n",frame->pts, frame_meta->display_picture_number, frame->coded_picture_number);

                ret = libxevd_image_copy(avctx, imgb, frame);

                evc_mp_free(xectx->mp_fm, frame_meta);
                evc_mp_free(xectx->mp_pq, pq_element);

                // xevd_pull uses pool of objects of type XEVD_IMGB.
                // The pool size is equal MAX_PB_SIZE (26), so release object when it is no more needed
                imgb->release(imgb);
                imgb = NULL;
                return ret;
            }
        } else {
            if (imgb) { // already has a decoded image
                imgb->release(imgb);
                imgb = NULL;
            }
        }
    }

    /* feed decoder */
    {
        XEVD_STAT stat;
        XEVD_BITB bitb;
        int nalu_size, bs_read_pos, dec_read_bytes;

        AVPacket pkt = {0};

        pkt.data = NULL;
        pkt.size = 0;

        // obtain input data
        ret = ff_decode_get_packet(avctx, &pkt);
        if (ret < 0)   // no data is currently available or end of stream has been reached
            return ret;

        memset(&stat, 0, sizeof(XEVD_STAT));
        memset(&bitb, 0, sizeof(XEVD_BITB));

        if (pkt.size) {

            bs_read_pos = 0;
            dec_read_bytes = 0;
            while (pkt.size > (bs_read_pos + XEVD_NAL_UNIT_LENGTH_BYTE)) {

                nalu_size = read_nal_unit_length(pkt.data + bs_read_pos, XEVD_NAL_UNIT_LENGTH_BYTE, avctx);
                if (nalu_size == 0) {
                    av_log(avctx, AV_LOG_ERROR, "Invalid bitstream\n");
                    ret = AVERROR_INVALIDDATA;
                    goto ERR;
                }
                bs_read_pos += XEVD_NAL_UNIT_LENGTH_BYTE;

                bitb.addr = pkt.data + bs_read_pos;
                bitb.ssize = nalu_size;

                bitb.ts[0] = pkt.pts;
                bitb.ts[1] = pkt.dts;
                bitb.ts[2] = 0;
                bitb.ts[3] = 0;

                av_log(avctx, AV_LOG_ERROR, "pkt.pts %ld | pkt.dts %ld\n", pkt.pts, pkt.dts);

                /* main decoding block */
                xevd_ret = xevd_decode(xectx->id, &bitb, &stat);
                if (XEVD_FAILED(xevd_ret)) {
                    av_log(avctx, AV_LOG_ERROR, "Failed to decode bitstream\n");
                    ret = AVERROR_EXTERNAL;
                    goto ERR;
                }

                bs_read_pos += nalu_size;
                dec_read_bytes += nalu_size;

                if (stat.nalu_type == XEVD_NUT_SPS) { // EVC stream parameters changed
                    if ((ret = export_stream_params(xectx, avctx)) != 0)
                        goto ERR;
                } else if (stat.nalu_type == XEVD_NUT_IDR || stat.nalu_type == XEVD_NUT_NONIDR) {
                    // av_log(avctx, AV_LOG_INFO, "coded_picture_number: %d | stat.poc: %d\n", xectx->coded_picture_number, stat.poc);

                    struct FrameMetaData *frame_meta = (struct FrameMetaData*)evc_mp_alloc(xectx->mp_fm, sizeof(struct FrameMetaData), true);
                    struct EvcPriorityQueueElement* el = (struct EvcPriorityQueueElement*)evc_mp_alloc(xectx->mp_pq, sizeof(struct EvcPriorityQueueElement), true);

                    AVRational timebase;
                    timebase.num = 1;
                    timebase.den = avctx->pkt_timebase.den/pkt.duration; // avctx->pkt_timebase.den = 1200000 ; pkt.duration = 48000 for fps=25 => 1200000/48000=25

                    av_log(avctx, AV_LOG_INFO, "--- > pkt.pts: %ld | pkt.dts: %ld | pkt.duration: %ld | pkt.time_base: %d %d %d\n", pkt.pts, pkt.dts, pkt.duration, pkt.time_base.den, avctx->pkt_timebase.den, avctx->time_base.den);
                    av_packet_rescale_ts(&pkt, avctx->pkt_timebase, timebase);

                    frame_meta->coded_picture_number = xectx->coded_picture_number;
                    frame_meta->display_picture_number = stat.poc;
                    frame_meta->pkt_pts = stat.poc;
                    frame_meta->pkt_dts = xectx->coded_picture_number;
                    frame_meta->pts = pkt.pts;

                    av_log(avctx, AV_LOG_INFO, "*** > pkt.pts: %ld | pkt.dts: %ld | pkt.duration: %ld | avctx->framerate: %d %d %d\n", pkt.pts, pkt.dts, pkt.duration, avctx->framerate.num, avctx->pkt_timebase.den, avctx->time_base.den);
                    av_log(avctx, AV_LOG_INFO, "--- > delay: %ld \n", avctx->delay);
                    xectx->coded_picture_number++;

                    // el = (struct EvcPriorityQueueElement*)malloc(sizeof(EvcPriorityQueueElement));

                    el->key = stat.poc;
                    el->value = frame_meta;

                    evc_pq_enqueue(xectx->pq, el);
                }

                if (stat.read != dec_read_bytes) {
                    av_log(avctx, AV_LOG_INFO, "Different reading of bitstream (in:%d, read:%d)\n", nalu_size, stat.read);
                    ret = AVERROR_EXTERNAL;
                    goto ERR;
                }
            }
        }

ERR:
        av_packet_unref(&pkt);
        return ret;
    }

    return 0;
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
    evc_pq_destroy(xectx->pq);

    evc_mp_destroy(xectx->mp_fm);
    evc_mp_destroy(xectx->mp_pq);

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
    FF_CODEC_RECEIVE_FRAME_CB(libxevd_receive_frame),
    .close              = libxevd_close,
    .priv_data_size     = sizeof(XevdContext),
    .p.priv_class       = &libxevd_class,
    .p.capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS | AV_CODEC_CAP_AVOID_PROBING,
    .p.profiles         = NULL_IF_CONFIG_SMALL(ff_evc_profiles),
    .p.wrapper_name     = "libxevd",
    .caps_internal      = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_SETS_PKT_DTS,
};
