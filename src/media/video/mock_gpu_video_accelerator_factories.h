// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_VIDEO_MOCK_GPU_VIDEO_ACCELERATOR_FACTORIES_H_
#define MEDIA_VIDEO_MOCK_GPU_VIDEO_ACCELERATOR_FACTORIES_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/sequenced_task_runner.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "media/video/video_encode_accelerator.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace media {

class MockGpuVideoAcceleratorFactories : public GpuVideoAcceleratorFactories {
 public:
  explicit MockGpuVideoAcceleratorFactories(gpu::SharedImageInterface* sii);
  ~MockGpuVideoAcceleratorFactories() override;

  bool IsGpuVideoAcceleratorEnabled() override;

  MOCK_METHOD0(GetChannelToken, base::UnguessableToken());
  MOCK_METHOD0(GetCommandBufferRouteId, int32_t());

  MOCK_METHOD1(IsDecoderConfigSupported, Supported(const VideoDecoderConfig&));
  MOCK_METHOD0(GetDecoderType, VideoDecoderType());
  MOCK_METHOD0(IsDecoderSupportKnown, bool());
  MOCK_METHOD1(NotifyDecoderSupportKnown, void(base::OnceClosure));
  MOCK_METHOD2(CreateVideoDecoder,
               std::unique_ptr<media::VideoDecoder>(MediaLog*,
                                                    RequestOverlayInfoCB));

  MOCK_METHOD0(IsEncoderSupportKnown, bool());
  MOCK_METHOD1(NotifyEncoderSupportKnown, void(base::OnceClosure));
  // CreateVideoEncodeAccelerator returns scoped_ptr, which the mocking
  // framework does not want. Trampoline it.
  MOCK_METHOD0(DoCreateVideoEncodeAccelerator, VideoEncodeAccelerator*());

  MOCK_METHOD0(GetTaskRunner, scoped_refptr<base::SequencedTaskRunner>());
  MOCK_METHOD0(GetMediaContextProvider, viz::RasterContextProvider*());
  MOCK_METHOD1(SetRenderingColorSpace, void(const gfx::ColorSpace&));
  MOCK_CONST_METHOD0(GetRenderingColorSpace, const gfx::ColorSpace&());

#if defined(USE_NEVA_MEDIA)
  MOCK_METHOD1(SetUseVideoDecodeAccelerator, void(bool));
  MOCK_METHOD0(UseVideoDecodeAccelerator, bool());
#endif

  std::unique_ptr<gfx::GpuMemoryBuffer> CreateGpuMemoryBuffer(
      const gfx::Size& size,
      gfx::BufferFormat format,
      gfx::BufferUsage usage) override;

  bool ShouldUseGpuMemoryBuffersForVideoFrames(
      bool for_media_stream) const override;
  unsigned ImageTextureTarget(gfx::BufferFormat format) override;
  OutputFormat VideoFrameOutputFormat(VideoPixelFormat pixel_format) override {
    return video_frame_output_format_;
  }

  gpu::SharedImageInterface* SharedImageInterface() override { return sii_; }
  gpu::GpuMemoryBufferManager* GpuMemoryBufferManager() override {
    return nullptr;
  }

  void SetVideoFrameOutputFormat(const OutputFormat video_frame_output_format) {
    video_frame_output_format_ = video_frame_output_format;
  }

  void SetFailToAllocateGpuMemoryBufferForTesting(bool fail) {
    fail_to_allocate_gpu_memory_buffer_ = fail;
  }

  void SetFailToMapGpuMemoryBufferForTesting(bool fail) {
    fail_to_map_gpu_memory_buffer_ = fail;
  }

  void SetGpuMemoryBuffersInUseByMacOSWindowServer(bool in_use);

  // Allocate & return a read-only shared memory region
  base::UnsafeSharedMemoryRegion CreateSharedMemoryRegion(size_t size) override;

  std::unique_ptr<VideoEncodeAccelerator> CreateVideoEncodeAccelerator()
      override;
  absl::optional<VideoEncodeAccelerator::SupportedProfiles>
  GetVideoEncodeAcceleratorSupportedProfiles() override {
    return VideoEncodeAccelerator::SupportedProfiles();
  }

  const std::vector<gfx::GpuMemoryBuffer*>& created_memory_buffers() {
    return created_memory_buffers_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MockGpuVideoAcceleratorFactories);

  base::Lock lock_;
  OutputFormat video_frame_output_format_ = OutputFormat::I420;

  bool fail_to_allocate_gpu_memory_buffer_ = false;

  bool fail_to_map_gpu_memory_buffer_ = false;

  gpu::SharedImageInterface* sii_;

  std::vector<gfx::GpuMemoryBuffer*> created_memory_buffers_;
};

}  // namespace media

#endif  // MEDIA_VIDEO_MOCK_GPU_VIDEO_ACCELERATOR_FACTORIES_H_
