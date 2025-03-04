// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/web/modules/mediastream/webmediaplayer_ms.h"

#include <stddef.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "cc/layers/video_frame_provider_client_impl.h"
#include "cc/layers/video_layer.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/media_content_type.h"
#include "media/base/media_log.h"
#include "media/base/video_frame.h"
#include "media/base/video_transformation.h"
#include "media/base/video_types.h"
#include "media/mojo/mojom/media_metrics_provider.mojom.h"
#include "media/video/gpu_memory_buffer_video_frame_pool.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/common/browser_interface_broker_proxy.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_audio_renderer.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_video_renderer.h"
#include "third_party/blink/public/platform/modules/webrtc/webrtc_logging.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/url_conversion.h"
#include "third_party/blink/public/platform/web_media_player_client.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/public/platform/web_surface_layer_bridge.h"
#include "third_party/blink/public/web/modules/media/webmediaplayer_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_local_frame_wrapper.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_renderer_factory.h"
#include "third_party/blink/renderer/modules/mediastream/webmediaplayer_ms_compositor.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_audio_track.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_component.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_descriptor.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_source.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/timer.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

#if defined(USE_WEBOS_AUDIO)
#include "media/audio/audio_device_description.h"
#endif

namespace WTF {

template <>
struct CrossThreadCopier<viz::SurfaceId>
    : public CrossThreadCopierPassThrough<viz::SurfaceId> {
  STATIC_ONLY(CrossThreadCopier);
};

}  // namespace WTF

namespace blink {

namespace {

enum class RendererReloadAction {
  KEEP_RENDERER,
  REMOVE_RENDERER,
  NEW_RENDERER
};

bool IsPlayableTrack(MediaStreamComponent* component) {
  return component && component->Source() &&
         component->Source()->GetReadyState() !=
             MediaStreamSource::kReadyStateEnded;
}

const char* LoadTypeToString(WebMediaPlayer::LoadType type) {
  switch (type) {
    case WebMediaPlayer::kLoadTypeURL:
      return "URL";
    case WebMediaPlayer::kLoadTypeMediaSource:
      return "MediaSource";
    case WebMediaPlayer::kLoadTypeMediaStream:
      return "MediaStream";
#if defined(USE_NEVA_MEDIA)
    case blink::WebMediaPlayer::kLoadTypeBlobURL:
      return "BlobURL";
    case blink::WebMediaPlayer::kLoadTypeDataURL:
      return "DataURL";
#endif
  }
}

const char* ReadyStateToString(WebMediaPlayer::ReadyState state) {
  switch (state) {
    case WebMediaPlayer::kReadyStateHaveNothing:
      return "HaveNothing";
    case WebMediaPlayer::kReadyStateHaveMetadata:
      return "HaveMetadata";
    case WebMediaPlayer::kReadyStateHaveCurrentData:
      return "HaveCurrentData";
    case WebMediaPlayer::kReadyStateHaveFutureData:
      return "HaveFutureData";
    case WebMediaPlayer::kReadyStateHaveEnoughData:
      return "HaveEnoughData";
  }
}

const char* NetworkStateToString(WebMediaPlayer::NetworkState state) {
  switch (state) {
    case WebMediaPlayer::kNetworkStateEmpty:
      return "Empty";
    case WebMediaPlayer::kNetworkStateIdle:
      return "Idle";
    case WebMediaPlayer::kNetworkStateLoading:
      return "Loading";
    case WebMediaPlayer::kNetworkStateLoaded:
      return "Loaded";
    case WebMediaPlayer::kNetworkStateFormatError:
      return "FormatError";
    case WebMediaPlayer::kNetworkStateNetworkError:
      return "NetworkError";
    case WebMediaPlayer::kNetworkStateDecodeError:
      return "DecodeError";
  }
}

constexpr base::TimeDelta kForceBeginFramesTimeout =
    base::TimeDelta::FromSeconds(1);
}  // namespace

#if defined(OS_WIN)
// Since we do not have native GMB support in Windows, using GMBs can cause a
// CPU regression. This is more apparent and can have adverse affects in lower
// resolution content which are defined by these thresholds, see
// https://crbug.com/835752.
// static
const gfx::Size WebMediaPlayerMS::kUseGpuMemoryBufferVideoFramesMinResolution =
    gfx::Size(1920, 1080);
#endif  // defined(OS_WIN)

// FrameDeliverer is responsible for delivering frames received on
// the IO thread by calling of EnqueueFrame() method of |compositor_|.
//
// It is created on the main thread, but methods should be called and class
// should be destructed on the IO thread.
class WebMediaPlayerMS::FrameDeliverer {
 public:
  using RepaintCB = WTF::CrossThreadRepeatingFunction<
      void(scoped_refptr<media::VideoFrame> frame, bool is_copy)>;
  FrameDeliverer(const base::WeakPtr<WebMediaPlayerMS>& player,
                 RepaintCB enqueue_frame_cb,
                 scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
                 scoped_refptr<base::TaskRunner> worker_task_runner,
                 media::GpuVideoAcceleratorFactories* gpu_factories)
      : main_task_runner_(base::ThreadTaskRunnerHandle::Get()),
        player_(player),
        enqueue_frame_cb_(std::move(enqueue_frame_cb)),
        media_task_runner_(media_task_runner) {
    DETACH_FROM_THREAD(io_thread_checker_);

    if (gpu_factories && gpu_factories->ShouldUseGpuMemoryBuffersForVideoFrames(
                             true /* for_media_stream */)) {
      gpu_memory_buffer_pool_ =
          std::make_unique<media::GpuMemoryBufferVideoFramePool>(
              media_task_runner, worker_task_runner, gpu_factories);
    }
  }

  ~FrameDeliverer() {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    if (gpu_memory_buffer_pool_) {
      DropCurrentPoolTasks();
      media_task_runner_->DeleteSoon(FROM_HERE,
                                     gpu_memory_buffer_pool_.release());
    }
  }

  void OnVideoFrame(scoped_refptr<media::VideoFrame> frame) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);

#if defined(USE_NEVA_WEBRTC)
    if (player_ && !player_->HandleVideoFrame(frame) &&
        frame->metadata().is_transparent_frame) {
      return;
    }
#endif

// On Android, stop passing frames.
#if defined(OS_ANDROID)
    if (render_frame_suspended_)
      return;
#endif  // defined(OS_ANDROID)

    if (!gpu_memory_buffer_pool_) {
      int original_frame_id = frame->unique_id();
      EnqueueFrame(original_frame_id, std::move(frame));
      return;
    }

#if defined(OS_WIN)
    const bool skip_creating_gpu_memory_buffer =
        frame->visible_rect().width() <
            kUseGpuMemoryBufferVideoFramesMinResolution.width() ||
        frame->visible_rect().height() <
            kUseGpuMemoryBufferVideoFramesMinResolution.height();
#else
    const bool skip_creating_gpu_memory_buffer = false;
#endif  // defined(OS_WIN)

