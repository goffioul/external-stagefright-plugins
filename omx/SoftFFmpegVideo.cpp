/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
 * Copyright 2015 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "SoftFFmpegVideo"
#include <utils/Log.h>
#include <cutils/properties.h>

#include "SoftFFmpegVideo.h"
#include "FFmpegComponents.h"
#include "ffmpeg_hwaccel.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>

#define DEBUG_PKT 0
#define DEBUG_FRM 0
#define DEBUG_EXTRADATA 0

static int decoder_reorder_pts = -1;

namespace android {

static const CodecProfileLevel kM4VProfileLevels[] = {
    { OMX_VIDEO_MPEG4ProfileSimple, OMX_VIDEO_MPEG4Level5 },
    { OMX_VIDEO_MPEG4ProfileAdvancedSimple, OMX_VIDEO_MPEG4Level5 },
};

static const CodecProfileLevel kAVCProfileLevels[] = {
    // Only declare the highest level for each supported profile
    { OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel52 },
    { OMX_VIDEO_AVCProfileMain,     OMX_VIDEO_AVCLevel52 },
    { OMX_VIDEO_AVCProfileHigh,     OMX_VIDEO_AVCLevel52 },
};

static const CodecProfileLevel kHEVCProfileLevels[] = {
    { OMX_VIDEO_HEVCProfileMain,      OMX_VIDEO_HEVCMainTierLevel51 },
    { OMX_VIDEO_HEVCProfileMainStill, OMX_VIDEO_HEVCMainTierLevel51 },
};

static const CodecProfileLevel kVP9ProfileLevels[] = {
    // Only need to declare the highest supported profile and level here.
    { OMX_VIDEO_VP9Profile0, OMX_VIDEO_VP9Level5 },
    { OMX_VIDEO_VP9Profile2, OMX_VIDEO_VP9Level5 },
};

SoftFFmpegVideo::SoftFFmpegVideo(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const CodecProfileLevel *profileLevels,
        size_t numProfileLevels,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component,
        enum AVCodecID codecID)
    : SoftVideoDecoderOMXComponent(name, componentRole, codingType,
            profileLevels, numProfileLevels, 352, 288, callbacks, appData, component),
      mCodingType(codingType),
      mFFmpegAlreadyInited(false),
      mCodecAlreadyOpened(false),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mPacket(NULL),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mStride(320),
      mSignalledError(false),
      mLoggedError(0) {

    ALOGD("SoftFFmpegVideo component: %s codingType=%d appData: %p", name, codingType, appData);

    initPorts(
            kNumInputBuffers,
            1024 * 1024 /* inputBufferSize */,
            kNumOutputBuffers,
            name);

    CHECK_EQ(initDecoder(codecID), (status_t)OK);
}

SoftFFmpegVideo::~SoftFFmpegVideo() {
    ALOGV("~SoftFFmpegVideo");
    deInitDecoder();
    if (mFFmpegAlreadyInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegVideo::setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec __unused) {
    int fast = property_get_bool("debug.ffmpeg.fast", 0);

    avctx->workaround_bugs   = 1;
    avctx->idct_algo         = 0;
    avctx->skip_frame        = AVDISCARD_DEFAULT;
    avctx->skip_idct         = AVDISCARD_DEFAULT;
    avctx->skip_loop_filter  = AVDISCARD_DEFAULT;
    avctx->error_concealment = 3;

    if (fast)   avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#ifdef CODEC_FLAG_EMU_EDGE
    if (codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
}

status_t SoftFFmpegVideo::initDecoder(enum AVCodecID codecID) {
    status_t status;

    status = initFFmpeg();
    if (status != OK) {
        return NO_INIT;
    }
    mFFmpegAlreadyInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = codecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = mWidth;
    mCtx->height = mHeight;
    ALOGD("%p initDecoder: %p", this, mCtx);
    return OK;
}

void SoftFFmpegVideo::deInitDecoder() {
    ALOGD("%p deInitDecoder: %p", this, mCtx);
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            avcodec_flush_buffers(mCtx);
        }
        if (mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }
        if (mCodecAlreadyOpened) {
            avcodec_close(mCtx);
            mCodecAlreadyOpened = false;
        }
        ffmpeg_hwaccel_deinit(mCtx);
        av_freep(&mCtx);
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    //ALOGV("internalGetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_WMVFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_RVFormatUnused;

            return OMX_ErrorNone;
        }

        default:
        {
            if (index != (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg) {
                return SoftVideoDecoderOMXComponent::internalGetParameter(index, params);
            }

            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eCodecId = AV_CODEC_ID_NONE;
            profile->nWidth   = 0;
            profile->nHeight  = 0;

            return OMX_ErrorNone;
        }
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::isRoleSupported(
                const OMX_PARAM_COMPONENTROLETYPE *roleParams) {
    for (size_t i = 0;
         i < sizeof(kVideoComponents) / sizeof(kVideoComponents[0]);
         ++i) {
        if (strncmp((const char *)roleParams->cRole,
                kVideoComponents[i].mRole, OMX_MAX_STRINGNAME_SIZE - 1) == 0) {
            return OMX_ErrorNone;
        }
    }
    ALOGE("unsupported role: %s", (const char *)roleParams->cRole);
    return OMX_ErrorUndefined;
}

OMX_ERRORTYPE SoftFFmpegVideo::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    //ALOGV("internalSetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;
            return isRoleSupported(roleParams);
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *newParams =
                (OMX_PARAM_PORTDEFINITIONTYPE *)params;
            OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &newParams->format.video;
            OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(newParams->nPortIndex)->mDef;

            uint32_t oldWidth = def->format.video.nFrameWidth;
            uint32_t oldHeight = def->format.video.nFrameHeight;
            uint32_t newWidth = video_def->nFrameWidth;
            uint32_t newHeight = video_def->nFrameHeight;
            if (newWidth != oldWidth || newHeight != oldHeight) {
                bool outputPort = (newParams->nPortIndex == kOutputPortIndex);
                if (outputPort) {
                    ALOGI("OMX_IndexParamPortDefinition (output) width=%d height=%d", newWidth, newHeight);

                    // only update (essentially crop) if size changes
                    mWidth = newWidth;
                    mHeight = newHeight;

                    updatePortDefinitions(true, true);
                    // reset buffer size based on frame size
                    newParams->nBufferSize = def->nBufferSize;
                } else {
                    // For input port, we only set nFrameWidth and nFrameHeight. Buffer size
                    // is updated when configuring the output port using the max-frame-size,
                    // though client can still request a larger size.
                    ALOGI("OMX_IndexParamPortDefinition (input) width=%d height=%d", newWidth, newHeight);
                    def->format.video.nFrameWidth = newWidth;
                    def->format.video.nFrameHeight = newHeight;
                    mCtx->width = newWidth;
                    mCtx->height = newHeight;
                }
            }
            return SoftVideoDecoderOMXComponent::internalSetParameter(index, params);
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_WMVFormat7) {
                mCtx->codec_id = AV_CODEC_ID_WMV1;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_WMV2;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_WMV3;
            } else {
                mCtx->codec_id = AV_CODEC_ID_VC1;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_RVFormatG2) {
                mCtx->codec_id = AV_CODEC_ID_RV20;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_RV30;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_RV40;
            } else {
                ALOGE("unsupported rv codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
        {
            if (index != (OMX_INDEXTYPE)OMX_IndexParamVideoFFmpeg) {
                return SoftVideoDecoderOMXComponent::internalSetParameter(index, params);
            }

            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = (enum AVCodecID)profile->eCodecId;
            mCtx->width    = profile->nWidth;
            mCtx->height   = profile->nHeight;

            ALOGD("got OMX_IndexParamVideoFFmpeg, "
                "eCodecId:%d(%s), width:%u, height:%u",
                profile->eCodecId,
                avcodec_get_name(mCtx->codec_id),
                profile->nWidth,
                profile->nHeight);

            return OMX_ErrorNone;
        }
    }
}

int32_t SoftFFmpegVideo::handleExtradata() {
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = *inQueue.begin();
    OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

#if DEBUG_EXTRADATA
    ALOGD("got extradata, ignore: %d, size: %u",
            mIgnoreExtradata, inHeader->nFilledLen);
    hexdump(inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
#endif

    if (mIgnoreExtradata) {
        ALOGI("got extradata, size: %u, but ignore it", inHeader->nFilledLen);
    } else {
        if (!mExtradataReady) {
            //if (mMode == MODE_H264)
            //it is possible to receive multiple input buffer with OMX_BUFFERFLAG_CODECCONFIG flag.
            //for example, H264, the first input buffer is SPS, and another is PPS!
            int orig_extradata_size = mCtx->extradata_size;
            mCtx->extradata_size += inHeader->nFilledLen;
            mCtx->extradata = (uint8_t *)realloc(mCtx->extradata,
                    mCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!mCtx->extradata) {
                ALOGE("ffmpeg video decoder failed to alloc extradata memory.");
                return ERR_OOM;
            }

            memcpy(mCtx->extradata + orig_extradata_size,
                    inHeader->pBuffer + inHeader->nOffset,
                    inHeader->nFilledLen);
            memset(mCtx->extradata + mCtx->extradata_size, 0,
                    AV_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    inQueue.erase(inQueue.begin());
    inInfo->mOwnedByUs = false;
    notifyEmptyBufferDone(inHeader);

    return ERR_OK;
}

int32_t SoftFFmpegVideo::openDecoder() {
    if (mCodecAlreadyOpened) {
        return ERR_OK;
    }

    if (!mExtradataReady) {
#if DEBUG_EXTRADATA
        ALOGD("extradata is ready, size: %d", mCtx->extradata_size);
        hexdump(mCtx->extradata, mCtx->extradata_size);
#endif
        mExtradataReady = true;
    }

    //find decoder again as codec_id may have changed
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec) {
        ALOGE("ffmpeg video decoder failed to find codec");
        return ERR_CODEC_NOT_FOUND;
    }

    setDefaultCtx(mCtx, mCtx->codec);
    ffmpeg_hwaccel_init(mCtx);

    ALOGD("begin to open ffmpeg decoder(%s) now",
            avcodec_get_name(mCtx->codec_id));

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return ERR_DECODER_OPEN_FAILED;
    }
    mCodecAlreadyOpened = true;

    ALOGD("open ffmpeg video decoder(%s) success",
            avcodec_get_name(mCtx->codec_id));

    mFrame = av_frame_alloc();
    if (!mFrame) {
        ALOGE("oom for video frame");
        return ERR_OOM;
    }

    return ERR_OK;
}

void SoftFFmpegVideo::initPacket(AVPacket *pkt,
        OMX_BUFFERHEADERTYPE *inHeader) {
    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp;
        pkt->dts = inHeader->nTimeStamp;
    } else {
        pkt->data = NULL;
        pkt->size = 0;
        pkt->pts = AV_NOPTS_VALUE;
    }

#if DEBUG_PKT
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        ALOGD("pkt size:%d, pts:%lld", pkt->size, pkt->pts);
    } else {
        ALOGD("pkt size:%d, pts:N/A", pkt->size);
    }
#endif
}

int32_t SoftFFmpegVideo::decodeVideo() {
    int err = 0;
    int gotPic = false;
    int32_t ret = ERR_OK;
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (!inQueue.empty()) {
        inInfo = *inQueue.begin();
        if (inInfo != NULL)  {
            inHeader = inInfo->mHeader;
        }
    }

    if (mEOSStatus == INPUT_EOS_SEEN && (!inHeader || inHeader->nFilledLen == 0)
        && !(mCtx->codec->capabilities & AV_CODEC_CAP_DELAY)) {
        return ERR_FLUSHED;
    }

    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("oom for video packet");
            return ERR_OOM;
        }
    }

