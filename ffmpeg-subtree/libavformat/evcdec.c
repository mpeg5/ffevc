/*
 * RAW EVC video demuxer
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
#include "avformat.h"
#include "rawdec.h"
#include "libavcodec/internal.h"
#include <xevd.h>

#define MAX_BS_BUF (16*1024*1024) /* byte */

typedef enum _STATES
{
    STATE_DECODING,
    STATE_BUMPING
} STATES;

static void nal_unit_type(const XEVD_STAT * stat, int ret, int *sps, int *pps, int *sei, int *idr, int* sli)
{
    int i, j;

    if(XEVD_SUCCEEDED(ret))
    {
        if(stat->nalu_type < XEVD_NUT_SPS)
        {
            if(stat->nalu_type==XEVD_NUT_IDR) {
                (*idr)++;
            }
            av_log(NULL, AV_LOG_DEBUG, "%c-slice", stat->stype == XEVD_ST_I ? 'I' : stat->stype == XEVD_ST_P ? 'P' : 'B');
            av_log(NULL, AV_LOG_DEBUG, " (%d bytes", stat->read);

            av_log(NULL, AV_LOG_DEBUG, ", poc=%d, tid=%d, fnum=%d ", (int)stat->poc, (int)stat->tid, (int)stat->fnum);

            if(stat->stype == XEVD_ST_I || stat->stype == XEVD_ST_P || stat->stype == XEVD_ST_B) {
                (*sli)++;
            }

            for (i = 0; i < 2; i++)
            {
                av_log(NULL, AV_LOG_DEBUG, "[L%d ", i);
                for (j = 0; j < stat->refpic_num[i]; j++) av_log(NULL, AV_LOG_DEBUG,"%d ", stat->refpic[i][j]);
                av_log(NULL, AV_LOG_DEBUG, "] ");
            }
        }
        else if(stat->nalu_type == XEVD_NUT_SPS)
        {
            av_log(NULL, AV_LOG_DEBUG, "Sequence Parameter Set (%d bytes)\n", stat->read);
            (*sps)++;
        }
        else if (stat->nalu_type == XEVD_NUT_PPS)
        {
            av_log(NULL, AV_LOG_DEBUG, "Picture Parameter Set (%d bytes)\n", stat->read);
            (*pps)++;
        }
        else if (stat->nalu_type == XEVD_NUT_SEI)
        {
            av_log(NULL, AV_LOG_DEBUG, "SEI message: ");
            if (ret == XEVD_OK)
            {
                av_log(NULL, AV_LOG_DEBUG, "MD5 check OK\n");
            }
            else if (ret == XEVD_ERR_BAD_CRC)
            {
                av_log(NULL, AV_LOG_DEBUG, "MD5 check mismatch!\n");
            }
            else if (ret == XEVD_WARN_CRC_IGNORED)
            {
                av_log(NULL, AV_LOG_DEBUG, "MD5 check ignored!\n");
            }
            (*sei)++;
        }
        else
        {
            av_log(NULL, AV_LOG_ERROR, "Unknown bitstream\n");
        }
        av_log(NULL, AV_LOG_INFO, "\n");
    }
    else
    {
        av_log(NULL, AV_LOG_ERROR, "Decoding error = %d\n", ret);
    }
}