    // If |render_frame_suspended_|, we can keep passing the frames to keep the
    // latest frame in compositor up to date. However, creating GMB backed
    // frames is unnecessary, because the frames are not going to be shown for
    // the time period.
    if (render_frame_suspended_ || skip_creating_gpu_memory_buffer) {
      int original_frame_id = frame->unique_id();
      EnqueueFrame(original_frame_id, std::move(frame));
      // If there are any existing MaybeCreateHardwareFrame() calls, we do not
      // want those frames to be placed after the current one, so just drop
      // them.
      DropCurrentPoolTasks();
      return;
    }

    int original_frame_id = frame->unique_id();

    // |gpu_memory_buffer_pool_| deletion is going to be posted to
    // |media_task_runner_|. base::Unretained() usage is fine since
    // |gpu_memory_buffer_pool_| outlives the task.
    //
    // TODO(crbug.com/964947): Converting this to PostCrossThreadTask requires
    // re-binding a CrossThreadOnceFunction instance.
    media_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &media::GpuMemoryBufferVideoFramePool::MaybeCreateHardwareFrame,
            base::Unretained(gpu_memory_buffer_pool_.get()), std::move(frame),
            media::BindToCurrentLoop(base::BindOnce(
                &FrameDeliverer::EnqueueFrame,
                weak_factory_for_pool_.GetWeakPtr(), original_frame_id))));
  }

  void SetRenderFrameSuspended(bool render_frame_suspended) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    render_frame_suspended_ = render_frame_suspended;
  }

  WTF::CrossThreadRepeatingFunction<
      void(scoped_refptr<media::VideoFrame> frame)>
  GetRepaintCallback() {
    return CrossThreadBindRepeating(&FrameDeliverer::OnVideoFrame,
                                    weak_factory_.GetWeakPtr());
  }

 private:
  friend class WebMediaPlayerMS;

  void EnqueueFrame(int original_frame_id,
                    scoped_refptr<media::VideoFrame> frame) {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);

    {
      bool tracing_enabled = false;
      TRACE_EVENT_CATEGORY_GROUP_ENABLED("media", &tracing_enabled);
      if (tracing_enabled) {
        if (frame->metadata().reference_time.has_value()) {
          TRACE_EVENT1("media", "EnqueueFrame", "Ideal Render Instant",
                       frame->metadata().reference_time->ToInternalValue());
        } else {
          TRACE_EVENT0("media", "EnqueueFrame");
        }
      }
    }

    bool is_copy = original_frame_id != frame->unique_id();
    enqueue_frame_cb_.Run(std::move(frame), is_copy);
  }

  void DropCurrentPoolTasks() {
    DCHECK_CALLED_ON_VALID_THREAD(io_thread_checker_);
    DCHECK(gpu_memory_buffer_pool_);

    if (!weak_factory_for_pool_.HasWeakPtrs())
      return;

    //  |gpu_memory_buffer_pool_| deletion is going to be posted to
    //  |media_task_runner_|. CrossThreadUnretained() usage is fine since
    //  |gpu_memory_buffer_pool_| outlives the task.
    PostCrossThreadTask(
        *media_task_runner_, FROM_HERE,
        CrossThreadBindOnce(
            &media::GpuMemoryBufferVideoFramePool::Abort,
            CrossThreadUnretained(gpu_memory_buffer_pool_.get())));
    weak_factory_for_pool_.InvalidateWeakPtrs();
  }

  bool render_frame_suspended_ = false;

  const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
  const base::WeakPtr<WebMediaPlayerMS> player_;
  RepaintCB enqueue_frame_cb_;

  // Pool of GpuMemoryBuffers and resources used to create hardware frames.
  std::unique_ptr<media::GpuMemoryBufferVideoFramePool> gpu_memory_buffer_pool_;
  const scoped_refptr<base::SingleThreadTaskRunner> media_task_runner_;

  // Used for DCHECKs to ensure method calls are executed on the correct thread.
  THREAD_CHECKER(io_thread_checker_);

  base::WeakPtrFactory<FrameDeliverer> weak_factory_for_pool_{this};
  base::WeakPtrFactory<FrameDeliverer> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(FrameDeliverer);
};

WebMediaPlayerMS::WebMediaPlayerMS(
    WebLocalFrame* frame,
    WebMediaPlayerClient* client,
    WebMediaPlayerDelegate* delegate,
    std::unique_ptr<media::MediaLog> media_log,
    scoped_refptr<base::SingleThreadTaskRunner> main_render_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> compositor_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
    scoped_refptr<base::TaskRunner> worker_task_runner,
    media::GpuVideoAcceleratorFactories* gpu_factories,
    const WebString& sink_id,
    CreateSurfaceLayerBridgeCB create_bridge_callback,
    std::unique_ptr<WebVideoFrameSubmitter> submitter,
    WebMediaPlayer::SurfaceLayerMode surface_layer_mode)
    : internal_frame_(std::make_unique<MediaStreamInternalFrameWrapper>(frame)),
      network_state_(WebMediaPlayer::kNetworkStateEmpty),
      ready_state_(WebMediaPlayer::kReadyStateHaveNothing),
      buffered_(static_cast<size_t>(0)),
      client_(client),
      delegate_(delegate),
      delegate_id_(0),
      paused_(true),
      video_transformation_(media::kNoTransformation),
      media_log_(std::move(media_log)),
      renderer_factory_(std::make_unique<MediaStreamRendererFactory>()),
      main_render_task_runner_(std::move(main_render_task_runner)),
      io_task_runner_(std::move(io_task_runner)),
      compositor_task_runner_(std::move(compositor_task_runner)),
      media_task_runner_(std::move(media_task_runner)),
      worker_task_runner_(std::move(worker_task_runner)),
      gpu_factories_(gpu_factories),
      initial_audio_output_device_id_(sink_id),
      volume_(1.0),
      volume_multiplier_(1.0),
      should_play_upon_shown_(false),
      create_bridge_callback_(std::move(create_bridge_callback)),
      stop_force_begin_frames_timer_(
          std::make_unique<TaskRunnerTimer<WebMediaPlayerMS>>(
              main_render_task_runner_,
              this,
              &WebMediaPlayerMS::StopForceBeginFrames)),
      submitter_(std::move(submitter)),
      surface_layer_mode_(surface_layer_mode) {
  DCHECK(client);
  DCHECK(delegate_);
  weak_this_ = weak_factory_.GetWeakPtr();
  delegate_id_ = delegate_->AddObserver(this);
  SendLogMessage(String::Format(
      "%s({delegate_id=%d}, {is_audio_element=%s}, {sink_id=%s})", __func__,
      delegate_id_, client->IsAudioElement() ? "true" : "false",
      sink_id.Utf8().c_str()));

  // TODO(tmathmeyer) WebMediaPlayerImpl gets the URL from the WebLocalFrame.
  // doing that here causes a nullptr deref.
  media_log_->AddEvent<media::MediaLogEvent::kWebMediaPlayerCreated>();
}

