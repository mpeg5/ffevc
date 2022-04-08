/*
 * libxeve encoder
 * EVC (MPEG-5 Essential Video Coding) encoding using XEVE MPEG-5 EVC encoder library
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
#define XEVE_API_IMPORTS 1
#endif

#include <float.h>
#include <stdlib.h>

#include <xeve.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "internal.h"
#include "packet_internal.h"

#define MAX_BS_BUF (16*1024*1024)

/**
 * Error codes
 */
#define XEVE_PARAM_BAD_NAME -100
#define XEVE_PARAM_BAD_VALUE -200

/**
 * Macro for eliminating the unused variable warning
 */
#define UNUSED(x) (void)(x)

/**
 * Encoder states
 *
 * STATE_ENCODING - the encoder receives and processes input frames
 * STATE_BUMPING  - there are no more input frames, however the encoder still processes previously received data
 * STATE_SKIPPING - skipping input frames
 */
typedef enum State {
    STATE_ENCODING,
    STATE_BUMPING,
    STATE_SKIPPING
} State;

/**
 * The structure stores all the state associated with the instance of Xeve MPEG-5 EVC encoder
 * The first field is a pointer to an AVClass struct (@see https://ffmpeg.org/doxygen/trunk/structAVClass.html#details).
 */
typedef struct XeveContext {
    const AVClass *class;

    XEVE id;            // XEVE instance identifier
    XEVE_CDSC cdsc;     // coding parameters i.e profile, width & height of input frame, num of therads, frame rate ...
    XEVE_BITB bitb;     // bitstream buffer (output)
    XEVE_STAT stat;     // encoding status (output)
    XEVE_IMGB imgb;     // image buffer (input)

    State state;        // encoder state (skipping, encoding, bumping)

    int encod_frames;   // num of encoded frames
    double bytes_total; // encoded bitstream byte size
    double bitrate;     // bits per second
    int packet_count;   // num of packets created by encoder

    // Chroma subsampling
    int width_luma;
    int height_luma;
    int width_chroma;
    int height_chroma;

    int profile_id;     // encoder profile (main, baseline)
    int preset_id;      // preset of xeve ( fast, medium, slow, placebo)
    int tune_id;        // tune of xeve (psnr, zerolatency)
    int input_depth;    // input bit-depth: 8bit, 10bit
    int hash;

    /* variables for input parameter */
    char *op_preset;
    char *op_tune;
    int op_qp;
    int op_crf;

    // configuration parameters
    // xeve configuration read from a :-separated list of key=value parameters
    AVDictionary *xeve_params;
} XeveContext;

/**
 * Gets Xeve encoder pre-defined profile
 *
 * @param profile string describing Xeve encoder profile (baseline, main)
 * @return XEVE pre-defined profile on success, negative value on failure
 */
static int get_profile_id(const char *profile)
{
    if (!strcmp(profile, "baseline"))
        return XEVE_PROFILE_BASELINE;
    else if (!strcmp(profile, "main"))
        return XEVE_PROFILE_MAIN;
    else
        return -1;
}

/**
 * Gets Xeve pre-defined preset
 *
 * @param preset string describing Xeve encoder preset (fast, medium, slow, placebo )
 * @return XEVE pre-defined profile on success, negative value on failure
 */
static int get_preset_id(const char *preset)
{
    if((!strcmp(preset, "fast")))
        return XEVE_PRESET_FAST;
    else if (!strcmp(preset, "medium"))
        return XEVE_PRESET_MEDIUM;
    else if (!strcmp(preset, "slow"))
        return XEVE_PRESET_SLOW;
    else if (!strcmp(preset, "placebo"))
        return XEVE_PRESET_PLACEBO;
    else
        return -1;
}

/**
 * Gets Xeve pre-defined tune id
 *
 * @param[in] tune string describing Xeve encoder tune (psnr, zerolatency )
 * @return XEVE pre-defined profile on success, negative value on failure
 */
static int get_tune_id(const char *tune)
{
    if((!strcmp(tune, "psnr")))
        return XEVE_TUNE_PSNR;
    else if (!strcmp(tune, "zerolatency"))
        return XEVE_TUNE_ZEROLATENCY;
    else
        return -1;
}

/**
 * Convert ffmpeg pixel format (AVPixelFormat) to XEVE pre-defined color format
 *
 * @param[in]  px_fmt pixel format (@see https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5)
 * @param[out] color_format XEVE pre-defined color format (@see xeve.h)
 * @param[out] bit_depth bit depth
 *
 * @return 0 on success, negative value on failure
 */
static int get_pix_fmt(enum AVPixelFormat pix_fmt, int *color_format, int *bit_depth)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        *color_format = XEVE_CF_YCBCR420;
        *bit_depth = 8;
        break;
    case AV_PIX_FMT_YUV422P:
        *color_format = XEVE_CF_YCBCR422;
        *bit_depth = 8;
        break;
    case AV_PIX_FMT_YUV444P:
        *color_format = XEVE_CF_YCBCR444;
        *bit_depth = 8;
        break;
    case AV_PIX_FMT_YUV420P10:
        *color_format = XEVE_CF_YCBCR420;
        *bit_depth = 10;
        break;
    case AV_PIX_FMT_YUV422P10:
        *color_format = XEVE_CF_YCBCR422;
        *bit_depth = 10;
        break;
    case AV_PIX_FMT_YUV444P10:
        *color_format = XEVE_CF_YCBCR444;
        *bit_depth = 10;
        break;
    default:
        *color_format = XEVE_CF_UNKNOWN;
        return -1;
    }

    return 0;
}