    initPacket(mPacket, inHeader);
    err = avcodec_send_packet(mCtx, mPacket);
    av_packet_unref(mPacket);

    if (err < 0 && err != AVERROR(EAGAIN)) {
        ALOGE("ffmpeg video decoder failed to send packet. (%d)", err);
        // don't send error to OMXCodec, skip packet!
    }

    err = avcodec_receive_frame(mCtx, mFrame);

    if (err == 0) {
        gotPic = true;
    } else if (err == AVERROR(EAGAIN)) {
        gotPic = false;
        err = 0;
    }

    if (err < 0) {
        if ((mLoggedError & 0x01) == 0) {
            ALOGE("ffmpeg video decoder failed to decode frame (logged only once). (%d)", err);
            mLoggedError |= 0x01;
        }
        //don't send error to OMXCodec, skip!
        ret = ERR_NO_FRM;
    } else {
        if (!gotPic) {
            //stop sending empty packets if the decoder is finished
            if ((mEOSStatus != INPUT_DATA_AVAILABLE && (mCtx->codec->capabilities & AV_CODEC_CAP_DELAY) &&
                !inHeader) || inHeader->nFilledLen == 0) {
                ALOGD("ffmpeg video decoder flushed.");
                ret = ERR_FLUSHED;
            } else {
                ALOGV("ffmpeg video decoder failed to get frame.");
                ret = ERR_NO_FRM;
            }
        } else {
            err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
            if (err < 0) {
                if ((mLoggedError & 0x02) == 0) {
                    ALOGE("ffmpeg HW video decoder failed to decode frame (logged only once). (%d)", err);
                    mLoggedError |= 0x02;
                }
                //don't send error to OMXCodec, skip!
                ret = ERR_NO_FRM;
            } else {
                ret = ERR_OK;
            }
        }
    }