WebMediaPlayerMS::~WebMediaPlayerMS() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(
      String::Format("%s() [delegate_id=%d]", __func__, delegate_id_));

  if (!web_stream_.IsNull())
    web_stream_.RemoveObserver(this);

  // Destruct compositor resources in the proper order.
  get_client()->SetCcLayer(nullptr);
  if (video_layer_) {
    DCHECK(surface_layer_mode_ != WebMediaPlayer::SurfaceLayerMode::kAlways);
    video_layer_->StopUsingProvider();
  }

  if (frame_deliverer_)
    io_task_runner_->DeleteSoon(FROM_HERE, frame_deliverer_.release());

  if (compositor_)
    compositor_->StopUsingProvider();

  if (video_frame_provider_)
    video_frame_provider_->Stop();

  if (audio_renderer_)
    audio_renderer_->Stop();

  media_log_->AddEvent<media::MediaLogEvent::kWebMediaPlayerDestroyed>();

  delegate_->PlayerGone(delegate_id_);
  delegate_->RemoveObserver(delegate_id_);
}

void WebMediaPlayerMS::OnAudioRenderErrorCallback() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (watch_time_reporter_)
    watch_time_reporter_->OnError(media::AUDIO_RENDERER_ERROR);

  if (ready_state_ == WebMediaPlayer::kReadyStateHaveNothing) {
    // Any error that occurs before reaching ReadyStateHaveMetadata should
    // be considered a format error.
    SetNetworkState(WebMediaPlayer::kNetworkStateFormatError);
  } else {
    SetNetworkState(WebMediaPlayer::kNetworkStateDecodeError);
  }
}

WebMediaPlayer::LoadTiming WebMediaPlayerMS::Load(
    LoadType load_type,
    const WebMediaPlayerSource& source,
    CorsMode /*cors_mode*/,
    bool is_cache_disabled) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s({load_type=%s})", __func__,
                                LoadTypeToString(load_type)));

  // TODO(acolwell): Change this to DCHECK_EQ(load_type, LoadTypeMediaStream)
  // once Blink-side changes land.
  DCHECK_NE(load_type, kLoadTypeMediaSource);
  web_stream_ = source.GetAsMediaStream();
  if (!web_stream_.IsNull())
    web_stream_.AddObserver(this);

  watch_time_reporter_.reset();

  compositor_ = base::MakeRefCounted<WebMediaPlayerMSCompositor>(
      compositor_task_runner_, io_task_runner_, web_stream_,
      std::move(submitter_), surface_layer_mode_, weak_this_);

  // We can receive a call to RequestVideoFrameCallback() before |compositor_|
  // is created. In that case, we suspend the request, and wait until now to
  // reiniate it.
  if (pending_rvfc_request_) {
    RequestVideoFrameCallback();
    pending_rvfc_request_ = false;
  }

  SetNetworkState(WebMediaPlayer::kNetworkStateLoading);
  SetReadyState(WebMediaPlayer::kReadyStateHaveNothing);
  std::string stream_id =
      web_stream_.IsNull() ? std::string() : web_stream_.Id().Utf8();
  media_log_->AddEvent<media::MediaLogEvent::kLoad>(stream_id);
  SendLogMessage(
      String::Format("%s => (stream_id=%s)", __func__, stream_id.c_str()));

  frame_deliverer_ = std::make_unique<WebMediaPlayerMS::FrameDeliverer>(
      weak_this_,
      CrossThreadBindRepeating(&WebMediaPlayerMSCompositor::EnqueueFrame,
                               compositor_),
      media_task_runner_, worker_task_runner_, gpu_factories_);
  video_frame_provider_ = renderer_factory_->GetVideoRenderer(
      web_stream_,
      ConvertToBaseRepeatingCallback(frame_deliverer_->GetRepaintCallback()),
      io_task_runner_, main_render_task_runner_);

  if (internal_frame_->web_frame()) {
    WebURL url = source.GetAsURL();
    // Report UMA metrics.
    ReportMetrics(load_type, url, *internal_frame_->web_frame(),
                  media_log_.get());
  }

  audio_renderer_ = renderer_factory_->GetAudioRenderer(
      web_stream_, internal_frame_->web_frame(),
      initial_audio_output_device_id_,
      WTF::BindRepeating(&WebMediaPlayerMS::OnAudioRenderErrorCallback,
                         weak_factory_.GetWeakPtr()));

  if (!video_frame_provider_ && !audio_renderer_) {
    SetNetworkState(WebMediaPlayer::kNetworkStateNetworkError);
    SendLogMessage(String::Format(
        "%s => (ERROR: WebMediaPlayer::kNetworkStateNetworkError)", __func__));
    return WebMediaPlayer::LoadTiming::kImmediate;
  }

  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume_);
    audio_renderer_->Start();

    if (!web_stream_.IsNull()) {
      MediaStreamDescriptor& descriptor = *web_stream_;
      auto audio_components = descriptor.AudioComponents();
      // Store the ID of audio track being played in |current_audio_track_id_|.
      DCHECK_GT(audio_components.size(), 0U);
      current_audio_track_id_ = WebString(audio_components[0]->Id());
      SendLogMessage(String::Format("%s => (audio_track_id=%s)", __func__,
                                    current_audio_track_id_.Utf8().c_str()));
      // Report the media track information to blink. Only the first audio track
      // is enabled by default to match blink logic.
      bool is_first_audio_track = true;
      for (auto component : audio_components) {
        client_->AddAudioTrack(
            blink::WebString::FromUTF8(component->Id().Utf8()),
            blink::WebMediaPlayerClient::kAudioTrackKindMain,
            blink::WebString::FromUTF8(component->Source()->GetName().Utf8()),
            /*language=*/"", is_first_audio_track);
        is_first_audio_track = false;
      }
    }
  }

  if (video_frame_provider_) {
    video_frame_provider_->Start();

    if (!web_stream_.IsNull()) {
      MediaStreamDescriptor& descriptor = *web_stream_;
      auto video_components = descriptor.VideoComponents();
      // Store the ID of video track being played in |current_video_track_id_|.
      DCHECK_GT(video_components.size(), 0U);
      current_video_track_id_ = WebString(video_components[0]->Id());
      SendLogMessage(String::Format("%s => (video_track_id=%s)", __func__,
                                    current_video_track_id_.Utf8().c_str()));
      // Report the media track information to blink. Only the first video track
      // is enabled by default to match blink logic.
      bool is_first_video_track = true;
      for (auto component : video_components) {
        client_->AddVideoTrack(
            blink::WebString::FromUTF8(component->Id().Utf8()),
            blink::WebMediaPlayerClient::kVideoTrackKindMain,
            blink::WebString::FromUTF8(component->Source()->GetName().Utf8()),
            /*language=*/"", is_first_video_track);
        is_first_video_track = false;
      }
    }
  }
  // When associated with an <audio> element, we don't want to wait for the
  // first video frame to become available as we do for <video> elements
  // (<audio> elements can also be assigned video tracks).
  // For more details, see https://crbug.com/738379
  if (audio_renderer_ &&
      (client_->IsAudioElement() || !video_frame_provider_)) {
    SendLogMessage(String::Format("%s => (audio only mode)", __func__));
    SetReadyState(WebMediaPlayer::kReadyStateHaveMetadata);
    SetReadyState(WebMediaPlayer::kReadyStateHaveEnoughData);
    MaybeCreateWatchTimeReporter();
  }

  client_->DidMediaMetadataChange(HasAudio(), HasVideo(),
                                  media::MediaContentType::OneShot);
  delegate_->DidMediaMetadataChange(delegate_id_, HasAudio(), HasVideo(),
                                    media::MediaContentType::OneShot);

  return WebMediaPlayer::LoadTiming::kImmediate;
}

