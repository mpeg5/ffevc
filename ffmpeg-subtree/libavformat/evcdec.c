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

#include "rawdec.h"
#include "avformat.h"

typedef struct EVCParserContext {
    int got_sps;
    int got_pps;
    int got_idr;
    int got_nonidr;
} EVCParserContext;

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

        bits += nalu_size;
        bytes_to_read -= nalu_size;

        if (nalu_type == EVC_SPS_NUT)
            ev->got_sps++;
        else if (nalu_type == EVC_PPS_NUT)
            ev->got_pps++;
        else if (nalu_type == EVC_IDR_NUT )
            ev->got_idr++;
        else if (nalu_type == EVC_NOIDR_NUT)
            ev->got_nonidr++;
    }

    return 0;
}

static int evc_probe(const AVProbeData *p)
{
    EVCParserContext ev = {0};
    int ret = parse_nal_units(p, &ev);

    if (ret == 0 && ev.got_sps && ev.got_pps && (ev.got_idr || ev.got_nonidr > 3))
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg

    return 0;
}

FF_DEF_RAWVIDEO_DEMUXER(evc, "raw EVC video", evc_probe, "evc", AV_CODEC_ID_EVC)
