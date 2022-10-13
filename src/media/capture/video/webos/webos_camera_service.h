// Copyright 2022 LG Electronics, Inc.
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

#ifndef MEDIA_CAPTURE_VIDEO_WEBOS_WEBOS_CAMERA_SERVICE_H_
#define MEDIA_CAPTURE_VIDEO_WEBOS_WEBOS_CAMERA_SERVICE_H_

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/neva/webos/luna_service_client.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/platform_thread.h"
#include "base/values.h"
#include "media/capture/capture_export.h"
#include "media/capture/video/webos/webos_camera_constants.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace camera {
class CameraBuffer;
}

namespace media {

class CAPTURE_EXPORT WebOSCameraService
    : public base::RefCountedThreadSafe<WebOSCameraService> {
 public:
  using ResponseCB = base::RepeatingCallback<void(const std::string&)>;

  explicit WebOSCameraService();
  virtual ~WebOSCameraService();

  int Open(base::PlatformThreadId pid,
           const std::string& device_id,
           const std::string& mode);
  bool Close(base::PlatformThreadId pid, int handle);

  bool GetDeviceIds(std::vector<std::string>* device_ids);
  std::string GetDeviceName(const std::string& device_id);

  bool GetProperties(int handle, base::Value* properties);
  bool SetProperties(int handle, base::Value* properties);

  bool SetFormat(int handle,
                 int width,
                 int height,
                 const std::string& format,
                 int fps);
  int StartPreview(int handle);
  bool StopPreview(int handle);

  void SubscribeCameraChange(ResponseCB cb);
  void SubscribeFaultEvent(ResponseCB cb);

  bool OpenCameraBuffer(int shmem_key);
  bool ReadCameraBuffer(uint8_t** buffer, int* size);
  bool CloseCameraBuffer();

  bool GetRootDictionary(const std::string& payload,
                         std::unique_ptr<base::DictionaryValue>* root);

 private:
  struct LunaCbHandle {
    std::string uri;
    std::string* response = nullptr;
    base::WaitableEvent sync_done;
  };

  friend class base::RefCountedThreadSafe<WebOSCameraService>;

  WebOSCameraService(const WebOSCameraService&) = delete;
  WebOSCameraService& operator=(const WebOSCameraService&) = delete;

  bool LunaCallInternal(const std::string& uri,
                        const std::string& param,
                        std::string* response,
                        int64_t timeout = 2000);

  void OnLunaCallResponse(LunaCbHandle* handle, const std::string& response);

  std::unique_ptr<base::LunaServiceClient> luna_service_client_;

  LSMessageToken fault_event_subscribe_key_ = 0;
  LSMessageToken camera_list_subscribe_key_ = 0;

  scoped_refptr<base::SingleThreadTaskRunner> luna_task_runner_ = nullptr;

  base::Lock camera_service_lock_;

  std::unique_ptr<camera::CameraBuffer> camera_buffer_;

  base::WeakPtr<WebOSCameraService> weak_this_;
  base::WeakPtrFactory<WebOSCameraService> weak_factory_;
};

}  // namespace media

#endif  // MEDIA_CAPTURE_VIDEO_WEBOS_WEBOS_CAMERA_SERVICE_H_