static int evc_probe(const AVProbeData *p)
{
    int op_parallel_task = 1;
    STATES state = STATE_DECODING;
    unsigned char *bs_buf = NULL;
    XEVD id = NULL;
    XEVD_CDSC cdsc;
    XEVD_BITB bitb;
    XEVD_IMGB *imgb;
    XEVD_STAT stat;
    XEVD_OPL opl;
    int ret;
    int sps = 0, pps = 0, sei = 0, idr = 0, sli = 0;
    int bd = 0;
    int bs_size, bs_read_pos = 0;

    bs_buf = malloc(MAX_BS_BUF);
    if(bs_buf == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "ERROR: cannot allocate bit buffer, size=%d\n", MAX_BS_BUF);
        return -1;
    }

    cdsc.threads = (int)op_parallel_task;

    id = xevd_create(&cdsc, NULL);
    if(id == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "ERROR: cannot create XEVD decoder\n");
        ret = -1;
        goto END;
    }

    while(1) {
        if (state == STATE_DECODING) {

            memset(&stat, 0, sizeof(XEVD_STAT));
            if(bs_read_pos + XEVD_NAL_UNIT_LENGTH_BYTE>p->buf_size) {
                state = STATE_BUMPING;
                av_log(NULL, AV_LOG_DEBUG, "bumping process starting...\n");
                continue;
            }
            memcpy(&bs_size, p->buf + bs_read_pos, XEVD_NAL_UNIT_LENGTH_BYTE);
            bs_read_pos += XEVD_NAL_UNIT_LENGTH_BYTE;

            if(bs_read_pos + bs_size > p->buf_size) {
                state = STATE_BUMPING;
                av_log(NULL, AV_LOG_DEBUG, "bumping process starting...\n");
                continue;
            }

            memcpy(bs_buf, p->buf + bs_read_pos, bs_size);
            bs_read_pos += bs_size;

            stat.read += XEVD_NAL_UNIT_LENGTH_BYTE;
            bitb.addr = bs_buf;
            bitb.ssize = bs_size;
            bitb.bsize = MAX_BS_BUF;

            /* main decoding block */
            ret = xevd_decode(id, &bitb, &stat);
            if(XEVD_FAILED(ret)) {
                av_log(NULL, AV_LOG_ERROR, "failed to decode bitstream\n");
                ret = -1;
                goto END;
            }
            nal_unit_type(&stat, ret, &sps, &pps, &sei, &idr, &sli);

            if(stat.read - XEVD_NAL_UNIT_LENGTH_BYTE != bs_size) {
                av_log(NULL, AV_LOG_ERROR, "\t=> different reading of bitstream (in:%d, read:%d)\n", bs_size, stat.read);
            }
        }
        if(stat.fnum >= 0 || state == STATE_BUMPING) {
            ret = xevd_pull(id, &imgb, &opl);
            if(ret == XEVD_ERR_UNEXPECTED) {
                av_log(NULL, AV_LOG_DEBUG, "bumping process completed\n");
                ret = 0;
                goto END;
            } else if(XEVD_FAILED(ret)) {
                av_log(NULL, AV_LOG_ERROR, "failed to pull the decoded image\n");
                ret = -1;
                goto END;
            }
        }
        else {
            imgb = NULL;
        }

        if(imgb) {
            int bit_depth = XEVD_CS_GET_BIT_DEPTH(imgb->cs);
            int w = imgb->w[0];
            int h = imgb->h[0];
            int cs_w_off, cs_h_off;
            int chroma_format;
            int b;

            av_log(NULL, AV_LOG_DEBUG, "bit depth: %d\n", bit_depth);
            av_log(NULL, AV_LOG_DEBUG, "width: %d\n", w);
            av_log(NULL, AV_LOG_DEBUG, "height: %d\n", h);

            av_log(NULL, AV_LOG_DEBUG, "bit depth: %d\n", bit_depth);
            av_log(NULL, AV_LOG_DEBUG, "imgb->aw[0]: %d\n", imgb->aw[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->aw[1]: %d\n", imgb->aw[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->aw[2]: %d\n", imgb->aw[2]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->ah[0]: %d\n", imgb->ah[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->ah[1]: %d\n", imgb->ah[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->ah[2]: %d\n", imgb->ah[2]);

            av_log(NULL, AV_LOG_DEBUG, "imgb->w[0]: %d\n", imgb->w[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->w[1]: %d\n", imgb->w[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->w[2]: %d\n", imgb->w[2]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->h[0]: %d\n", imgb->h[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->h[1]: %d\n", imgb->h[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->h[2]: %d\n", imgb->h[2]);

            av_log(NULL, AV_LOG_DEBUG, "imgb->a[0]: %p\n", imgb->a[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->a[1]: %p\n", imgb->a[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->a[2]: %p\n", imgb->a[2]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->baddr[0]: %p\n", imgb->baddr[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->baddr[1]: %p\n", imgb->baddr[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->baddr[2]: %p\n", imgb->baddr[2]);

            av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[0]: %d\n", imgb->bsize[0]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[1]: %d\n", imgb->bsize[1]);
            av_log(NULL, AV_LOG_DEBUG, "imgb->bsize[2]: %d\n", imgb->bsize[2]);

            chroma_format = XEVD_CS_GET_FORMAT(imgb->cs);

            b = (chroma_format == XEVD_CF_YCBCR420)?1:0;
            av_log(NULL, AV_LOG_DEBUG, "chroma_format == XEVD_CF_YCBCR420: %d\n", b);

            if (bit_depth == 8 && (chroma_format == XEVD_CF_YCBCR400 || chroma_format == XEVD_CF_YCBCR420 || chroma_format == XEVD_CF_YCBCR422 || chroma_format == XEVD_CF_YCBCR444)) {
                bd = 1;
                cs_w_off = 2;
                cs_h_off = 2;
            } else if (bit_depth >= 10 && bit_depth <= 14 && (chroma_format == XEVD_CF_YCBCR400 || chroma_format == XEVD_CF_YCBCR420 || chroma_format == XEVD_CF_YCBCR422 || chroma_format == XEVD_CF_YCBCR444)) {
                bd = 2;
                cs_w_off = 2;
                cs_h_off = 2;
            } else {
                av_log(NULL, AV_LOG_ERROR, "cannot support the color space\n");
                ret = -1;
                goto END;
            }

            av_log(NULL, AV_LOG_DEBUG, "bd: %d\n", bd);
            av_log(NULL, AV_LOG_DEBUG, "cs_w_off: %d\n", cs_w_off);
            av_log(NULL, AV_LOG_DEBUG, "cs_h_off: %d\n", cs_h_off);

            if (sps && pps && (idr || sli > 3)) {
                ret = 0;
                goto END;
            }

            imgb->release(imgb);
        }
    }

END:
    if(id) xevd_delete(id);
    if(bs_buf) free(bs_buf);
    if(imgb) imgb->release(imgb);

    if (ret == 0 && sps && pps && (idr || sli > 3)) {
        av_log(NULL, AV_LOG_DEBUG, "sps:=%d pps=%d idr=%d sli=%d\n",sps, pps,idr, sli);
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg
    }
    return ret;
}

FF_DEF_RAWVIDEO_DEMUXER(evc, "raw EVC video", evc_probe, "evc", AV_CODEC_ID_EVC)
