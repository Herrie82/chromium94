// Copyright 2017-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "media/filters/neva/emptybuffer_audio_decoder.h"

#include <utility>

#include "base/callback_helpers.h"
#include "base/logging.h"
#include "base/single_thread_task_runner.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_decoder_config.h"
#include "media/base/audio_discard_helper.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/decoder_buffer.h"
#include "media/base/sample_format.h"
#include "neva/logging.h"

namespace media {

EmptyBufferAudioDecoder::EmptyBufferAudioDecoder(
    const scoped_refptr<base::SingleThreadTaskRunner>& task_runner)
    : task_runner_(task_runner), state_(kUninitialized), av_sample_format_(0) {}

EmptyBufferAudioDecoder::~EmptyBufferAudioDecoder() {
  NEVA_DCHECK(task_runner_->BelongsToCurrentThread());

  if (state_ != kUninitialized) {
    ResetTimestampState();
  }
}

void EmptyBufferAudioDecoder::Decode(scoped_refptr<DecoderBuffer> buffer,
                                     DecodeCB decode_cb) {
  NEVA_DCHECK(task_runner_->BelongsToCurrentThread());
  NEVA_DCHECK(!decode_cb.is_null());
  CHECK_NE(state_, kUninitialized);
  DecodeCB decode_cb_bound = BindToCurrentLoop(std::move(decode_cb));

  if (state_ == kError) {
    std::move(decode_cb_bound).Run(DecodeStatus::DECODE_ERROR);
    return;
  }

  // Do nothing if decoding has finished.
  if (state_ == kDecodeFinished) {
    std::move(decode_cb_bound).Run(DecodeStatus::OK);
    return;
  }

  DecodeBuffer(buffer, std::move(decode_cb_bound));
}

void EmptyBufferAudioDecoder::Reset(base::OnceClosure closure) {
  NEVA_DCHECK(task_runner_->BelongsToCurrentThread());
  state_ = kNormal;
  ResetTimestampState();
  task_runner_->PostTask(FROM_HERE, std::move(closure));
}

void EmptyBufferAudioDecoder::DecodeBuffer(
    const scoped_refptr<DecoderBuffer>& buffer,
    DecodeCB decode_cb) {
  NEVA_DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK_NE(state_, kUninitialized);
  DCHECK_NE(state_, kDecodeFinished);
  DCHECK_NE(state_, kError);
  NEVA_DCHECK(buffer);

  // Make sure we are notified if http://crbug.com/49709 returns.  Issue also
  // occurs with some damaged files.
  if (!buffer->end_of_stream() && buffer->timestamp() == kNoTimestamp) {
    DVLOG(1) << "Received a buffer without timestamps!";
    std::move(decode_cb).Run(DecodeStatus::DECODE_ERROR);
    return;
  }

  bool has_produced_frame;
  do {
    has_produced_frame = false;
    if (buffer->data() && buffer->data_size() > 0) {
      if (!FeedForPlatformMediaAudioDecoder(buffer)) {
        state_ = kError;
        std::move(decode_cb).Run(DecodeStatus::DECODE_ERROR);
        return;
      }

      has_produced_frame = true;

      scoped_refptr<AudioBuffer> audio_buffer;
      int frame_count = buffer->duration().InMicroseconds() *
                        config_.samples_per_second() /
                        base::Time::kMicrosecondsPerSecond;
#if defined(OS_WEBOS)
      // FFmpeg returns some audio frames as 0 or 1 duration for some mp4-dash
      // files and frame_count becomes zero. Since AudioBuffer cannot be created
      // with 0 frames so, we need to ignore that frame and proceed.
      if (!frame_count) {
        std::move(decode_cb).Run(DecodeStatus::OK);
        return;
      }
#endif

      audio_buffer = AudioBuffer::CreateEmptyBuffer(
          config_.channel_layout(),
          ChannelLayoutToChannelCount(config_.channel_layout()),
          config_.samples_per_second(), frame_count, buffer->timestamp());

      output_cb_.Run(audio_buffer);
    }
    // Repeat to flush the decoder after receiving EOS buffer.
  } while (buffer->end_of_stream() && has_produced_frame);

  if (buffer->end_of_stream()) {
    if (!FeedForPlatformMediaAudioDecoder(buffer)) {
      state_ = kError;
      std::move(decode_cb).Run(DecodeStatus::DECODE_ERROR);
      return;
    }
    state_ = kDecodeFinished;
  }

  std::move(decode_cb).Run(DecodeStatus::OK);
}

AudioDecoderType EmptyBufferAudioDecoder::GetDecoderType() const {
  return AudioDecoderType::kNeva;
}

bool EmptyBufferAudioDecoder::ConfigureDecoder() {
  if (!config_.IsValidConfig()) {
    DLOG(ERROR) << "Invalid audio stream -"
                << " codec: " << config_.codec()
                << " channel layout: " << config_.channel_layout()
                << " bits per channel: " << config_.bits_per_channel()
                << " samples per second: " << config_.samples_per_second();
    return false;
  }

  if (config_.is_encrypted()) {
    DLOG(ERROR) << "Encrypted audio stream not supported";
    return false;
  }

  discard_helper_.reset(new AudioDiscardHelper(config_.samples_per_second(),
                                               config_.codec_delay(), false));
  ChannelLayoutToChannelCount(config_.channel_layout());
  ResetTimestampState();
  return true;
}

void EmptyBufferAudioDecoder::ResetTimestampState() {
  discard_helper_->Reset(config_.codec_delay());
}

}  // namespace media