    if (!inQueue.empty()) {
        inQueue.erase(inQueue.begin());
        if (inInfo) {
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
        }
    }

    return ret;
}

int32_t SoftFFmpegVideo::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    uint8_t *data[4];
    int linesize[4];

    int64_t pts = AV_NOPTS_VALUE;
    uint8_t *dst = outHeader->pBuffer;

    uint32_t bufferWidth = outputBufferWidth();
    uint32_t bufferHeight = outputBufferHeight();
#if DEBUG_FRM
    uint32_t frameWidth = mFrame->width;
    uint32_t frameHeight = mFrame->height;
#endif

    // offsets and linesizes must match expected YUV420p layout in:
    // frameworks/av/media/libstagefright/colorconversion/SoftwareRenderer.cpp

    data[0] = dst;
    data[1] = dst + bufferWidth * bufferHeight;
    data[2] = data[1] + bufferWidth * bufferHeight / 4;
    linesize[0] = bufferWidth;
    linesize[1] = bufferWidth / 2;
    linesize[2] = bufferWidth / 2;

#if DEBUG_FRM
    ALOGD("drainOneOutputBuffer: frame_width=%d frame_height=%d buffer_width=%d buffer_height=%d ctx_width=%d ctx_height=%d mIsAdaptive=%d",
          frameWidth, frameHeight, bufferWidth, bufferHeight, mCtx->width, mCtx->height, mIsAdaptive);
