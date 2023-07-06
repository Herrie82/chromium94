// Copyright 2020 LG Electronics, Inc.
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

#include "media/webrtc/neva/webrtc_pass_through_video_decoder.h"

#include "base/command_line.h"
#include "base/containers/contains.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_util.h"
#include "content/renderer/media/neva/mojo_media_player_factory.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_switches_neva.h"
#include "media/base/media_util.h"
#include "media/base/video_frame.h"
#include "media/neva/media_preferences.h"
#include "third_party/blink/renderer/platform/webrtc/webrtc_video_frame_adapter.h"
#include "third_party/webrtc/api/video/encoded_image.h"
#include "third_party/webrtc/api/video_codecs/sdp_video_format.h"
#include "third_party/webrtc/modules/video_coding/include/video_error_codes.h"
#include "third_party/webrtc/rtc_base/helpers.h"
#include "third_party/webrtc/rtc_base/ref_counted_object.h"

namespace media {

namespace {

const char* kImplementationName = "WebRtcPassThroughVideoDecoder";

// Maximum number of frames that we will queue in |pending_buffers_|.
constexpr int32_t kMaxPendingBuffers = 8;

// Maximum number of timestamps that will be maintained in |decode_timestamps_|.
// Really only needs to be a bit larger than the maximum reorder distance (which
// is presumably 0 for WebRTC), but being larger doesn't hurt much.
constexpr int32_t kMaxDecodeHistory = 32;

// Maximum number of consecutive frames that can fail to decode before
// requesting fallback to software decode.
constexpr int32_t kMaxConsecutiveErrors = 5;

// Maximum number seconds to wait for initialization to complete
constexpr int32_t kTimeoutSeconds = 10;

// Maximum number of HW decoder instances that can be used
// -1 allows unlimited instances till platform resources exhausted
constexpr int32_t kMaxDecodersSupported = 4;

// Map webrtc::VideoCodecType to media::VideoCodec.
media::VideoCodec ToVideoCodec(webrtc::VideoCodecType webrtc_codec) {
  switch (webrtc_codec) {
    case webrtc::kVideoCodecVP8:
      return media::kCodecVP8;
    case webrtc::kVideoCodecVP9:
      return media::kCodecVP9;
    case webrtc::kVideoCodecH264:
      return media::kCodecH264;
    default:
      break;
  }
  return media::kUnknownVideoCodec;
}

VideoDecoderConfig GetVideoConfig(VideoCodec video_codec,
                                  const gfx::Size& default_size) {
  VideoCodecProfile profile = media::VIDEO_CODEC_PROFILE_UNKNOWN;
  switch (video_codec) {
    case media::kCodecH264:
      profile = media::H264PROFILE_MIN;
      break;
    case media::kCodecVP8:
      profile = media::VP8PROFILE_ANY;
      break;
    case media::kCodecVP9:
      profile = media::VP9PROFILE_MIN;
      break;
    default:
      // forgot to handle new encoded video format?
      NOTREACHED();
      break;
  }

  VideoDecoderConfig video_config(
      video_codec, profile, media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      media::VideoColorSpace(), media::kNoTransformation, default_size,
      gfx::Rect(default_size), default_size, media::EmptyExtraData(),
      media::EncryptionScheme::kUnencrypted);
  video_config.set_live_stream(true);

  VLOG(1) << __func__ << " config=" << video_config.AsHumanReadableString();

  return video_config;
}

void FinishWait(base::WaitableEvent* waiter, bool* result_out, bool result) {
  *result_out = result;
  waiter->Signal();
}

}  // namespace

#define BIND_TO_MAIN_TASK(function)     \
  base::BindPostTask(main_task_runner_, \
                     base::BindRepeating(function, weak_this_), FROM_HERE)

// static
int32_t WebRtcPassThroughVideoDecoder::current_decoder_count_ = 0;

// static
std::unique_ptr<WebRtcPassThroughVideoDecoder>
WebRtcPassThroughVideoDecoder::Create(
    scoped_refptr<base::SequencedTaskRunner> main_task_runner,
    scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    const webrtc::SdpVideoFormat& sdp_format) {
  DVLOG(1) << __func__ << "(" << sdp_format.name << ")";

  const webrtc::VideoCodecType webrtc_codec_type =
      webrtc::PayloadStringToCodecType(sdp_format.name);

  // Bail early for unknown codecs.
  media::VideoCodec video_codec = ToVideoCodec(webrtc_codec_type);
  if (video_codec == media::kUnknownVideoCodec)
    return nullptr;

  // Fallback to software decoder if not supported by platform.
  const std::string& codec_name = base::ToUpperASCII(GetCodecName(video_codec));
  const auto capability =
      MediaPreferences::Get()->GetMediaCodecCapabilityForCodec(codec_name);
  if (!capability.has_value()) {
    VLOG(1) << __func__ << " " << codec_name << " is unsupported by HW";
    return nullptr;
  }

  if (kMaxDecodersSupported != -1 &&
      current_decoder_count_ >= kMaxDecodersSupported) {
    return nullptr;
  }

  return base::WrapUnique(new WebRtcPassThroughVideoDecoder(
      main_task_runner, media_task_runner, video_codec));
}

WebRtcPassThroughVideoDecoder::WebRtcPassThroughVideoDecoder(
    scoped_refptr<base::SequencedTaskRunner> main_task_runner,
    scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    media::VideoCodec video_codec)
    : video_codec_(video_codec),
      main_task_runner_(main_task_runner),
      media_task_runner_(
          static_cast<base::SingleThreadTaskRunner*>(media_task_runner.get())) {
  VLOG(1) << __func__ << "[" << this << "] "
          << " codec: " << GetCodecName(video_codec);
  weak_this_ = weak_this_factory_.GetWeakPtr();

  current_decoder_count_++;
}

WebRtcPassThroughVideoDecoder::~WebRtcPassThroughVideoDecoder() {
  VLOG(1) << __func__ << "[" << this << "] ";

  current_decoder_count_--;

  if (media_platform_api_)
    media_platform_api_->Finalize();
}

int32_t WebRtcPassThroughVideoDecoder::InitDecode(
    const webrtc::VideoCodec* codec_settings,
    int32_t number_of_cores) {
  VLOG(1) << __func__ << "[" << this << "] "
          << " codec: " << GetCodecName(video_codec_)
          << " has_error_=" << has_error_;

  video_codec_type_ = codec_settings->codecType;

  base::AutoLock auto_lock(lock_);
  return has_error_ ? WEBRTC_VIDEO_CODEC_UNINITIALIZED : WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebRtcPassThroughVideoDecoder::Decode(
    const webrtc::EncodedImage& input_image,
    bool missing_frames,
    int64_t render_time_ms) {
  // Hardware VP9 decoders don't handle more than one spatial layer. Fall back
  // to software decoding. See https://crbug.com/webrtc/9304.
  if (video_codec_type_ == webrtc::kVideoCodecVP9 &&
      input_image.SpatialIndex().value_or(0) > 0) {
    LOG(WARNING) << __func__
                 << " VP9 with more spatial index > 0. Fallback to s/w Decoder";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  if (missing_frames) {
    VLOG(1) << __func__ << " Missing or incomplete frames";
    // We probably can't handle broken frames. Request a key frame.
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (is_suspended_) {
    VLOG(1) << __func__ << " Player suspended. Return!";
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  if (!input_image.data() || !input_image.size()) {
    LOG(ERROR) << __func__ << " Invalid Encoded Image";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (key_frame_required_) {
    // We discarded previous frame because we have too many pending frames
    // (see logic) below. Now we need to wait for the key frame and discard
    // everything else.
    if (input_image._frameType != webrtc::VideoFrameType::kVideoFrameKey) {
      DVLOG(2) << __func__ << "Discard non-key frame";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    DVLOG(2) << __func__ << "Key frame received, resume decoding";

    // ok, we got key frame and can continue decoding
    key_frame_required_ = false;
  }

  bool is_key_frame =
      input_image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
  if (is_key_frame) {
    frame_size_.set_width(input_image._encodedWidth);
    frame_size_.set_height(input_image._encodedHeight);
  }

  const base::TimeDelta incoming_timestamp =
      base::TimeDelta::FromMicroseconds(input_image.Timestamp());
  // To avoid buffer copy create an empty decode buffer and set the
  // encoded data pointer in it.
  scoped_refptr<DecoderBuffer> decode_buffer =
      base::WrapRefCounted(new media::DecoderBuffer(0));
  if (!decode_buffer) {
    LOG(ERROR) << __func__ << " Could not allocate decode_buffer.";
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  decode_buffer->set_encoded_data(input_image.GetEncodedData());
  decode_buffer->set_is_key_frame(is_key_frame);
  decode_buffer->set_timestamp(incoming_timestamp);

  // Queue for decoding.
  {
    base::AutoLock auto_lock(lock_);

    if (has_error_) {
      LOG(WARNING) << __func__ << " Got error. Fallback to software";
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    if (pending_buffers_.size() >= kMaxPendingBuffers) {
      // We are severely behind. Drop pending frames and request a keyframe to
      // catch up as quickly as possible.
      VLOG(1) << __func__ << " Pending VideoFrames overflow";
      pending_buffers_.clear();

      // Actually we just discarded a frame. We must wait for the key frame and
      // drop any other non-key frame.
      key_frame_required_ = true;
      if (++consecutive_error_count_ > kMaxConsecutiveErrors) {
        decode_timestamps_.clear();
        LOG(WARNING) << __func__ << " error_count=" << consecutive_error_count_
                     << ", Fallback to software";
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    pending_buffers_.push_back(std::move(decode_buffer));
  }

  if (!player_load_notified_) {
    // We notify and wait for the player to cmplete initialization
    if (!InitializeMediaPlayer(incoming_timestamp))
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&WebRtcPassThroughVideoDecoder::DecodeOnMediaThread,
                     weak_this_));
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebRtcPassThroughVideoDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  base::AutoLock auto_lock(lock_);
  decode_complete_callback_ = callback;
  VLOG(1) << __func__ << " decoder available: " << !has_error_;
  return has_error_ ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                    : WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebRtcPassThroughVideoDecoder::Release() {
  VLOG(1) << __func__ << " decoder available: " << !has_error_;

  base::AutoLock auto_lock(lock_);

  pending_buffers_.clear();
  decode_timestamps_.clear();

  return has_error_ ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                    : WEBRTC_VIDEO_CODEC_OK;
}

const char* WebRtcPassThroughVideoDecoder::ImplementationName() const {
  return kImplementationName;
}

void WebRtcPassThroughVideoDecoder::OnMediaPlayerInitCb(
    const std::string& app_id,
    const std::string& window_id,
    const base::RepeatingClosure& suspend_done_cb,
    const base::RepeatingClosure& resume_done_cb,
    const MediaPlatformAPI::VideoSizeChangedCB& video_size_changed_cb,
    const MediaPlatformAPI::ActiveRegionCB& active_region_cb) {
  VLOG(1) << __func__ << " app_id=" << app_id << " window_id=" << window_id;

  base::AutoLock auto_lock(lock_);

  app_id_ = app_id;
  window_id_ = window_id;

  resume_done_cb_ = resume_done_cb;
  suspend_done_cb_ = suspend_done_cb;
  video_size_changed_cb_ = video_size_changed_cb;
  active_region_cb_ = active_region_cb;

  if (app_id_.empty() || window_id_.empty()) {
    // We have fallen back to SW decoding.
    main_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&WebRtcPassThroughVideoDecoder::ReleaseMediaPlatformAPI,
                       weak_this_));
  } else {
    main_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&WebRtcPassThroughVideoDecoder::CreateMediaPlatformAPI,
                       weak_this_));
  }
}

void WebRtcPassThroughVideoDecoder::OnMediaPlayerSuspendCb(bool suspend) {
  VLOG(1) << __func__ << " suspend=[" << is_suspended_ << " -> " << suspend
          << "]";

  base::AutoLock auto_lock(lock_);

  if (!media_platform_api_)
    return;

  if (is_suspended_ == suspend)
    return;

  is_suspended_ = suspend;
  if (is_suspended_) {
    media_platform_api_->Suspend(media::SuspendReason::kBackgrounded);
  } else {
    key_frame_required_ = true;
    media_platform_api_->Resume(base::TimeDelta::FromMilliseconds(0),
                                media::RestorePlaybackMode::kPlaying);
  }
}

void WebRtcPassThroughVideoDecoder::DecodeOnMediaThread() {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  while (outstanding_decode_requests_ < kMaxPendingBuffers) {
    scoped_refptr<media::DecoderBuffer> buffer;
    {
      base::AutoLock auto_lock(lock_);

      // Take the first pending buffer.
      if (pending_buffers_.empty())
        return;

      buffer = pending_buffers_.front();
      pending_buffers_.pop_front();

      // Record the timestamp.
      while (decode_timestamps_.size() >= kMaxDecodeHistory)
        decode_timestamps_.pop_front();
      decode_timestamps_.push_back(buffer->timestamp());
    }

    if (is_suspended_)
      continue;

    // Submit for decoding.
    outstanding_decode_requests_++;
    const base::TimeDelta incoming_timestamp = buffer->timestamp();

    if (media_platform_api_->Feed(std::move(buffer), FeedType::kVideo)) {
      DVLOG(2) << __func__ << " Feed Success! ts=" << buffer->timestamp();
      media_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&WebRtcPassThroughVideoDecoder::ReturnEmptyOutputFrame,
                         weak_this_, incoming_timestamp));
    } else {
      LOG(WARNING) << __func__ << " Feed Failed! ts=" << buffer->timestamp();
      base::AutoLock auto_lock(lock_);
      has_error_ = true;
      pending_buffers_.clear();
      decode_timestamps_.clear();
    }
  }
}

void WebRtcPassThroughVideoDecoder::ReturnEmptyOutputFrame(
    const base::TimeDelta& timestamp) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  outstanding_decode_requests_--;

  // Make a shallow copy.
  scoped_refptr<media::VideoFrame> video_frame =
      media::VideoFrame::CreateTransparentFrame(frame_size_);
  if (!video_frame) {
    LOG(ERROR) << __func__ << " Could not allocate video_frame.";
    return;
  }

  // The bind ensures that we keep a pointer to the encoded data.
  video_frame->set_timestamp(timestamp);
  video_frame->metadata().is_transparent_frame = true;

  if (!player_load_notified_) {
    video_frame->metadata().media_player_init_cb =
        BindToCurrentLoop(base::BindRepeating(
            &WebRtcPassThroughVideoDecoder::OnMediaPlayerInitCb, weak_this_));
    video_frame->metadata().media_player_suspend_cb =
        BindToCurrentLoop(base::BindRepeating(
            &WebRtcPassThroughVideoDecoder::OnMediaPlayerSuspendCb,
            weak_this_));
    player_load_notified_ = true;
  }

  SendEmptyRtcFrame(std::move(video_frame));
}

void WebRtcPassThroughVideoDecoder::SendEmptyRtcFrame(
    scoped_refptr<media::VideoFrame> video_frame) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  const base::TimeDelta timestamp = video_frame->timestamp();
  webrtc::VideoFrame rtc_frame =
      webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(
              new rtc::RefCountedObject<blink::WebRtcVideoFrameAdapter>(
                  std::move(video_frame)))
          .set_timestamp_rtp(static_cast<uint32_t>(timestamp.InMicroseconds()))
          .set_timestamp_us(0)
          .set_rotation(webrtc::kVideoRotation_0)
          .build();

  base::AutoLock auto_lock(lock_);

  if (!base::Contains(decode_timestamps_, timestamp)) {
    LOG(WARNING) << __func__
                 << " Discarding frame with timestamp: " << timestamp;
    return;
  }

  DCHECK(decode_complete_callback_);
  decode_complete_callback_->Decoded(rtc_frame);
  consecutive_error_count_ = 0;
}

void WebRtcPassThroughVideoDecoder::CreateMediaPlatformAPI() {
  VLOG(1) << __func__;

  // If this decoder is used for a new player than old MediaPlatformAPI
  // need to be destroyed because the window id might have changed already.
  DestroyMediaPlatformAPI();

  media::CreateMediaPlatformAPICB create_media_platform_api_cb;
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableNevaMediaService)) {
    create_media_platform_api_cb =
        base::BindRepeating(&media::CreateMojoMediaPlatformAPI);
  }

  // Create MediaPlatformAPI
  media::MediaPlatformAPI::ActiveRegionCB empty_active_region_cb;
  if (create_media_platform_api_cb) {
    media_platform_api_ = create_media_platform_api_cb.Run(
        media_task_runner_, true, app_id_, video_size_changed_cb_,
        resume_done_cb_, suspend_done_cb_, active_region_cb_,
        BIND_TO_MAIN_TASK(&WebRtcPassThroughVideoDecoder::OnPipelineError));
  } else {
    media_platform_api_ = media::MediaPlatformAPI::Create(
        media_task_runner_, true, app_id_, video_size_changed_cb_,
        resume_done_cb_, suspend_done_cb_, active_region_cb_,
        BIND_TO_MAIN_TASK(&WebRtcPassThroughVideoDecoder::OnPipelineError));
  }

  if (!media_platform_api_) {
    LOG(ERROR) << __func__ << " Could not create media_platform_api";
    if (!player_loaded_cb_.is_null())
      std::move(player_loaded_cb_).Run(false);
    return;
  }

  media_platform_api_->SetMediaPreferences(
      media::MediaPreferences::Get()->GetRawMediaPreferences());
  media_platform_api_->SetMediaCodecCapabilities(
      media::MediaPreferences::Get()->GetMediaCodecCapabilities());

  media_platform_api_->SetMediaLayerId(window_id_);
  media_platform_api_->SetDisableAudio(true);

  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&WebRtcPassThroughVideoDecoder::InitMediaPlatformAPI,
                     weak_this_));
}