static int kbps_str_to_int(char *str)
{
    int kbps = 0;
    if (strchr(str, 'K') || strchr(str, 'k')) {
        char *tmp = strtok(str, "Kk ");
        kbps = (int)(atof(tmp));
    } else if (strchr(str, 'M') || strchr(str, 'm')) {
        char *tmp = strtok(str, "Mm ");
        kbps = (int)(atof(tmp) * 1000);
    } else
        kbps = atoi(str);

    return kbps;
}

/**
 * Parse :-separated list of key=value parameters
 *
 * @param[in] avctx context for logger
 * @param[in] key
 * @param[in] value
 * @param[out] cdsc contains all Xeve MPEG-5 EVC encoder encoder parameters that 
 *                  should be initialized before the encoder is use
 *
 * @return 0 on success, negative value on failure
 */
static int parse_xeve_params(AVCodecContext *avctx, const char *key, const char *value, XEVE_CDSC* cdsc)
{
    XeveContext *xectx = NULL;
    xectx = avctx->priv_data;

    if(!key) {
        av_log(avctx, AV_LOG_ERROR, "Ivalid argument: key string is NULL\n");
        return XEVE_PARAM_BAD_VALUE;
    }
    if(!value) {
        if (strcmp(key, "hash") == 0) {
            xectx->hash = 1;
            av_log(avctx, AV_LOG_INFO, "embedding signature is enabled\n");
        } else {
            av_log(avctx, AV_LOG_ERROR, "Ivalid argument: value string is NULL\n");
            return XEVE_PARAM_BAD_VALUE;
        }
    } else if (strcmp(key, "vbv-bufsize") == 0 ) {
        cdsc->param.vbv_bufsize = kbps_str_to_int((char *)value);
        av_log(avctx, AV_LOG_INFO, "VBV buffer size: %dkbits\n", cdsc->param.vbv_bufsize);
    } else if (strcmp(key, "rc-type") == 0 ) {
        int rc_type = atoi(value);
        if(rc_type < 0 || rc_type > 2) {
            av_log(avctx, AV_LOG_ERROR, "Rate control type [ 0(rc_off) / 1(CBR) ] bad value: %d\n", rc_type);
            return XEVE_PARAM_BAD_VALUE;
        }
        cdsc->param.rc_type = rc_type;
        av_log(avctx, AV_LOG_INFO, "Rate control type [ 0(rc_off) / 1(CBR) ] : %d\n", rc_type);
    } else if (strcmp(key, "bframes") == 0 ) {
        int bframes = atoi(value);
        if(bframes < 0) {
            av_log(avctx, AV_LOG_ERROR, "bframes: bad value: %d\n", bframes);
            return XEVE_PARAM_BAD_VALUE;
        }
        cdsc->param.bframes = bframes;
        av_log(avctx, AV_LOG_INFO, "bframes : %d\n", bframes);
    } else if (strcmp(key, "profile") == 0 ) {
        const char *profile = value;
        int profile_id;
        av_log(avctx, AV_LOG_INFO, "profile (baseline, main): %s\n", profile);
        profile_id = get_profile_id(profile);
        if (profile_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid xeve param: profile(%s)\n", profile);
            return XEVE_PARAM_BAD_VALUE;
        }
        xectx->profile_id = profile_id;
    } else if (strcmp(key, "preset") == 0 ) {
        const char *preset = value;
        int preset_id;
        av_log(avctx, AV_LOG_INFO, "Preset of xeve (fast, medium, slow, placebo): %s\n", preset);
        preset_id = get_preset_id(preset);
        if( preset_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid xeve param: preset(%s)\n", preset);
            return XEVE_PARAM_BAD_VALUE;
        }
        xectx->preset_id = preset_id;
    } else if (strcmp(key, "tune") == 0 ) {
        const char *tune = value;
        int tune_id;
        av_log(avctx, AV_LOG_INFO, "Tune of xeve (psnr, zerolatency): %s\n", tune);
        tune_id = get_tune_id(tune);
        if( tune_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid xeve param: tune(%s)\n", tune);
            return XEVE_PARAM_BAD_VALUE;
        }
        xectx->tune_id = tune_id;
    } else if (strcmp(key, "bitrate") == 0 ) {
        cdsc->param.bitrate = kbps_str_to_int((char *)value);
        av_log(avctx, AV_LOG_INFO, "Bitrate = %dkbps\n", cdsc->param.bitrate);
    } else if (strcmp(key, "q") == 0 || strcmp(key, "qp") == 0) {
        int qp = atoi(value);
        if(qp < 0 || qp > 51) {
            av_log(avctx, AV_LOG_ERROR, "Invalid QP value (0~51) :%d\n", qp);
            return XEVE_PARAM_BAD_VALUE;
        }
        cdsc->param.qp = qp;
        av_log(avctx, AV_LOG_INFO, "QP value (0~51): %d\n", cdsc->param.qp);
    } else {
        av_log(avctx, AV_LOG_ERROR, "Unknown xeve codec option: %s\n", key);
        return XEVE_PARAM_BAD_NAME;
    }

    return 0;
}