#endif

    int sws_flags = SWS_BICUBIC;
    mImgConvertCtx = sws_getCachedContext(mImgConvertCtx,
           mFrame->width, mFrame->height, (AVPixelFormat)mFrame->format, mFrame->width, mFrame->height,
           AV_PIX_FMT_YUV420P, sws_flags, NULL, NULL, NULL);
    if (mImgConvertCtx == NULL) {
        ALOGE("Cannot initialize the conversion context");
        return ERR_SWS_FAILED;
    }
    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
            0, mFrame->height, data, linesize);

    outHeader->nOffset = 0;
    outHeader->nFilledLen = (bufferWidth * bufferHeight * 3) / 2;
    outHeader->nFlags = 0;
    if (mFrame->key_frame) {
        outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
    }

    //process timestamps
#ifndef LIBAV_CONFIG_H
    if (decoder_reorder_pts == -1) {
        pts = mFrame->best_effort_timestamp;
    } else
#endif
    if (decoder_reorder_pts) {
        pts = mFrame->pts;
    } else {
        pts = mFrame->pkt_dts;
    }

    if (pts == AV_NOPTS_VALUE) {
        pts = 0;
    }
    outHeader->nTimeStamp = pts; //FIXME pts is right???

#if DEBUG_FRM
    ALOGD("mFrame pkt_pts: %lld pkt_dts: %lld used %lld", mFrame->pkt_pts, mFrame->pkt_dts, pts);
#endif

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    return ERR_OK;
}

void SoftFFmpegVideo::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    CHECK(outInfo != NULL);
    outQueue.erase(outQueue.begin());
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    ALOGD("ffmpeg video decoder fill eos outbuf");

    outHeader->nTimeStamp = 0;
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mEOSStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftFFmpegVideo::drainAllOutputBuffers() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
   if (!mCodecAlreadyOpened) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
       return;
   }

    while (!outQueue.empty()) {
        int32_t err = decodeVideo();
        if (err < ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        } else if (err == ERR_FLUSHED) {
            drainEOSOutputBuffer();
            return;
        } else if (err == ERR_NO_FRM) {
            continue;
        } else {
            CHECK_EQ(err, (int32_t)ERR_OK);
        }
        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        }
    }
}