void WebMediaPlayerMS::OnWebLayerUpdated() {}

void WebMediaPlayerMS::RegisterContentsLayer(cc::Layer* layer) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(bridge_);

  bridge_->SetContentsOpaque(opaque_);
  client_->SetCcLayer(layer);
}

void WebMediaPlayerMS::UnregisterContentsLayer(cc::Layer* layer) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // |client_| will unregister its cc::Layer if given a nullptr.
  client_->SetCcLayer(nullptr);
}

void WebMediaPlayerMS::OnSurfaceIdUpdated(viz::SurfaceId surface_id) {
  // TODO(726619): Handle the behavior when Picture-in-Picture mode is
  // disabled.
  // The viz::SurfaceId may be updated when the video begins playback or when
  // the size of the video changes.
  if (client_)
    client_->OnPictureInPictureStateChange();
}

void WebMediaPlayerMS::TrackAdded(const WebString& track_id) {
  SendLogMessage(
      String::Format("%s({track_id=%s})", __func__, track_id.Utf8().c_str()));
  Reload();
}

void WebMediaPlayerMS::TrackRemoved(const WebString& track_id) {
  SendLogMessage(
      String::Format("%s({track_id=%s})", __func__, track_id.Utf8().c_str()));
  Reload();
}

void WebMediaPlayerMS::ActiveStateChanged(bool is_active) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s({is_active=%s})", __func__,
                                is_active ? "true" : "false"));
  // The case when the stream becomes active is handled by TrackAdded().
  if (is_active)
    return;

  // This makes the media element eligible to be garbage collected. Otherwise,
  // the element will be considered active and will never be garbage
  // collected.
  SetNetworkState(kNetworkStateIdle);

  // Stop the audio renderer to free up resources that are not required for an
  // inactive stream. This is useful if the media element is not garbage
  // collected.
  // Note that the video renderer should not be stopped because the ended video
  // track is expected to produce a black frame after becoming inactive.
  if (audio_renderer_)
    audio_renderer_->Stop();
}

int WebMediaPlayerMS::GetDelegateId() {
  return delegate_id_;
}

absl::optional<viz::SurfaceId> WebMediaPlayerMS::GetSurfaceId() {
  if (bridge_)
    return bridge_->GetSurfaceId();
  return absl::nullopt;
}

base::WeakPtr<WebMediaPlayer> WebMediaPlayerMS::AsWeakPtr() {
  return weak_this_;
}

void WebMediaPlayerMS::Reload() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (web_stream_.IsNull())
    return;

  ReloadVideo();
  ReloadAudio();
}

void WebMediaPlayerMS::ReloadVideo() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!web_stream_.IsNull());
  MediaStreamDescriptor& descriptor = *web_stream_;
  auto video_components = descriptor.VideoComponents();

  RendererReloadAction renderer_action = RendererReloadAction::KEEP_RENDERER;
  if (video_components.IsEmpty()) {
    if (video_frame_provider_)
      renderer_action = RendererReloadAction::REMOVE_RENDERER;
    current_video_track_id_ = WebString();
  } else if (WebString(video_components[0]->Id()) != current_video_track_id_ &&
             IsPlayableTrack(video_components[0])) {
    renderer_action = RendererReloadAction::NEW_RENDERER;
    current_video_track_id_ = video_components[0]->Id();
  }

  switch (renderer_action) {
    case RendererReloadAction::NEW_RENDERER:
      if (video_frame_provider_)
        video_frame_provider_->Stop();

      SetNetworkState(kNetworkStateLoading);
      video_frame_provider_ = renderer_factory_->GetVideoRenderer(
          web_stream_,
          ConvertToBaseRepeatingCallback(
              frame_deliverer_->GetRepaintCallback()),
          io_task_runner_, main_render_task_runner_);
      DCHECK(video_frame_provider_);
      video_frame_provider_->Start();
      break;

    case RendererReloadAction::REMOVE_RENDERER:
      video_frame_provider_->Stop();
      video_frame_provider_ = nullptr;
      break;

    default:
      return;
  }

  DCHECK_NE(renderer_action, RendererReloadAction::KEEP_RENDERER);
  if (!paused_) {
    client_->DidPlayerSizeChange(NaturalSize());
    if (watch_time_reporter_)
      UpdateWatchTimeReporterSecondaryProperties();
  }

  // TODO(perkj, magjed): We use OneShot focus type here so that it takes
  // audio focus once it starts, and then will not respond to further audio
  // focus changes. See https://crbug.com/596516 for more details.
  client_->DidMediaMetadataChange(HasAudio(), HasVideo(),
                                  media::MediaContentType::OneShot);
  delegate_->DidMediaMetadataChange(delegate_id_, HasAudio(), HasVideo(),
                                    media::MediaContentType::OneShot);
}

void WebMediaPlayerMS::ReloadAudio() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!web_stream_.IsNull());
  if (!internal_frame_->web_frame())
    return;
  SendLogMessage(String::Format("%s()", __func__));

  MediaStreamDescriptor& descriptor = *web_stream_;
  auto audio_components = descriptor.AudioComponents();

  RendererReloadAction renderer_action = RendererReloadAction::KEEP_RENDERER;
  if (audio_components.IsEmpty()) {
    if (audio_renderer_)
      renderer_action = RendererReloadAction::REMOVE_RENDERER;
    current_audio_track_id_ = WebString();
  } else if (WebString(audio_components[0]->Id()) != current_audio_track_id_ &&
             IsPlayableTrack(audio_components[0])) {
    renderer_action = RendererReloadAction::NEW_RENDERER;
    current_audio_track_id_ = audio_components[0]->Id();
  }

  switch (renderer_action) {
    case RendererReloadAction::NEW_RENDERER:
      if (audio_renderer_)
        audio_renderer_->Stop();

      SetNetworkState(WebMediaPlayer::kNetworkStateLoading);
      audio_renderer_ = renderer_factory_->GetAudioRenderer(
          web_stream_, internal_frame_->web_frame(),
          initial_audio_output_device_id_,
          WTF::BindRepeating(&WebMediaPlayerMS::OnAudioRenderErrorCallback,
                             weak_factory_.GetWeakPtr()));

      // |audio_renderer_| can be null in tests.
      if (!audio_renderer_)
        break;

      audio_renderer_->SetVolume(volume_);
      audio_renderer_->Start();
      audio_renderer_->Play();
      break;

    case RendererReloadAction::REMOVE_RENDERER:
      audio_renderer_->Stop();
      audio_renderer_ = nullptr;
      break;

    default:
      break;
  }

  // TODO(perkj, magjed): We use OneShot focus type here so that it takes
  // audio focus once it starts, and then will not respond to further audio
  // focus changes. See https://crbug.com/596516 for more details.
  client_->DidMediaMetadataChange(HasAudio(), HasVideo(),
                                  media::MediaContentType::OneShot);
  delegate_->DidMediaMetadataChange(delegate_id_, HasAudio(), HasVideo(),
                                    media::MediaContentType::OneShot);
}