/**
 * The function returns a pointer to the object of XEVE_CDSC type.
 * XEVE_CDSC contains all encoder parameters that should be initialized before the encoder use.
 *
 * The field values of the XEVE_CDSC structure are populated based on:
 * - the corresponding field values of the AvCodecConetxt structure,
 * - the xeve encoder specific option values,
 *   (the full list of options available for xeve encoder is displayed after executing the command ./ffmpeg --help encoder = libxeve)
 * - and the xeve encoder options specified as a list of key value pairs following xeve-params option
 *
 * The order of processing input data and populating the XEVE_CDSC structure
 * 1) first, the fields of the AVCodecContext structure corresponding to the provided input options are processed,
 *    (i.e -pix_fmt yuv420p -s:v 1920x1080 -r 30 -profile:v 0)
 * 2) then xeve-specific options added as AVOption to the xeve AVCodec implementation
 *    (i.e -threads_cnt 3 -preset 0)
 * 3) finally, the options specified after the xeve-params option as the parameter list of type key value are processed
 *    (i.e -xeve-params "m=2:q=17")
 *
 * Keep in mind that, there are options that can be set in different ways.
 * In this case, please follow the above-mentioned order of processing.
 * The most recent assignments overwrite the previous values.
 *
 * @param[in] avctx codec context (AVCodecContext)
 * @param[out] cdsc contains all Xeve MPEG-5 EVC encoder encoder parameters that should be initialized before the encoder is use
 *
 * @return 0 on success, negative error code on failure
 */
static int get_conf(AVCodecContext *avctx, XEVE_CDSC *cdsc)
{
    XeveContext *xectx = NULL;
    int color_format;
    int cpu_count = av_cpu_count();
    int ret;
    
    xectx = avctx->priv_data;
    xectx->hash = 0;

    /* initialize xeve_param struct with default values */
    ret = xeve_param_default(&cdsc->param);
    if (XEVE_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "cannot set_default parameter\n");
        goto ERR;
    }

    /* read options from AVCodecContext */
    if(avctx->width > 0)
        cdsc->param.w = xectx->width_luma = avctx->width;

    if(avctx->height > 0)
        cdsc->param.h = xectx->height_luma = avctx->height;

    if(avctx->framerate.num > 0) {
        /* @todo: fps can be float number, but xeve API doesn't support it */
        cdsc->param.fps = (int)(((float)avctx->framerate.num / avctx->framerate.den) + 0.5);
    }

    if(avctx->gop_size >= 0) { // GOP size (key-frame interval)
        cdsc->param.keyint = avctx->gop_size; // 0: only one I-frame at the first time; 1: every frame is coded in I-frame
    }
    
    if (avctx->max_b_frames == 0 || avctx->max_b_frames == 1 || avctx->max_b_frames == 3 ||
        avctx->max_b_frames == 7 || avctx->max_b_frames == 15) { // number of b-frames
        cdsc->param.bframes = avctx->max_b_frames;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Incorrect value for maximum number of B frames: (%d) \n"
               "Acceptable values for bf option (maximum number of B frames) are 0,1,3,7 or 15\n", avctx->max_b_frames);
        goto ERR;
    }

    if (avctx->level >= 0)
        cdsc->param.level_idc = avctx->level;
    
    ret = get_pix_fmt(avctx->pix_fmt, &color_format, &xectx->input_depth);
    
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format.\n");
        goto ERR;
    }
    cdsc->param.cs = XEVE_CS_SET(color_format, xectx->input_depth, 0);

    if (avctx->rc_buffer_size > 0) { // VBV buf size
        cdsc->param.vbv_bufsize = (int)(avctx->rc_buffer_size / 1000);
    }

    // rc_type:  Rate control type [ 0(CQP) / 1(ABR) / 2(CRF) ]
    if (avctx->bit_rate > 0) {
        if (avctx->bit_rate / 1000 > INT_MAX || avctx->rc_max_rate / 1000 > INT_MAX) {
            av_log(avctx, AV_LOG_ERROR, "not supported bitrate bit_rate and rc_max_rate > %d000\n", INT_MAX);
            goto ERR;
        }
        cdsc->param.bitrate = (int)(avctx->bit_rate / 1000);
        cdsc->param.rc_type = XEVE_RC_ABR;
    }
    
    if (xectx->op_crf >= 0) {
        cdsc->param.crf = xectx->op_crf;
        cdsc->param.rc_type = XEVE_RC_CRF;
    }

    if(avctx->thread_count <= 0) {
        cdsc->param.threads = (cpu_count < XEVE_MAX_THREADS) ? cpu_count : XEVE_MAX_THREADS;
    } else if(avctx->thread_count > XEVE_MAX_THREADS)
        cdsc->param.threads = XEVE_MAX_THREADS;
    else
        cdsc->param.threads = avctx->thread_count;

    cdsc->param.cs = XEVE_CS_SET(color_format, cdsc->param.codec_bit_depth, 0);
    cdsc->max_bs_buf_size = MAX_BS_BUF;

    if(avctx->profile == FF_PROFILE_EVC_BASELINE)
        xectx->profile_id = XEVE_PROFILE_BASELINE;
    else if(avctx->profile == FF_PROFILE_EVC_MAIN)
        xectx->profile_id = XEVE_PROFILE_MAIN;
    else {
        av_log(avctx, AV_LOG_ERROR, "Unknown encoder profile (%d)\n"
               "Acceptable values for profile option are 0 and 1 (0: baseline profile; 1: main profile)\n", avctx->profile);
        goto ERR;
    }
    
    if (xectx->op_preset) { // preset
        xectx->preset_id = get_preset_id(xectx->op_preset);
    }

    if (xectx->op_tune) { // tune
        xectx->tune_id = get_tune_id(xectx->op_tune);
    }

    ret = xeve_param_ppt(&cdsc->param, xectx->profile_id, xectx->preset_id, xectx->tune_id);
    if (XEVE_FAILED(ret)) {
        av_log(avctx, AV_LOG_ERROR, "cannot set profile(%d), preset(%d), tune(%d)\n", xectx->profile_id, xectx->preset_id, xectx->tune_id);
        goto ERR;
    }

    /* parse :-separated list of key=value parameters and set values for created descriptor (XEVE_CDSC) */
    {
        AVDictionaryEntry *en = NULL;
        
        // Start to parse xeve_params
        while ((en = av_dict_get(xectx->xeve_params, "", en, AV_DICT_IGNORE_SUFFIX))) {
            int parse_ret = parse_xeve_params(avctx, en->key, en->value, cdsc);

            switch (parse_ret) {
            case XEVE_PARAM_BAD_NAME:
                av_log(avctx, AV_LOG_WARNING, "Unknown option: %s.\n", en->key);
                break;
            case XEVE_PARAM_BAD_VALUE:
                av_log(avctx, AV_LOG_WARNING, "Invalid value for %s: %s.\n", en->key, en->value);
                break;
            default:
                break;
            }
        }
    }

    return 0;

