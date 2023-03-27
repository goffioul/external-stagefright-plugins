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

#ifndef FFMPEG_UTILS_H_

#define FFMPEG_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Condition.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>

extern "C" {

#include "config.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libswscale/swscale.h"
#ifdef LIBAV_CONFIG_H
#include "libavresample/avresample.h"
#else
#include "libswresample/swresample.h"
#endif
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include <system/audio.h>

}

//XXX hack!!!
#define SF_NOPTS_VALUE ((uint64_t)AV_NOPTS_VALUE-1)

namespace android {

//////////////////////////////////////////////////////////////////////////////////
// log
//////////////////////////////////////////////////////////////////////////////////
void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl);
void nam_av_log_set_flags(int arg);

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////
status_t initFFmpeg();
void deInitFFmpeg();

//////////////////////////////////////////////////////////////////////////////////
// parser
//////////////////////////////////////////////////////////////////////////////////
int is_extradata_compatible_with_android(AVCodecParameters *avpar);
int parser_split(AVCodecParameters *avpar, const uint8_t *buf, int buf_size);

//////////////////////////////////////////////////////////////////////////////////
// packet queue
//////////////////////////////////////////////////////////////////////////////////

typedef struct PacketQueue PacketQueue;

PacketQueue* packet_queue_alloc();
void packet_queue_free(PacketQueue **q);
void packet_queue_flush(PacketQueue *q);
void packet_queue_start(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
int packet_queue_is_wait_for_data(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

//////////////////////////////////////////////////////////////////////////////////
// misc
//////////////////////////////////////////////////////////////////////////////////
bool setup_vorbis_extradata(uint8_t **extradata, int *extradata_size,
        const uint8_t *header_start[3], const int header_len[3]);

int64_t get_timestamp(void);

audio_format_t to_android_audio_format(enum AVSampleFormat fmt);

}  // namespace android

#endif  // FFMPEG_UTILS_H_