void WebRtcPassThroughVideoDecoder::DestroyMediaPlatformAPI() {
  VLOG(1) << __func__;

  if (!media_platform_api_)
    return;

  media_platform_api_->Finalize();
  media_platform_api_ = nullptr;
}

void WebRtcPassThroughVideoDecoder::InitMediaPlatformAPI() {
  DCHECK(media_task_runner_->BelongsToCurrentThread());
  VLOG(1) << __func__ << " frame_size: " << frame_size_.ToString();

  media::AudioDecoderConfig audio_config;  // Just video data only
  media::VideoDecoderConfig video_config =
      GetVideoConfig(video_codec_, frame_size_);
  media_platform_api_->Initialize(
      audio_config, video_config,
      base::BindRepeating(
          &WebRtcPassThroughVideoDecoder::OnMediaPlatformAPIInitialized,
          weak_this_));
}

void WebRtcPassThroughVideoDecoder::ReleaseMediaPlatformAPI() {
  VLOG(1) << __func__;

  is_destroying_ = true;
  player_load_notified_ = false;

  DestroyMediaPlatformAPI();
}

void WebRtcPassThroughVideoDecoder::OnMediaPlatformAPIInitialized(
    media::PipelineStatus status) {
  VLOG(1) << __func__ << " status : " << status;

  if (is_destroying_ || !media_platform_api_) {
    LOG(ERROR) << __func__ << " Is destroying";
    return;
  }

  {
    base::AutoLock auto_lock(lock_);
    has_error_ = !(status == media::PIPELINE_OK);
  }

  media_platform_api_->SetPlaybackRate(1.0f);

  if (!player_loaded_cb_.is_null())
    std::move(player_loaded_cb_).Run(status == media::PIPELINE_OK);
}