void WebMediaPlayerMS::Play() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s()", __func__));

  media_log_->AddEvent<media::MediaLogEvent::kPlay>();
  if (!paused_)
    return;

  if (video_frame_provider_)
    video_frame_provider_->Resume();

  compositor_->StartRendering();

  if (audio_renderer_)
    audio_renderer_->Play();

  if (watch_time_reporter_) {
    watch_time_reporter_->SetAutoplayInitiated(client_->WasAutoplayInitiated());
    watch_time_reporter_->OnPlaying();
  }

  if (HasVideo()) {
    client_->DidPlayerSizeChange(NaturalSize());
    if (watch_time_reporter_)
      UpdateWatchTimeReporterSecondaryProperties();
  }

  client_->DidPlayerStartPlaying();
  delegate_->DidPlay(delegate_id_);

  delegate_->SetIdle(delegate_id_, false);
  paused_ = false;
}

void WebMediaPlayerMS::Pause() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s()", __func__));

  should_play_upon_shown_ = false;
  media_log_->AddEvent<media::MediaLogEvent::kPause>();
  if (paused_)
    return;

  if (video_frame_provider_)
    video_frame_provider_->Pause();

  compositor_->StopRendering();
  compositor_->ReplaceCurrentFrameWithACopy();

  if (audio_renderer_)
    audio_renderer_->Pause();

  client_->DidPlayerPaused(/* stream_ended = */ false);
  if (watch_time_reporter_)
    watch_time_reporter_->OnPaused();

  delegate_->DidPause(delegate_id_, /* reached_end_of_stream = */ false);
  delegate_->SetIdle(delegate_id_, true);

  paused_ = true;
}

void WebMediaPlayerMS::Seek(double seconds) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

void WebMediaPlayerMS::SetRate(double rate) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

void WebMediaPlayerMS::SetVolume(double volume) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s({volume=%.2f})", __func__, volume));
  volume_ = volume;
  if (audio_renderer_.get())
    audio_renderer_->SetVolume(volume_ * volume_multiplier_);
  if (watch_time_reporter_)
    watch_time_reporter_->OnVolumeChange(volume);
  client_->DidPlayerMutedStatusChange(volume == 0.0);
}

void WebMediaPlayerMS::SetLatencyHint(double seconds) {
  // WebRTC latency has separate latency APIs, focused more on network jitter
  // and implemented inside the WebRTC stack.
  // https://webrtc.org/experiments/rtp-hdrext/playout-delay/
  // https://henbos.github.io/webrtc-timing/#dom-rtcrtpreceiver-playoutdelayhint
}

void WebMediaPlayerMS::SetPreservesPitch(bool preserves_pitch) {
  // Since WebMediaPlayerMS::SetRate() is a no-op, it doesn't make sense to
  // handle pitch preservation flags. The playback rate should always be 1.0,
  // and thus there should be no pitch-shifting.
}

void WebMediaPlayerMS::SetAutoplayInitiated(bool autoplay_initiated) {}

void WebMediaPlayerMS::OnRequestPictureInPicture() {
  if (!bridge_)
    ActivateSurfaceLayerForVideo();

  DCHECK(bridge_);
  DCHECK(bridge_->GetSurfaceId().is_valid());
}

bool WebMediaPlayerMS::SetSinkId(
    const WebString& sink_id,
    WebSetSinkIdCompleteCallback completion_callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(
      String::Format("%s({sink_id=%s})", __func__, sink_id.Utf8().c_str()));

  media::OutputDeviceStatusCB callback =
      ConvertToOutputDeviceStatusCB(std::move(completion_callback));

  if (!audio_renderer_) {
    SendLogMessage(String::Format(
        "%s => (WARNING: failed to instantiate audio renderer)", __func__));
    std::move(callback).Run(media::OUTPUT_DEVICE_STATUS_ERROR_INTERNAL);
    SendLogMessage(String::Format(
        "%s => (ERROR: OUTPUT_DEVICE_STATUS_ERROR_INTERNAL)", __func__));
    return false;
  }

  auto sink_id_utf8 = sink_id.Utf8();
#if defined(USE_WEBOS_AUDIO)
  if (!initial_audio_output_device_id_.IsEmpty() &&
      sink_id_utf8 == media::AudioDeviceDescription::kDefaultDeviceId) {
    return true;
  }
#endif
  audio_renderer_->SwitchOutputDevice(sink_id_utf8, std::move(callback));
  return true;
}

void WebMediaPlayerMS::SetPreload(WebMediaPlayer::Preload preload) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

bool WebMediaPlayerMS::HasVideo() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return !!video_frame_provider_;
}

bool WebMediaPlayerMS::HasAudio() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return !!audio_renderer_;
}

gfx::Size WebMediaPlayerMS::NaturalSize() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (!video_frame_provider_)
    return gfx::Size();

  const gfx::Size& current_size = compositor_->GetCurrentSize();
  if (video_transformation_.rotation == media::VIDEO_ROTATION_90 ||
      video_transformation_.rotation == media::VIDEO_ROTATION_270) {
    return gfx::Size(current_size.height(), current_size.width());
  }
  return current_size;
}

gfx::Size WebMediaPlayerMS::VisibleSize() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  scoped_refptr<media::VideoFrame> video_frame = compositor_->GetCurrentFrame();
  if (!video_frame)
    return gfx::Size();

  const gfx::Rect& visible_rect = video_frame->visible_rect();
  if (video_transformation_.rotation == media::VIDEO_ROTATION_90 ||
      video_transformation_.rotation == media::VIDEO_ROTATION_270) {
    return gfx::Size(visible_rect.height(), visible_rect.width());
  }
  return visible_rect.size();
}

bool WebMediaPlayerMS::Paused() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return paused_;
}

bool WebMediaPlayerMS::Seeking() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return false;
}

double WebMediaPlayerMS::Duration() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return std::numeric_limits<double>::infinity();
}

double WebMediaPlayerMS::CurrentTime() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  const base::TimeDelta current_time = compositor_->GetCurrentTime();
  if (current_time.ToInternalValue() != 0)
    return current_time.InSecondsF();
  else if (audio_renderer_.get())
    return audio_renderer_->GetCurrentRenderTime().InSecondsF();
  return 0.0;
}