ERR:
    return AVERROR(EINVAL);
}

/**
 * Check codec configuration
 *
 * @param[in] avctx context for logger
 * @param[in] cdsc contains all encoder parameters that should be initialized before its use
 *
 * @return 0 on success, negative error code on failure
 */
static int check_conf(AVCodecContext *avctx,  XEVE_CDSC *cdsc)
{
    int ret = 0;
    int min_block_size = 4;
    int pic_m;

    if(cdsc->param.profile == XEVE_PROFILE_BASELINE) {
        if (cdsc->param.tool_amvr    == 1) {
            av_log(avctx, AV_LOG_ERROR, "AMVR cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_mmvd    == 1) {
            av_log(avctx, AV_LOG_ERROR, "MMVD cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_affine  == 1) {
            av_log(avctx, AV_LOG_ERROR, "Affine cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_dmvr    == 1) {
            av_log(avctx, AV_LOG_ERROR, "DMVR cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_admvp   == 1) {
            av_log(avctx, AV_LOG_ERROR, "ADMVP cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_hmvp    == 1) {
            av_log(avctx, AV_LOG_ERROR, "HMVP cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_addb    == 1) {
            av_log(avctx, AV_LOG_ERROR, "ADDB cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_alf     == 1) {
            av_log(avctx, AV_LOG_ERROR, "ALF cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_htdf    == 1) {
            av_log(avctx, AV_LOG_ERROR, "HTDF cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.btt          == 1) {
            av_log(avctx, AV_LOG_ERROR, "BTT cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.suco         == 1) {
            av_log(avctx, AV_LOG_ERROR, "SUCO cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_eipd    == 1) {
            av_log(avctx, AV_LOG_ERROR, "EIPD cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_iqt     == 1) {
            av_log(avctx, AV_LOG_ERROR, "IQT cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_cm_init == 1) {
            av_log(avctx, AV_LOG_ERROR, "CM_INIT cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_adcc    == 1) {
            av_log(avctx, AV_LOG_ERROR, "ADCC cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_ats     == 1) {
            av_log(avctx, AV_LOG_ERROR, "ATS_INTRA cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.ibc_flag     == 1) {
            av_log(avctx, AV_LOG_ERROR, "IBC cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_rpl     == 1) {
            av_log(avctx, AV_LOG_ERROR, "RPL cannot be on in base profile\n");
            ret = -1;
        }
        if (cdsc->param.tool_pocs    == 1) {
            av_log(avctx, AV_LOG_ERROR, "POCS cannot be on in base profile\n");
            ret = -1;
        }
    } else {
        if (cdsc->param.tool_admvp   == 0 && cdsc->param.tool_affine == 1) {
            av_log(avctx, AV_LOG_ERROR, "AFFINE cannot be on when ADMVP is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_admvp   == 0 && cdsc->param.tool_amvr   == 1) {
            av_log(avctx, AV_LOG_ERROR, "AMVR cannot be on when ADMVP is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_admvp   == 0 && cdsc->param.tool_dmvr   == 1) {
            av_log(avctx, AV_LOG_ERROR, "DMVR cannot be on when ADMVP is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_admvp   == 0 && cdsc->param.tool_mmvd   == 1) {
            av_log(avctx, AV_LOG_ERROR, "MMVD cannot be on when ADMVP is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_eipd    == 0 && cdsc->param.ibc_flag    == 1) {
            av_log(avctx, AV_LOG_ERROR, "IBC cannot be on when EIPD is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_iqt     == 0 && cdsc->param.tool_ats    == 1) {
            av_log(avctx, AV_LOG_ERROR, "ATS cannot be on when IQT is off\n");
            ret = -1;
        }
        if (cdsc->param.tool_cm_init == 0 && cdsc->param.tool_adcc   == 1) {
            av_log(avctx, AV_LOG_ERROR, "ADCC cannot be on when CM_INIT is off\n");
            ret = -1;
        }
    }

    if (cdsc->param.btt == 1) {
        if (cdsc->param.framework_cb_max && cdsc->param.framework_cb_max < 5) {
            av_log(avctx, AV_LOG_ERROR, "Maximun Coding Block size cannot be smaller than 5\n");
            ret = -1;
        }
        if (cdsc->param.framework_cb_max > 7) {
            av_log(avctx, AV_LOG_ERROR, "Maximun Coding Block size cannot be greater than 7\n");
            ret = -1;
        }
        if (cdsc->param.framework_cb_min && cdsc->param.framework_cb_min < 2) {
            av_log(avctx, AV_LOG_ERROR, "Minimum Coding Block size cannot be smaller than 2\n");
            ret = -1;
        }
        if ((cdsc->param.framework_cb_max || cdsc->param.framework_cb_min) &&
            cdsc->param.framework_cb_min > cdsc->param.framework_cb_max) {
            av_log(avctx, AV_LOG_ERROR, "Minimum Coding Block size cannot be greater than Maximum coding Block size\n");
            ret = -1;
        }
        if (cdsc->param.framework_cu14_max > 6) {
            av_log(avctx, AV_LOG_ERROR, "Maximun 1:4 Coding Block size cannot be greater than 6\n");
            ret = -1;
        }
        if ((cdsc->param.framework_cb_max || cdsc->param.framework_cu14_max) &&
            cdsc->param.framework_cu14_max > cdsc->param.framework_cb_max) {
            av_log(avctx, AV_LOG_ERROR, "Maximun 1:4 Coding Block size cannot be greater than Maximum coding Block size\n");
            ret = -1;
        }
        if (cdsc->param.framework_tris_max > 6) {
            av_log(avctx, AV_LOG_ERROR, "Maximun Tri-split Block size be greater than 6\n");
            ret = -1;
        }
        if ((cdsc->param.framework_tris_max || cdsc->param.framework_cb_max) &&
            cdsc->param.framework_tris_max > cdsc->param.framework_cb_max) {
            av_log(avctx, AV_LOG_ERROR, "Maximun Tri-split Block size cannot be greater than Maximum coding Block size\n");
            ret = -1;
        }
        if ((cdsc->param.framework_tris_min || cdsc->param.framework_cb_min) &&
            cdsc->param.framework_tris_min < cdsc->param.framework_cb_min + 2) {
            av_log(avctx, AV_LOG_ERROR, "Maximun Tri-split Block size cannot be smaller than Minimum Coding Block size plus two\n");
            ret = -1;
        }
        if(cdsc->param.framework_cb_min) min_block_size = 1 << cdsc->param.framework_cb_min;
        else min_block_size = 8;
    }

    if (cdsc->param.suco == 1) {
        if (cdsc->param.framework_suco_max > 6) {
            av_log(avctx, AV_LOG_ERROR, "Maximun SUCO size cannot be greater than 6\n");
            ret = -1;
        }
        if (cdsc->param.framework_cb_max && cdsc->param.framework_suco_max > cdsc->param.framework_cb_max) {
            av_log(avctx, AV_LOG_ERROR, "Maximun SUCO size cannot be greater than Maximum coding Block size\n");
            ret = -1;
        }
        if (cdsc->param.framework_suco_min < 4) {
            av_log(avctx, AV_LOG_ERROR, "Minimun SUCO size cannot be smaller than 4\n");
            ret = -1;
        }
        if (cdsc->param.framework_cb_min && cdsc->param.framework_suco_min < cdsc->param.framework_cb_min) {
            av_log(avctx, AV_LOG_ERROR, "Minimun SUCO size cannot be smaller than Minimum coding Block size\n");
            ret = -1;
        }
        if (cdsc->param.framework_suco_min > cdsc->param.framework_suco_max) {
            av_log(avctx, AV_LOG_ERROR, "Minimum SUCO size cannot be greater than Maximum SUCO size\n");
            ret = -1;
        }
    }

    pic_m = (8 > min_block_size) ? min_block_size : 8;
    if ((cdsc->param.w & (pic_m - 1)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Current encoder does not support picture width, not multiple of max(8, minimum CU size)\n");
        ret = -1;
    }
    if ((cdsc->param.h & (pic_m - 1)) != 0) {
        av_log(avctx, AV_LOG_ERROR, "Current encoder does not support picture height, not multiple of max(8, minimum CU size)\n");
        ret = -1;
    }

    return ret;
}

/**
 * Set XEVE_CFG_SET_USE_PIC_SIGNATURE for encoder
 *
 * @param[in] logger context
 * @param[in] id XEVE encodec instance identifier
 * @param[in] ctx the structure stores all the state associated with the instance of Xeve MPEG-5 EVC encoder
 * 
 * @return 0 on success, negative error code on failure
 */
static int set_extra_config(AVCodecContext* avctx, XEVE id, XeveContext *ctx)
{
    int ret, size, value;

    if(ctx->hash) {
        value = 1;
        size = 4;
        ret = xeve_config(id, XEVE_CFG_SET_USE_PIC_SIGNATURE, &value, &size);
        if(XEVE_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "failed to set config for picture signature\n");
            return -1;
        }
    }

    return 0;
}

/**
 * Convert ffmpeg pixel format (AVPixelFormat) into XEVE pre-defined color space
 *
 * @param[in] px_fmt pixel format (@see https://ffmpeg.org/doxygen/trunk/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5)
 * 
 * @return XEVE pre-defined color space (@see xeve.h) on success, XEVE_CF_UNKNOWN on failure
 */
static int xeve_color_space(enum AVPixelFormat pix_fmt)
{
    /* color space of input image */
    int cs = XEVE_CF_UNKNOWN;

    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        cs = XEVE_CS_YCBCR420;
        break;
    case AV_PIX_FMT_YUV420P10:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR420, 10, 1);
#else
        cs = XEVE_CS_YCBCR420_10LE;
#endif

        break;
    case AV_PIX_FMT_YUV420P12:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR420, 12, 1);
#else
        cs = XEVE_CS_YCBCR420_12LE;
#endif

        break;
    case AV_PIX_FMT_YUV422P:
        cs = XEVE_CS_YCBCR422;
        break;
    case AV_PIX_FMT_YUV422P10:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR422, 10, 1);
#else
        cs = XEVE_CS_YCBCR422_10LE;
#endif

        break;
    case AV_PIX_FMT_YUV422P12:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR422, 12, 1);
#else
        cs = XEVE_CS_SET(XEVE_CF_YCBCR422, 12, 0);
#endif

        break;
    case AV_PIX_FMT_GBRP:
    case AV_PIX_FMT_GBRP10:
    case AV_PIX_FMT_GBRP12:
        cs = XEVE_CF_UNKNOWN;
        break;
    case AV_PIX_FMT_YUV444P:
        cs = XEVE_CF_YCBCR444;
        break;
    case AV_PIX_FMT_YUV444P10:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR444, 10, 1);
#else
        cs = XEVE_CS_YCBCR444_10LE;
#endif

        break;
    case AV_PIX_FMT_YUV444P12:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR444, 12, 1);
#else
        cs = XEVE_CS_SET(XEVE_CF_YCBCR444, 12, 0);
#endif

        break;
    case AV_PIX_FMT_GRAY8:
        cs = XEVE_CF_YCBCR400;
        break;
    case AV_PIX_FMT_GRAY10:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR400, 10, 1);
#else
        cs = XEVE_CS_YCBCR400_10LE;
#endif

        break;
    case AV_PIX_FMT_GRAY12:
#if AV_HAVE_BIGENDIAN
        cs = XEVE_CS_SET(XEVE_CF_YCBCR400, 12, 1);
#else
        cs = XEVE_CS_YCBCR400_12LE;
#endif

        break;
    default:
        cs = XEVE_CF_UNKNOWN;
        break;
    }

    return cs;
}

/**
 * @brief Switch encoder to bumping mode
 * 
 * @param id XEVE encodec instance identifier
 * @return 0 on success, negative error code on failure
 */
static int setup_bumping(XEVE id)
{
    int val, size;
    val  = 1;
    size = sizeof(int);
    if(XEVE_FAILED(xeve_config(id, XEVE_CFG_SET_FORCE_OUT, (void *)(&val), &size)))
        return -1;

    return 0;
}

/**
 * @brief Initialize eXtra-fast Essential Video Encoder codec
 * Create encoder instance and allocate all the needed resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libxeve_init(AVCodecContext *avctx)
{
    XeveContext *xectx = avctx->priv_data;
    unsigned char *bs_buf = NULL;
    int i, val = 0;
    int shift_h = 0;
    int shift_v = 0;
    XEVE_IMGB *imgb = NULL;

    XEVE_CDSC *cdsc = &(xectx->cdsc);

    if(avctx->pix_fmt != AV_PIX_FMT_YUV420P && avctx->pix_fmt != AV_PIX_FMT_YUV420P10) {
        av_log(avctx, AV_LOG_ERROR, "Invalid pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
        goto ERR;
    }

    /* allocate bitstream buffer */
    bs_buf = (unsigned char *)malloc(MAX_BS_BUF);
    if(bs_buf == NULL) {
        av_log(avctx, AV_LOG_ERROR, "cannot allocate bitstream buffer, size=%d", MAX_BS_BUF);
        goto ERR;
    }

    /* read configurations and set values for created descriptor (XEVE_CDSC) */
    val = get_conf(avctx, cdsc);
    if (val != XEVE_OK) {
        av_log(avctx, AV_LOG_ERROR, "cannot get configuration\n");
        goto ERR;
    }

    if (check_conf(avctx, cdsc) != 0) {
        av_log(avctx, AV_LOG_ERROR, "invalid configuration\n");
        goto ERR;
    }

    /* create encoder */
    xectx->id = xeve_create(cdsc, NULL);
    if(xectx->id == NULL) {
        av_log(avctx, AV_LOG_ERROR, "cannot create XEVE encoder\n");
        goto ERR;
    }

    if(set_extra_config(avctx, xectx->id, xectx)) {
        av_log(avctx, AV_LOG_ERROR, "cannot set extra configurations\n");
        goto ERR;
    }

    xectx->bitb.addr = bs_buf;
    xectx->bitb.bsize = MAX_BS_BUF;

    if(av_pix_fmt_get_chroma_sub_sample(avctx->pix_fmt, &shift_h, &shift_v)) {
        av_log(avctx, AV_LOG_ERROR, "failed to get  chroma shift\n");
        goto ERR;
    }

    // YUV format explanation
    // shift_h == 1 && shift_v == 1 : YUV420
    // shift_h == 1 && shift_v == 0 : YUV422
    // shift_h == 0 && shift_v == 0 : YUV444
    //
    xectx->width_chroma = AV_CEIL_RSHIFT(xectx->width_luma, shift_h);
    xectx->height_chroma = AV_CEIL_RSHIFT(xectx->height_luma, shift_v);

    /* set default values for input image buffer */
    imgb = &xectx->imgb;
    imgb->cs = xeve_color_space(avctx->pix_fmt);
    imgb->np = 3; /* only for yuv420p, yuv420ple */

    for (i = 0; i < imgb->np; i++)
        imgb->x[i] = imgb->y[i] = 0;

    imgb->w[0] = imgb->aw[0] = xectx->width_luma;
    imgb->w[1] = imgb->w[2] = imgb->aw[1] = imgb->aw[2] = xectx->width_chroma;
    imgb->h[0] = imgb->ah[0] = xectx->height_luma;
    imgb->h[1] = imgb->h[2] = imgb->ah[1] = imgb->ah[2] = xectx->height_chroma;

    xectx->encod_frames = 0;
    xectx->bytes_total = 0;
    xectx->state = STATE_ENCODING;
    xectx->packet_count = 0;
    xectx->bitrate = 0;

    return 0;

ERR:
    if(bs_buf)
        free(bs_buf);

    return -1;
}

/**
  * Encode data to an AVPacket.
  *
  * @param[in] avctx codec context
  * @param[out] pkt output AVPacket containing encoded data
  * @param[in] frame AVFrame containing the raw data to be encoded
  * @param[out] got_packet encoder sets to 0 or 1 to indicate that a
  *                         non-empty packet was returned in pkt
  * 
  * @return 0 on success, negative error code on failure
  */
static int libxeve_encode(AVCodecContext *avctx, AVPacket *pkt,
                          const AVFrame *frame, int *got_packet)
{
    XeveContext *xectx = NULL;
    int  ret = -1;
    int xeve_cs;
    if(avctx == NULL || pkt == NULL || got_packet == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid arguments\n");
        return -1;
    }
    xectx = avctx->priv_data;
    if(xectx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Invalid XEVE context\n");
        return -1;
    }
    if(xectx->state == STATE_SKIPPING && frame ) {
        xectx->state = STATE_ENCODING; // Entering encoding process
    } else if(xectx->state == STATE_ENCODING && frame == NULL) {
        if (setup_bumping(xectx->id) == 0)
            xectx->state = STATE_BUMPING;  // Entering bumping process
        else {
            av_log(avctx, AV_LOG_ERROR, "Failed to setup bumping\n");
            xectx->state = STATE_SKIPPING;
        }
    }

    if(xectx->state == STATE_ENCODING) {
        const AVPixFmtDescriptor *pixel_fmt_desc = av_pix_fmt_desc_get (frame->format);
        if(!pixel_fmt_desc) {
            av_log(avctx, AV_LOG_ERROR, "Invalid pixel format descriptor for pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
            return -1;
        }

        xeve_cs = xeve_color_space(avctx->pix_fmt);
        if(xeve_cs != XEVE_CS_YCBCR420 && xeve_cs != XEVE_CS_YCBCR420_10LE) {
            av_log(avctx, AV_LOG_ERROR, "Invalid pixel format: %s\n", av_get_pix_fmt_name(avctx->pix_fmt));
            return -1;
        }

        {
            int i;
            XEVE_IMGB *imgb = NULL;
            
            imgb = &xectx->imgb;

            for (i = 0; i < imgb->np; i++) {
                imgb->a[i] = frame->data[i];
                imgb->s[i] = frame->linesize[i];
            }

            if(xectx->id == NULL) {
                av_log(avctx, AV_LOG_ERROR, "Invalid XEVE encoder\n");
                return -1;
            }

            imgb->ts[0] = frame->pts;
            imgb->ts[1] = 0;
            imgb->ts[2] = 0;
            imgb->ts[3] = 0;

            /* push image to encoder */
            ret = xeve_push(xectx->id, imgb);
            if(XEVE_FAILED(ret)) {
                av_log(avctx, AV_LOG_ERROR, "xeve_push() failed\n");
                return -1;
            }
        }
    }
    if(xectx->state == STATE_ENCODING || xectx->state == STATE_BUMPING) {

        /* encoding */
        ret = xeve_encode(xectx->id, &(xectx->bitb), &(xectx->stat));
        if(XEVE_FAILED(ret)) {
            av_log(avctx, AV_LOG_ERROR, "xeve_encode() failed\n");
            return -1;
        }

        xectx->encod_frames++;

        /* store bitstream */
        if (ret == XEVE_OK_OUT_NOT_AVAILABLE) { // Return OK but picture is not available yet
            *got_packet = 0;
            return 0;
        } else if(ret == XEVE_OK) {
            int av_pic_type;

            if(xectx->stat.write > 0) {
                xectx->bytes_total += xectx->stat.write;
            
                ret = av_grow_packet(pkt, xectx->stat.write);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Can't allocate memory for AVPacket data\n");
                    return ret;
                }

                memcpy(pkt->data, xectx->bitb.addr, xectx->stat.write);

                pkt->pts = xectx->bitb.ts[0];
                pkt->dts = xectx->bitb.ts[1];

                xectx->bitrate += (xectx->stat.write - xectx->stat.sei_size);

                switch(xectx->stat.stype) {
                case XEVE_ST_I:
                    av_pic_type = AV_PICTURE_TYPE_I;
                    pkt->flags |= AV_PKT_FLAG_KEY;
                    break;
                case XEVE_ST_P:
                    av_pic_type = AV_PICTURE_TYPE_P;
                    break;
                case XEVE_ST_B:
                    av_pic_type = AV_PICTURE_TYPE_B;
                    break;
                case XEVE_ST_UNKNOWN:
                    av_log(avctx, AV_LOG_ERROR, "unknown slice type\n");
                    return -1;
                }

                ff_side_data_set_encoder_stats(pkt, xectx->stat.qp * FF_QP2LAMBDA, NULL, 0, av_pic_type);

                xectx->bitrate += (xectx->stat.write - xectx->stat.sei_size);

                *got_packet = 1;
                xectx->packet_count++;
            }
        } else if (ret == XEVE_OK_NO_MORE_FRM) {
            av_log(avctx, AV_LOG_INFO, "Return OK but no more frames (%d)\n", ret);
            return 0;
        } else {
            av_log(avctx, AV_LOG_ERROR, "Invalid return value (%d)\n", ret);
            return -1;
        }
    } else {
        av_log(avctx, AV_LOG_ERROR, "Udefined state: %d\n", xectx->state);
        return -1;
    }

    return 0;
}

/**
 * Destroy encoder and release all the allocated resources
 *
 * @param avctx codec context
 * @return 0 on success, negative error code on failure
 */
static av_cold int libxeve_close(AVCodecContext *avctx)
{
    XeveContext *xectx = avctx->priv_data;

    xeve_delete(xectx->id);

    if(xectx->bitb.addr) free(xectx->bitb.addr); /* release bitstream buffer */

    return 0;
}

#define OFFSET(x) offsetof(XeveContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

// Example of using: ./ffmpeg -xeve-params "m=2:q=17"
// Consider using following options (./ffmpeg --help encoder=libxeve)
//
static const AVOption xeve_options[] = {
    { "preset", "Encoding preset for setting encoding speed [fast, medium, slow, placebo]", OFFSET(op_preset), AV_OPT_TYPE_STRING, { .str = "medium" }, 0, 0, VE },
    { "tune", "Tuneing parameter for special purpose operation [psnr, zerolatency]", OFFSET(op_tune), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "qp", "quantization parameter qp <0..51> [default: 32]", OFFSET(op_qp), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },
    { "crf", "constant rate factor <-1..51> [default: 32]", OFFSET(op_crf), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 51, VE },
    { "xeve-params", "override the xeve configuration using a :-separated list of key=value parameters", OFFSET(xeve_params), AV_OPT_TYPE_DICT,   { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass xeve_class = {
    .class_name = "libxeve",
    .item_name  = av_default_item_name,
    .option     = xeve_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/**
 *  libavcodec generic global options, which can be set on all the encoders and decoders
 *  @see https://www.ffmpeg.org/ffmpeg-codecs.html#Codec-Options
 */
static const AVCodecDefault xeve_defaults[] = {
    { "b", "0" },       // bitrate
    { "g", "0" },       // gop_size (key-frame interval 0: only one I-frame at the first time; 1: every frame is coded in I-frame)
    { "bf", "15"},      // bframes (0: no B-frames)
    { "profile", "0"},  // encoder codec profile (0: baselie; 1: main)
    { "threads", "0"},  // number of threads to be used (0: automatically select the number of threads to set)
    { NULL },
};

AVCodec ff_libxeve_encoder = {
    .name             = "libxeve",
    .long_name        = NULL_IF_CONFIG_SMALL("libxeve MPEG-5 EVC"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_EVC,
    .init             = libxeve_init,
    .encode2          = libxeve_encode,
    .close            = libxeve_close,
    .priv_data_size   = sizeof(XeveContext),
    .priv_class       = &xeve_class,
    .defaults         = xeve_defaults,
    .capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS |
    AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .wrapper_name     = "libxeve",
};
