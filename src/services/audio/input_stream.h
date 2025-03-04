// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_AUDIO_INPUT_STREAM_H_
#define SERVICES_AUDIO_INPUT_STREAM_H_

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/sync_socket.h"
#include "base/unguessable_token.h"
#include "media/mojo/mojom/audio_data_pipe.mojom.h"
#include "media/mojo/mojom/audio_input_stream.mojom.h"
#include "media/mojo/mojom/audio_logging.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/shared_remote.h"
#include "services/audio/input_controller.h"

namespace media {

class AudioManager;
class AudioParameters;

}  // namespace media

namespace audio {

class InputStreamActivityMonitor;
class InputSyncWriter;
class UserInputMonitor;

class InputStream final : public media::mojom::AudioInputStream,
                          public InputController::EventHandler {
 public:
  using CreatedCallback =
      base::OnceCallback<void(media::mojom::ReadOnlyAudioDataPipePtr,
                              bool,
                              const absl::optional<base::UnguessableToken>&)>;
  using DeleteCallback = base::OnceCallback<void(InputStream*)>;

  InputStream(
      CreatedCallback created_callback,
      DeleteCallback delete_callback,
      mojo::PendingReceiver<media::mojom::AudioInputStream> receiver,
      mojo::PendingRemote<media::mojom::AudioInputStreamClient> client,
      mojo::PendingRemote<media::mojom::AudioInputStreamObserver> observer,
      mojo::PendingRemote<media::mojom::AudioLog> log,
      media::AudioManager* manager,
      std::unique_ptr<UserInputMonitor> user_input_monitor,
      InputStreamActivityMonitor* activity_monitor,
      const std::string& device_id,
      const media::AudioParameters& params,
      uint32_t shared_memory_count,
      bool enable_agc);
  ~InputStream() override;

  const base::UnguessableToken& id() const { return id_; }
  void SetOutputDeviceForAec(const std::string& output_device_id);

  // media::mojom::AudioInputStream implementation.
  void Record() override;
#if defined(USE_NEVA_SUSPEND_MEDIA_CAPTURE)
  void Pause() override;
  void Resume() override;
#endif
  void SetVolume(double volume) override;

  // InputController::EventHandler implementation.
  void OnCreated(bool initially_muted) override;
  void OnError(InputController::ErrorCode error_code) override;
  void OnLog(base::StringPiece) override;
  void OnMuted(bool is_muted) override;

 private:
  void OnStreamError(
      absl::optional<media::mojom::AudioInputStreamObserver::DisconnectReason>
          reason_to_report);
  void OnStreamPlatformError();
  void CallDeleter();
  void SendLogMessage(const char* format, ...) PRINTF_FORMAT(2, 3);

#if defined(USE_WEBOS_AUDIO)
  void CreateStreamAsync(media::AudioManager* audio_manager,
                         InputStreamActivityMonitor* activity_monitor,
                         const media::AudioParameters& params,
                         const std::string& device_id,
                         bool enable_agc);
#endif

  const base::UnguessableToken id_;

  mojo::Receiver<media::mojom::AudioInputStream> receiver_;
  mojo::Remote<media::mojom::AudioInputStreamClient> client_;
  mojo::Remote<media::mojom::AudioInputStreamObserver> observer_;
  const mojo::SharedRemote<media::mojom::AudioLog> log_;

  // Notify stream client on creation.
  CreatedCallback created_callback_;

  // Notify stream factory (audio service) on destruction.
  DeleteCallback delete_callback_;

  base::CancelableSyncSocket foreign_socket_;
  const std::unique_ptr<InputSyncWriter> writer_;
  std::unique_ptr<InputController> controller_;
  const std::unique_ptr<UserInputMonitor> user_input_monitor_;

  SEQUENCE_CHECKER(owning_sequence_);

  base::WeakPtrFactory<InputStream> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(InputStream);
};

}  // namespace audio

#endif  // SERVICES_AUDIO_INPUT_STREAM_H_