bool WebMediaPlayerMS::IsEnded() const {
  // MediaStreams never end.
  return false;
}

WebMediaPlayer::NetworkState WebMediaPlayerMS::GetNetworkState() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return network_state_;
}

WebMediaPlayer::ReadyState WebMediaPlayerMS::GetReadyState() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return ready_state_;
}

WebMediaPlayer::SurfaceLayerMode WebMediaPlayerMS::GetVideoSurfaceLayerMode()
    const {
  return surface_layer_mode_;
}

WebString WebMediaPlayerMS::GetErrorMessage() const {
  return WebString::FromUTF8(media_log_->GetErrorMessage());
}

WebTimeRanges WebMediaPlayerMS::Buffered() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return buffered_;
}

WebTimeRanges WebMediaPlayerMS::Seekable() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return WebTimeRanges();
}

bool WebMediaPlayerMS::DidLoadingProgress() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return true;
}

void WebMediaPlayerMS::Paint(cc::PaintCanvas* canvas,
                             const gfx::Rect& rect,
                             cc::PaintFlags& flags) {
  DVLOG(3) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  const scoped_refptr<media::VideoFrame> frame = compositor_->GetCurrentFrame();

  scoped_refptr<viz::RasterContextProvider> provider;
  if (frame && frame->HasTextures()) {
    provider = Platform::Current()->SharedMainThreadContextProvider();
    // GPU Process crashed.
    if (!provider)
      return;
  }
  const gfx::RectF dest_rect(rect);
  video_renderer_.Paint(frame, canvas, dest_rect, flags, video_transformation_,
                        provider.get());
}

scoped_refptr<media::VideoFrame> WebMediaPlayerMS::GetCurrentFrame() {
  DVLOG(3) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return compositor_->GetCurrentFrame();
}

bool WebMediaPlayerMS::WouldTaintOrigin() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return false;
}

double WebMediaPlayerMS::MediaTimeForTimeValue(double timeValue) const {
  return base::TimeDelta::FromSecondsD(timeValue).InSecondsF();
}

unsigned WebMediaPlayerMS::DecodedFrameCount() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return static_cast<unsigned>(compositor_->total_frame_count());
}

unsigned WebMediaPlayerMS::DroppedFrameCount() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return static_cast<unsigned>(compositor_->dropped_frame_count());
}

uint64_t WebMediaPlayerMS::AudioDecodedByteCount() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  NOTIMPLEMENTED();
  return 0;
}

uint64_t WebMediaPlayerMS::VideoDecodedByteCount() const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  NOTIMPLEMENTED();
  return 0;
}

bool WebMediaPlayerMS::HasAvailableVideoFrame() const {
  return has_first_frame_;
}

void WebMediaPlayerMS::OnFrameHidden() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (watch_time_reporter_)
    watch_time_reporter_->OnHidden();

  // This method is called when the RenderFrame is sent to background or
  // suspended. During undoable tab closures OnHidden() may be called back to
  // back, so we can't rely on |render_frame_suspended_| being false here.
  if (frame_deliverer_) {
    PostCrossThreadTask(
        *io_task_runner_, FROM_HERE,
        CrossThreadBindOnce(&FrameDeliverer::SetRenderFrameSuspended,
                            CrossThreadUnretained(frame_deliverer_.get()),
                            true));
  }

  PostCrossThreadTask(
      *compositor_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&WebMediaPlayerMSCompositor::SetIsPageVisible,
                          CrossThreadUnretained(compositor_.get()), false));

// On Android, substitute the displayed VideoFrame with a copy to avoid holding
// onto it unnecessarily.
#if defined(OS_ANDROID)
  if (!paused_)
    compositor_->ReplaceCurrentFrameWithACopy();
#endif  // defined(OS_ANDROID)
}

void WebMediaPlayerMS::SuspendForFrameClosed() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

// On Android, pause the video completely for this time period.
#if defined(OS_ANDROID)
  if (!paused_) {
    Pause();
    should_play_upon_shown_ = true;
  }

  delegate_->PlayerGone(delegate_id_);
#endif  // defined(OS_ANDROID)

  if (frame_deliverer_) {
    PostCrossThreadTask(
        *io_task_runner_, FROM_HERE,
        CrossThreadBindOnce(&FrameDeliverer::SetRenderFrameSuspended,
                            CrossThreadUnretained(frame_deliverer_.get()),
                            true));
  }
}

void WebMediaPlayerMS::OnFrameShown() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (watch_time_reporter_)
    watch_time_reporter_->OnShown();

  if (frame_deliverer_) {
    PostCrossThreadTask(
        *io_task_runner_, FROM_HERE,
        CrossThreadBindOnce(&FrameDeliverer::SetRenderFrameSuspended,
                            CrossThreadUnretained(frame_deliverer_.get()),
                            false));
  }

  PostCrossThreadTask(
      *compositor_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&WebMediaPlayerMSCompositor::SetIsPageVisible,
                          CrossThreadUnretained(compositor_.get()), true));

// On Android, resume playback on visibility. play() clears
// |should_play_upon_shown_|.
#if defined(OS_ANDROID)
  if (should_play_upon_shown_)
    Play();
#endif  // defined(OS_ANDROID)
}

void WebMediaPlayerMS::OnIdleTimeout() {}

void WebMediaPlayerMS::SetVolumeMultiplier(double multiplier) {
  // TODO(perkj, magjed): See TODO in OnPlay().
}

#if defined(USE_NEVA_WEBRTC)
void WebMediaPlayerMS::EnqueueFrame(scoped_refptr<media::VideoFrame> frame) {
  if (frame_deliverer_) {
    int original_frame_id = frame->unique_id();
    PostCrossThreadTask(
        *io_task_runner_, FROM_HERE,
        CrossThreadBindOnce(&FrameDeliverer::EnqueueFrame,
                            CrossThreadUnretained(frame_deliverer_.get()),
                            original_frame_id, std::move(frame)));
  }
}
#endif

void WebMediaPlayerMS::ActivateSurfaceLayerForVideo() {
  // Note that we might or might not already be in VideoLayer mode.
  DCHECK(!bridge_);

  // If we're in VideoLayer mode, then get rid of the layer.
  if (video_layer_) {
    client_->SetCcLayer(nullptr);
    video_layer_ = nullptr;
  }

  bridge_ = std::move(create_bridge_callback_)
                .Run(this, compositor_->GetUpdateSubmissionStateCallback());
  bridge_->CreateSurfaceLayer();
  bridge_->SetContentsOpaque(opaque_);

  PostCrossThreadTask(
      *compositor_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&WebMediaPlayerMSCompositor::EnableSubmission,
                          compositor_, bridge_->GetSurfaceId(),
                          video_transformation_, IsInPictureInPicture()));

  // If the element is already in Picture-in-Picture mode, it means that it
  // was set in this mode prior to this load, with a different
  // WebMediaPlayerImpl. The new player needs to send its id, size and
  // surface id to the browser process to make sure the states are properly
  // updated.
  // TODO(872056): the surface should be activated but for some reason, it
  // does not. It is possible that this will no longer be needed after 872056
  // is fixed.
  if (client_->GetDisplayType() == DisplayType::kPictureInPicture) {
    OnSurfaceIdUpdated(bridge_->GetSurfaceId());
  }
}