bool SoftFFmpegVideo::handlePortSettingsChange() {
#if 0
    CropSettingsMode crop = kCropUnSet;
    uint32_t width  = outputBufferWidth();
    uint32_t height = outputBufferHeight();
    if (width != (uint32_t)mCtx->width || height != (uint32_t)mCtx->height) {
        crop = kCropSet;
        if (mCropWidth != width || mCropHeight != height) {
            mCropLeft = 0;
            mCropTop = 0;
            mCropWidth = width;
            mCropHeight = height;
            crop = kCropChanged;
        }
    }

    bool portWillReset = false;
    SoftVideoDecoderOMXComponent::handlePortSettingsChange(
            &portWillReset, mCtx->width, mCtx->height, crop);
    return portWillReset;
#else
    bool portWillReset = false;
    SoftVideoDecoderOMXComponent::handlePortSettingsChange(
            &portWillReset, mCtx->width, mCtx->height);
    return portWillReset;
#endif
}

void SoftFFmpegVideo::onQueueFilled(OMX_U32 portIndex __unused) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (((mEOSStatus != INPUT_DATA_AVAILABLE) || !inQueue.empty())
            && !outQueue.empty()) {

        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        BufferInfo *inInfo = *inQueue.begin();
        if (inInfo == NULL) {
            continue;
        }
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
        if (inHeader == NULL) {
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            mEOSStatus = INPUT_EOS_SEEN;
            continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            ALOGD("ffmpeg got codecconfig buffer");
            if (handleExtradata() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
            }
            continue;
        }

        if (!mCodecAlreadyOpened) {
            if (openDecoder() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
        }

        int32_t err = decodeVideo();
        if (err < ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        } else if (err == ERR_FLUSHED) {
            drainEOSOutputBuffer();
            return;
        } else if (err == ERR_NO_FRM) {
            continue;
        } else {
            CHECK_EQ(err, (int32_t)ERR_OK);
        }

        if (handlePortSettingsChange()) {
            ALOGV("PORT RESET w=%d h=%d", mCtx->width, mCtx->height);
            return;
        }

        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
        }
    }
}

void SoftFFmpegVideo::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("ffmpeg video decoder flush port(%u)", portIndex);
    if (portIndex == kInputPortIndex) {
        if (mCtx && avcodec_is_open(mCtx)) {
            //Make sure that the next buffer output does not still
            //depend on fragments from the last one decoded.
            avcodec_flush_buffers(mCtx);
        }
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftFFmpegVideo::onReset() {
    ALOGV("onReset()");
    enum AVCodecID codecID = mCtx->codec_id;
    deInitDecoder();
    initDecoder(codecID);
    SoftVideoDecoderOMXComponent::onReset();
    mSignalledError = false;
    mExtradataReady = false;
    mEOSStatus = INPUT_DATA_AVAILABLE;
}

SoftOMXComponent* SoftFFmpegVideo::createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {

    OMX_VIDEO_CODINGTYPE codingType = OMX_VIDEO_CodingAutoDetect;
    const char *componentRole = NULL;
    enum AVCodecID codecID = AV_CODEC_ID_NONE;

    for (size_t i = 0; i < kNumVideoComponents; ++i) {
        if (!strcasecmp(name, kVideoComponents[i].mName)) {
            componentRole = kVideoComponents[i].mRole;
            codingType = kVideoComponents[i].mVideoCodingType;
            codecID = kVideoComponents[i].mCodecID;
            break;
        }
    }

    if (componentRole == NULL) {
        TRESPASS();
    }

    const CodecProfileLevel *codec_profile_levels;
    size_t codec_array_size;
    if (!strcmp(name, "OMX.ffmpeg.mpeg4.decoder")) {
        codec_profile_levels = kM4VProfileLevels;
        codec_array_size = ARRAY_SIZE(kM4VProfileLevels);
    } else if (!strcmp(name, "OMX.ffmpeg.h264.decoder")) {
        codec_profile_levels = kAVCProfileLevels;
        codec_array_size = ARRAY_SIZE(kAVCProfileLevels);
    } else if (!strcmp(name, "OMX.ffmpeg.hevc.decoder")) {
        codec_profile_levels = kHEVCProfileLevels;
        codec_array_size = ARRAY_SIZE(kHEVCProfileLevels);
    } else if (!strcmp(name, "OMX.ffmpeg.vp9.decoder")) {
        codec_profile_levels = kVP9ProfileLevels;
        codec_array_size = ARRAY_SIZE(kVP9ProfileLevels);
    } else {
        codec_profile_levels = NULL;
        codec_array_size = 0;
    }

    return new SoftFFmpegVideo(name, componentRole, codingType,
                codec_profile_levels, codec_array_size,
                callbacks, appData, component, codecID);
}

}  // namespace android
