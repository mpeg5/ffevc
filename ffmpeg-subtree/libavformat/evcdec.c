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
#include "avformat.h"
#include "rawdec.h"
#include "libavcodec/internal.h"
#include "xevd.h"

typedef struct EVCParserContext {
    int got_sps;
    int got_pps;
    int got_idr;
    int got_nonidr;
} EVCParserContext;

static int get_nalu_type(const uint8_t *bs, int bs_size)
{
    GetBitContext gb;
    int fzb, nut;
    int ret;

    if((ret=init_get_bits8(&gb, bs, bs_size)) < 0) {
        return ret;
    }

    fzb = get_bits1(&gb);
    if(fzb != 0) {
        av_log(NULL, AV_LOG_DEBUG, "forbidden_zero_bit is not clear\n");
    }
    nut = get_bits(&gb, 6); /* nal_unit_type_plus1 */
    return nut - 1;
}

/**
 * Read NAL unit length
 * @param bs input data (bitstream)
 * @return the lenghth of NAL unit on success, 0 value on failure
 */
static uint32_t read_nal_unit_length(const uint8_t *bs, int bs_size)
{
    uint32_t len = 0;
    XEVD_INFO info;
    int ret;

    if(bs_size>=XEVD_NAL_UNIT_LENGTH_BYTE) {
        ret = xevd_info((void*)bs, XEVD_NAL_UNIT_LENGTH_BYTE, 1, &info);
        if (XEVD_FAILED(ret)) {
            av_log(NULL, AV_LOG_ERROR, "Cannot get bitstream information\n");
            return 0;
        }
        len = info.nalu_len;
        if(len == 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Invalid bitstream size! [%d]\n", bs_size);
            return 0;
        }
    }
    return len;
}

static int parse_nal_units(const AVProbeData *p, EVCParserContext *ev)
{
    int nalu_type;
    size_t nalu_size;
    unsigned char* bits = (unsigned char *)p->buf;
    int bytes_to_read = p->buf_size;
    
    av_log(NULL, AV_LOG_DEBUG, "bytes_to_read: %d \n", bytes_to_read);

    while(bytes_to_read > XEVD_NAL_UNIT_LENGTH_BYTE) {
    
        nalu_size = read_nal_unit_length(bits, XEVD_NAL_UNIT_LENGTH_BYTE);
        if(nalu_size == 0) break;

        bits += XEVD_NAL_UNIT_LENGTH_BYTE;
        bytes_to_read -= XEVD_NAL_UNIT_LENGTH_BYTE;

        av_log(NULL, AV_LOG_DEBUG, "nalu_size: %ld \n", nalu_size);

        if(bytes_to_read < nalu_size) break;

        nalu_type = get_nalu_type(bits, bytes_to_read);

        bits += nalu_size;
        bytes_to_read -= nalu_size;

        if (nalu_type == XEVD_NUT_SPS) {
            av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_SPS \n");
            ev->got_sps++;
        }
        else if (nalu_type == XEVD_NUT_PPS) {
            av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_PPS \n");
            ev->got_pps++;
        }
        else if (nalu_type == XEVD_NUT_IDR ) {
            av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_IDR\n");
            ev->got_idr++;
        }
        else if (nalu_type == XEVD_NUT_NONIDR) {
            av_log(NULL, AV_LOG_DEBUG, "XEVD_NUT_NONIDR\n");
            ev->got_nonidr++;
        }
    }
    return 0;
}

static int evc_probe(const AVProbeData *p)
{
    EVCParserContext ev = {};
    int ret = parse_nal_units(p, &ev);

    av_log(NULL, AV_LOG_DEBUG, "sps:%d pps:%d idr:%d sli:%d\n", ev.got_sps, ev.got_pps, ev.got_idr, ev.got_nonidr);
    
    if (ret == 0 && ev.got_sps && ev.got_pps && (ev.got_idr || ev.got_nonidr > 3)) 
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg

    return 0;
}

FF_DEF_RAWVIDEO_DEMUXER(evc, "raw EVC video", evc_probe, "evc", AV_CODEC_ID_EVC)