void WebMediaPlayerMS::OnFirstFrameReceived(
    media::VideoTransformation video_transform,
    bool is_opaque) {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  has_first_frame_ = true;
  OnTransformChanged(video_transform);
  OnOpacityChanged(is_opaque);

  if (surface_layer_mode_ == WebMediaPlayer::SurfaceLayerMode::kAlways)
    ActivateSurfaceLayerForVideo();

  SetReadyState(WebMediaPlayer::kReadyStateHaveMetadata);
  SetReadyState(WebMediaPlayer::kReadyStateHaveEnoughData);
  TriggerResize();
  ResetCanvasCache();
  MaybeCreateWatchTimeReporter();
}

void WebMediaPlayerMS::OnOpacityChanged(bool is_opaque) {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  opaque_ = is_opaque;
  if (!bridge_) {
    // Opacity can be changed during the session without resetting
    // |video_layer_|.
    video_layer_->SetContentsOpaque(opaque_);
  } else {
    DCHECK(bridge_);
    bridge_->SetContentsOpaque(opaque_);
  }
}

void WebMediaPlayerMS::OnTransformChanged(
    media::VideoTransformation video_transform) {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  video_transformation_ = video_transform;

  if (!bridge_) {
    // Keep the old |video_layer_| alive until SetCcLayer() is called with a new
    // pointer, as it may use the pointer from the last call.
    auto new_video_layer =
        cc::VideoLayer::Create(compositor_.get(), video_transformation_);
    get_client()->SetCcLayer(new_video_layer.get());
    video_layer_ = std::move(new_video_layer);
  }
}

bool WebMediaPlayerMS::IsInPictureInPicture() const {
  DCHECK(client_);
  return (!client_->IsInAutoPIP() &&
          client_->GetDisplayType() == DisplayType::kPictureInPicture);
}

void WebMediaPlayerMS::RepaintInternal() {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  get_client()->Repaint();
}

void WebMediaPlayerMS::SetNetworkState(WebMediaPlayer::NetworkState state) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s => (state=%s)", __func__,
                                NetworkStateToString(network_state_)));
  network_state_ = state;
  // Always notify to ensure client has the latest value.
  get_client()->NetworkStateChanged();
}

void WebMediaPlayerMS::SetReadyState(WebMediaPlayer::ReadyState state) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  SendLogMessage(String::Format("%s => (state=%s)", __func__,
                                ReadyStateToString(ready_state_)));
  ready_state_ = state;
  // Always notify to ensure client has the latest value.
  get_client()->ReadyStateChanged();
}

media::PaintCanvasVideoRenderer*
WebMediaPlayerMS::GetPaintCanvasVideoRenderer() {
  return &video_renderer_;
}

void WebMediaPlayerMS::ResetCanvasCache() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  video_renderer_.ResetCache();
}

void WebMediaPlayerMS::TriggerResize() {
  if (HasVideo())
    get_client()->SizeChanged();

  client_->DidPlayerSizeChange(NaturalSize());
  if (watch_time_reporter_)
    UpdateWatchTimeReporterSecondaryProperties();
}

void WebMediaPlayerMS::SetGpuMemoryBufferVideoForTesting(
    media::GpuMemoryBufferVideoFramePool* gpu_memory_buffer_pool) {
  CHECK(frame_deliverer_);
  frame_deliverer_->gpu_memory_buffer_pool_.reset(gpu_memory_buffer_pool);
}

void WebMediaPlayerMS::SetMediaStreamRendererFactoryForTesting(
    std::unique_ptr<MediaStreamRendererFactory> renderer_factory) {
  renderer_factory_ = std::move(renderer_factory);
}

void WebMediaPlayerMS::OnDisplayTypeChanged(DisplayType display_type) {
  if (!bridge_)
    return;

  PostCrossThreadTask(
      *compositor_task_runner_, FROM_HERE,
      CrossThreadBindOnce(&WebMediaPlayerMSCompositor::SetForceSubmit,
                          CrossThreadUnretained(compositor_.get()),
                          display_type == DisplayType::kPictureInPicture));

  if (!watch_time_reporter_)
    return;

  switch (display_type) {
    case DisplayType::kInline:
      watch_time_reporter_->OnDisplayTypeInline();
      break;
    case DisplayType::kFullscreen:
      watch_time_reporter_->OnDisplayTypeFullscreen();
      break;
    case DisplayType::kPictureInPicture:
      watch_time_reporter_->OnDisplayTypePictureInPicture();
  }
}

void WebMediaPlayerMS::OnNewFramePresentedCallback() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  client_->OnRequestVideoFrameCallback();
}

void WebMediaPlayerMS::SendLogMessage(const WTF::String& message) const {
  WebRtcLogMessage("WMPMS::" + message.Utf8() +
                   String::Format(" [delegate_id=%d]", delegate_id_).Utf8());
}

std::unique_ptr<WebMediaPlayer::VideoFramePresentationMetadata>
WebMediaPlayerMS::GetVideoFramePresentationMetadata() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(compositor_);

  return compositor_->GetLastPresentedFrameMetadata();
}

void WebMediaPlayerMS::RequestVideoFrameCallback() {
  DCHECK(RuntimeEnabledFeatures::RequestVideoFrameCallbackEnabled());
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (!compositor_) {
    // Reissue the request after |compositor_| is created, in Load().
    pending_rvfc_request_ = true;
    return;
  }

  compositor_->SetOnFramePresentedCallback(
      media::BindToCurrentLoop(base::BindOnce(
          &WebMediaPlayerMS::OnNewFramePresentedCallback, weak_this_)));

  compositor_->SetForceBeginFrames(true);

  stop_force_begin_frames_timer_->StartOneShot(kForceBeginFramesTimeout,
                                               FROM_HERE);
}

void WebMediaPlayerMS::StopForceBeginFrames(TimerBase* timer) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  compositor_->SetForceBeginFrames(false);
}