void WebRtcPassThroughVideoDecoder::OnPipelineError(
    media::PipelineStatus status) {
  LOG(ERROR) << __func__ << "[" << this << "] "
             << " status : " << status;

  if (is_destroying_)
    return;

  {
    base::AutoLock auto_lock(lock_);

    pending_buffers_.clear();
    decode_timestamps_.clear();

    has_error_ = (status != media::PIPELINE_OK);
  }
}

bool WebRtcPassThroughVideoDecoder::InitializeMediaPlayer(
    const base::TimeDelta& start_time) {
  VLOG(1) << __func__ << " start_time=" << start_time;

  // Needed for the Contains check in SendEmptyRtcFrame
  decode_timestamps_.push_back(start_time);

  bool result = false;
  base::WaitableEvent waiter(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
  auto player_loaded_cb = base::BindOnce(&FinishWait, base::Unretained(&waiter),
                                         base::Unretained(&result));

  player_loaded_cb_ = std::move(player_loaded_cb);
  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&WebRtcPassThroughVideoDecoder::ReturnEmptyOutputFrame,
                     weak_this_, start_time));

  // We will wait for the media pipeline load complete by media player
  if (!waiter.TimedWait(base::TimeDelta::FromSeconds(kTimeoutSeconds))) {
    LOG(WARNING) << __func__ << " Initialize task timed out.";
    return false;
  }

  LOG(INFO) << __func__ << " result: " << (result ? "Success" : "Fail");
  return result;
}

}  // namespace media
