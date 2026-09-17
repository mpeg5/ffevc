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

#include "libavcodec/evc.h"
#include "libavcodec/bsf.h"

#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "evc.h"
#include "internal.h"


#define RAW_PACKET_SIZE 1024

typedef struct EVCDemuxContext {
    const AVClass *class;
    AVRational framerate;

    AVBSFContext *bsf;
    int64_t au_count;

    // Cached parameter set NAL units (with length prefixes, as found in the
    // stream). They are re-injected after a seek because a raw EVC stream
    // carries SPS/PPS only once, before the first picture.
    uint8_t *sps_buf;
    size_t   sps_size;
    uint8_t *pps_buf;
    size_t   pps_size;

} EVCDemuxContext;

#define DEC AV_OPT_FLAG_DECODING_PARAM
#define OFFSET(x) offsetof(EVCDemuxContext, x)
static const AVOption evc_options[] = {
    { "framerate", "", OFFSET(framerate), AV_OPT_TYPE_VIDEO_RATE, {.str = "25"}, 0, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

static const AVClass evc_demuxer_class = {
    .class_name = "EVC Annex B demuxer",
    .item_name  = av_default_item_name,
    .option     = evc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int annexb_probe(const AVProbeData *p)
{
    int nalu_type;
    size_t nalu_size;
    int got_sps = 0, got_pps = 0, got_idr = 0, got_nonidr = 0;
    const unsigned char *bits = p->buf;
    int bytes_to_read = p->buf_size;

    while (bytes_to_read > EVC_NALU_LENGTH_PREFIX_SIZE) {

        nalu_size = evc_read_nal_unit_length(bits, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (nalu_size == 0) break;

        bits += EVC_NALU_LENGTH_PREFIX_SIZE;
        bytes_to_read -= EVC_NALU_LENGTH_PREFIX_SIZE;

        if(bytes_to_read < nalu_size) break;

        nalu_type = evc_get_nalu_type(bits, bytes_to_read);

        if (nalu_type == EVC_SPS_NUT)
            got_sps++;
        else if (nalu_type == EVC_PPS_NUT)
            got_pps++;
        else if (nalu_type == EVC_IDR_NUT )
            got_idr++;
        else if (nalu_type == EVC_NOIDR_NUT)
            got_nonidr++;

        bits += nalu_size;
        bytes_to_read -= nalu_size;
    }

    if (got_sps && got_pps && (got_idr || got_nonidr > 3))
        return AVPROBE_SCORE_EXTENSION + 1;  // 1 more than .mpg

    return 0;
}

static int evc_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;
    const AVBitStreamFilter *filter = av_bsf_get_by_name("evc_frame_merge");
    EVCDemuxContext *c = s->priv_data;
    int ret = 0;

    if (!filter)
        return AVERROR_BUG;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    sti = ffstream(st);

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_EVC;

    // This causes sending to the parser full frames, not chunks of data
    // The flag PARSER_FLAG_COMPLETE_FRAMES will be set in demux.c (demux.c: 1316)
    sti->need_parsing = AVSTREAM_PARSE_HEADERS;

    st->avg_frame_rate = c->framerate;

    // taken from rawvideo demuxers
    avpriv_set_pts_info(st, 64, 1, 1200000);

    ret = av_bsf_alloc(filter, &c->bsf);
    if (ret < 0)
        return ret;

    ret = avcodec_parameters_copy(c->bsf->par_in, st->codecpar);
    if (ret < 0)
        return ret;

    ret = av_bsf_init(c->bsf);
    if (ret < 0)
        return ret;

fail:
    return ret;
}

static int evc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    uint32_t nalu_size;
    int au_end_found = 0;
    EVCDemuxContext *const c = s->priv_data;

    if (s->io_repositioned) {
        AVStream *st = s->streams[0];

        s->io_repositioned = 0;
        av_bsf_flush(c->bsf);
        // The stream carries no timing information, so access units are
        // stamped by counting them. After a seek, restart the count at the
        // seek point (cur_dts, set by the generic seek code) to keep packets
        // consistent with the index. A pending linear scan starts reading at
        // the beginning of the stream, so restart the count from zero then.
        if (avio_tell(s->pb) > ffformatcontext(s)->data_offset &&
            ffstream(st)->cur_dts != AV_NOPTS_VALUE)
            c->au_count = av_rescale_q(ffstream(st)->cur_dts, st->time_base, c->framerate);
        else
            c->au_count = 0;
        if (c->sps_size || c->pps_size) {
            // Re-send the cached parameter sets so that the parser and the
            // decoder can resume at the seek point.
            size_t size = c->sps_size + c->pps_size;
            if (size) {
                AVPacket *ps_pkt = av_packet_alloc();
                if (!ps_pkt)
                    return AVERROR(ENOMEM);
                ret = av_new_packet(ps_pkt, size);
                if (ret < 0) {
                    av_packet_free(&ps_pkt);
                    return ret;
                }
                // Carry the current position so the access unit eventually
                // assembled from this data is indexable/seekable.
                ps_pkt->pos = avio_tell(s->pb);
                if (c->sps_size)
                    memcpy(ps_pkt->data, c->sps_buf, c->sps_size);
                if (c->pps_size)
                    memcpy(ps_pkt->data + c->sps_size, c->pps_buf, c->pps_size);
                ret = av_bsf_send_packet(c->bsf, ps_pkt);
                av_packet_free(&ps_pkt);
                if (ret < 0) {
                    av_log(s, AV_LOG_ERROR, "Failed to re-send parameter sets to "
                           "evc_frame_merge filter\n");
                    return ret;
                }
                // Parameter sets alone do not complete an access unit; drain
                // the filter so the next send does not hit a pending packet.
                ret = av_bsf_receive_packet(c->bsf, pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                    return ret;
            }
        }
    }

    while(!au_end_found) {
        uint8_t buf[EVC_NALU_LENGTH_PREFIX_SIZE];

        if (avio_feof(s->pb))
            goto end;

        ret = ffio_ensure_seekback(s->pb, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (ret < 0)
            return ret;

        ret = avio_read(s->pb, buf, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (ret < 0)
            return ret;
        if (ret != EVC_NALU_LENGTH_PREFIX_SIZE)
            return AVERROR_INVALIDDATA;

        nalu_size = evc_read_nal_unit_length(buf, EVC_NALU_LENGTH_PREFIX_SIZE);
        if (!nalu_size || nalu_size > INT_MAX)
            return AVERROR_INVALIDDATA;

        avio_seek(s->pb, -EVC_NALU_LENGTH_PREFIX_SIZE, SEEK_CUR);

        ret = av_get_packet(s->pb, pkt, nalu_size + EVC_NALU_LENGTH_PREFIX_SIZE);
        if (ret < 0)
            return ret;
        if (ret != (nalu_size + EVC_NALU_LENGTH_PREFIX_SIZE))
            return AVERROR_INVALIDDATA;

        // Keep a copy of parameter set NAL units for post-seek re-injection
        {
            int nalu_type = evc_get_nalu_type(pkt->data + EVC_NALU_LENGTH_PREFIX_SIZE,
                                              nalu_size);
            uint8_t **buf = NULL;
            size_t *size = NULL;
            void *tmp;

            if (nalu_type == EVC_SPS_NUT) {
                buf = &c->sps_buf;
                size = &c->sps_size;
            } else if (nalu_type == EVC_PPS_NUT) {
                buf = &c->pps_buf;
                size = &c->pps_size;
            }
            if (buf) {
                tmp = av_realloc(*buf, nalu_size + EVC_NALU_LENGTH_PREFIX_SIZE);
                if (!tmp)
                    return AVERROR(ENOMEM);
                *buf = tmp;
                memcpy(*buf, pkt->data, nalu_size + EVC_NALU_LENGTH_PREFIX_SIZE);
                *size = nalu_size + EVC_NALU_LENGTH_PREFIX_SIZE;
            }
        }

end:
        ret = av_bsf_send_packet(c->bsf, pkt);
        if (ret < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to send packet to "
                   "evc_frame_merge filter\n");
            return ret;
        }

        ret = av_bsf_receive_packet(c->bsf, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            av_log(s, AV_LOG_ERROR, "evc_frame_merge filter failed to "
                   "send output packet\n");

        if (ret != AVERROR(EAGAIN))
            au_end_found = 1;
    }

    if (ret >= 0) {
        // raw input carries no timing; stamp access units in decode order at
        // the configured frame rate. Shift pts by the reorder delay of B
        // pictures so that pts != dts; a packet with pts == dts would make
        // the core discard the dts of reordered streams, breaking indexing
        // and seeking.
        AVStream *st = s->streams[0];
        AVRational dur = av_inv_q(c->framerate);

        pkt->dts = av_rescale_q(c->au_count, dur, st->time_base);
        pkt->pts = pkt->dts -
                   av_rescale_q(st->codecpar->video_delay, dur, st->time_base);
        pkt->duration = av_rescale_q(1, dur, st->time_base);
        c->au_count++;
    }

    return ret;
}

static int evc_read_close(AVFormatContext *s)
{
    EVCDemuxContext *const c = s->priv_data;

    av_bsf_free(&c->bsf);
    av_freep(&c->sps_buf);
    av_freep(&c->pps_buf);
    c->sps_size = c->pps_size = 0;
    return 0;
}

const FFInputFormat ff_evc_demuxer = {
    .p.name         = "evc",
    .p.long_name    = NULL_IF_CONFIG_SMALL("EVC Annex B"),
    .p.extensions   = "evc",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.priv_class   = &evc_demuxer_class,
    .read_probe     = annexb_probe,
    .read_header    = evc_read_header, // annexb_read_header
    .read_packet    = evc_read_packet, // annexb_read_packet
    .read_close     = evc_read_close,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .raw_codec_id   = AV_CODEC_ID_EVC,
    .priv_data_size = sizeof(EVCDemuxContext),
};