void WebMediaPlayerMS::MaybeCreateWatchTimeReporter() {
  if (!internal_frame_->web_frame())
    return;

  if (!HasAudio() && !HasVideo())
    return;

  absl::optional<media::mojom::MediaStreamType> media_stream_type =
      GetMediaStreamType();
  if (!media_stream_type)
    return;

  if (watch_time_reporter_)
    return;

  if (compositor_) {
    compositor_initial_time_ = compositor_->GetCurrentTime();
    compositor_last_time_ = compositor_initial_time_;
  }
  if (audio_renderer_) {
    audio_initial_time_ = audio_renderer_->GetCurrentRenderTime();
    audio_last_time_ = audio_initial_time_;
  }

  mojo::Remote<media::mojom::MediaMetricsProvider> media_metrics_provider;
  auto* execution_context =
      internal_frame_->frame()->DomWindow()->GetExecutionContext();
  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      execution_context->GetTaskRunner(TaskType::kMediaElementEvent);
  execution_context->GetBrowserInterfaceBroker().GetInterface(
      media_metrics_provider.BindNewPipeAndPassReceiver(task_runner));
  media_metrics_provider->Initialize(false /* is_mse */,
                                     media::mojom::MediaURLScheme::kMissing,
                                     *media_stream_type);

  // Create the watch time reporter and synchronize its initial state.
  // WTF::Unretained() is safe because WebMediaPlayerMS owns the
  // |watch_time_reporter_|, and therefore outlives it.
  watch_time_reporter_ = std::make_unique<WatchTimeReporter>(
      media::mojom::PlaybackProperties::New(
          HasAudio(), HasVideo(), false /*is_background*/, false /*is_muted*/,
          false /*is_mse*/, false /*is_eme*/,
          false /*is_embedded_media_experience*/, *media_stream_type),
      NaturalSize(),
      WTF::BindRepeating(&WebMediaPlayerMS::GetCurrentTimeInterval,
                         WTF::Unretained(this)),
      WTF::BindRepeating(&WebMediaPlayerMS::GetPipelineStatistics,
                         WTF::Unretained(this)),
      media_metrics_provider.get(),
      internal_frame_->web_frame()->GetTaskRunner(
          blink::TaskType::kInternalMedia));

  watch_time_reporter_->OnVolumeChange(volume_);

  if (delegate_->IsFrameHidden())
    watch_time_reporter_->OnHidden();
  else
    watch_time_reporter_->OnShown();

  if (client_) {
    if (client_->HasNativeControls())
      watch_time_reporter_->OnNativeControlsEnabled();
    else
      watch_time_reporter_->OnNativeControlsDisabled();

    switch (client_->GetDisplayType()) {
      case DisplayType::kInline:
        watch_time_reporter_->OnDisplayTypeInline();
        break;
      case DisplayType::kFullscreen:
        watch_time_reporter_->OnDisplayTypeFullscreen();
        break;
      case DisplayType::kPictureInPicture:
        watch_time_reporter_->OnDisplayTypePictureInPicture();
        break;
    }
  }

  UpdateWatchTimeReporterSecondaryProperties();

  // If the WatchTimeReporter was recreated in the middle of playback, we want
  // to resume playback here too since we won't get another play() call.
  if (!paused_)
    watch_time_reporter_->OnPlaying();
}

void WebMediaPlayerMS::UpdateWatchTimeReporterSecondaryProperties() {
  // Set only the natural size and use default values for the other secondary
  // properties. MediaStreams generally operate with raw data, where there is no
  // codec information. For the MediaStreams where coded information is
  // available, the coded information is currently not accessible to the media
  // player.
  // TODO(https://crbug.com/1147813) Report codec information once accessible.
  watch_time_reporter_->UpdateSecondaryProperties(
      media::mojom::SecondaryPlaybackProperties::New(
          media::kUnknownAudioCodec, media::kUnknownVideoCodec,
          media::AudioCodecProfile::kUnknown,
          media::VideoCodecProfile::VIDEO_CODEC_PROFILE_UNKNOWN,
          media::AudioDecoderType::kUnknown, media::VideoDecoderType::kUnknown,
          media::EncryptionScheme::kUnencrypted,
          media::EncryptionScheme::kUnencrypted, NaturalSize()));
}

base::TimeDelta WebMediaPlayerMS::GetCurrentTimeInterval() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (compositor_) {
    compositor_last_time_ =
        std::max(compositor_last_time_, compositor_->GetCurrentTime());
  }
  if (audio_renderer_) {
    audio_last_time_ =
        std::max(audio_last_time_, audio_renderer_->GetCurrentRenderTime());
  }

  base::TimeDelta compositor_interval =
      compositor_last_time_ - compositor_initial_time_;
  base::TimeDelta audio_interval = audio_last_time_ - audio_initial_time_;
  return std::max(compositor_interval, audio_interval);
}

media::PipelineStatistics WebMediaPlayerMS::GetPipelineStatistics() {
  media::PipelineStatistics stats;
  stats.video_frames_decoded = DecodedFrameCount();
  stats.video_frames_dropped = DroppedFrameCount();
  return stats;
}

absl::optional<media::mojom::MediaStreamType>
WebMediaPlayerMS::GetMediaStreamType() {
  if (web_stream_.IsNull())
    return absl::nullopt;

  // If either the first video or audio source is remote, the media stream is
  // of remote source.
  MediaStreamDescriptor& descriptor = *web_stream_;
  MediaStreamSource* media_source = nullptr;
  if (HasVideo()) {
    auto video_components = descriptor.VideoComponents();
    DCHECK_GT(video_components.size(), 0U);
    media_source = video_components[0]->Source();
  } else if (HasAudio()) {
    auto audio_components = descriptor.AudioComponents();
    DCHECK_GT(audio_components.size(), 0U);
    media_source = audio_components[0]->Source();
  }
  if (!media_source)
    return absl::nullopt;
  if (media_source->Remote())
    return media::mojom::MediaStreamType::kRemote;

  auto* platform_source = media_source->GetPlatformSource();
  if (!platform_source)
    return absl::nullopt;
  switch (platform_source->device().type) {
    case mojom::blink::MediaStreamType::NO_SERVICE:
      // Element capture uses the default NO_SERVICE value since it does not set
      // a device.
      return media::mojom::MediaStreamType::kLocalElementCapture;
    case mojom::blink::MediaStreamType::DEVICE_AUDIO_CAPTURE:
    case mojom::blink::MediaStreamType::DEVICE_VIDEO_CAPTURE:
      return media::mojom::MediaStreamType::kLocalDeviceCapture;
    case mojom::blink::MediaStreamType::GUM_TAB_AUDIO_CAPTURE:
    case mojom::blink::MediaStreamType::GUM_TAB_VIDEO_CAPTURE:
      return media::mojom::MediaStreamType::kLocalTabCapture;
    case mojom::blink::MediaStreamType::GUM_DESKTOP_AUDIO_CAPTURE:
    case mojom::blink::MediaStreamType::GUM_DESKTOP_VIDEO_CAPTURE:
      return media::mojom::MediaStreamType::kLocalDesktopCapture;
    case mojom::blink::MediaStreamType::DISPLAY_AUDIO_CAPTURE:
    case mojom::blink::MediaStreamType::DISPLAY_VIDEO_CAPTURE:
    case mojom::blink::MediaStreamType::DISPLAY_VIDEO_CAPTURE_THIS_TAB:
      return media::mojom::MediaStreamType::kLocalDisplayCapture;
    case mojom::blink::MediaStreamType::NUM_MEDIA_TYPES:
      NOTREACHED();
      return absl::nullopt;
  }

  return absl::nullopt;
}

}  // namespace blink
