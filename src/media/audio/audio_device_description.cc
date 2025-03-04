// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/audio_device_description.h"

#include <utility>

#include "base/bind.h"
#include "base/notreached.h"
#include "build/chromecast_buildflags.h"
#include "media/base/localized_strings.h"

namespace media {
const char AudioDeviceDescription::kDefaultDeviceId[] = "default";
const char AudioDeviceDescription::kCommunicationsDeviceId[] = "communications";
const char AudioDeviceDescription::kLoopbackInputDeviceId[] = "loopback";
const char AudioDeviceDescription::kLoopbackWithMuteDeviceId[] =
    "loopbackWithMute";

// static
bool AudioDeviceDescription::IsDefaultDevice(const std::string& device_id) {
#if defined(USE_WEBOS_AUDIO)
  if (!device_id.empty() &&
      device_id.compare(AudioDeviceDescription::kDefaultDeviceId)) {
    if ((device_id.size() - 1) > 0) {
      std::string device_name = device_id.substr(0, device_id.size() - 1);
      if (device_name == AudioDeviceDescription::kDefaultDeviceId)
        return true;
    }
  }
#endif
  return device_id.empty() ||
         device_id == AudioDeviceDescription::kDefaultDeviceId;
}

// static
bool AudioDeviceDescription::IsCommunicationsDevice(
    const std::string& device_id) {
  return device_id == AudioDeviceDescription::kCommunicationsDeviceId;
}

// static
bool AudioDeviceDescription::IsLoopbackDevice(const std::string& device_id) {
  return device_id.compare(kLoopbackInputDeviceId) == 0 ||
         device_id.compare(kLoopbackWithMuteDeviceId) == 0;
}

// static
bool AudioDeviceDescription::UseSessionIdToSelectDevice(
    const base::UnguessableToken& session_id,
    const std::string& device_id) {
  return !session_id.is_empty() && device_id.empty();
}

// static
std::string AudioDeviceDescription::GetDefaultDeviceName() {
#if !defined(OS_IOS)
  return GetLocalizedStringUTF8(DEFAULT_AUDIO_DEVICE_NAME);
#else
  NOTREACHED();
  return "";
#endif
}

// static
std::string AudioDeviceDescription::GetCommunicationsDeviceName() {
#if defined(OS_WIN)
  return GetLocalizedStringUTF8(COMMUNICATIONS_AUDIO_DEVICE_NAME);
#elif BUILDFLAG(IS_CHROMECAST)
  return "";
#else
  NOTREACHED();
  return "";
#endif
}

// static
std::string AudioDeviceDescription::GetDefaultDeviceName(
    const std::string& real_device_name) {
  if (real_device_name.empty())
    return GetDefaultDeviceName();
  // TODO(guidou): Put the names together in a localized manner.
  // http://crbug.com/788767
  return GetDefaultDeviceName() + " - " + real_device_name;
}

#if defined(USE_WEBOS_AUDIO)
std::string AudioDeviceDescription::GetDefaultDeviceId(
    const std::string& display_id) {
  if (display_id.empty())
    return std::string();

  // For display 1 (id = 0) the audio sink device id is "default1"
  // For display 2 (id = 1) the audio sink device id is "default2"
  // So, adding 1 to display_id to get desired virtual sink number.
  int device_number = std::stoi(display_id) + 1;
  return std::string(kDefaultDeviceId) + std::to_string(device_number);
}
#endif

// static
std::string AudioDeviceDescription::GetCommunicationsDeviceName(
    const std::string& real_device_name) {
  if (real_device_name.empty())
    return GetCommunicationsDeviceName();
  // TODO(guidou): Put the names together in a localized manner.
  // http://crbug.com/788767
  return GetCommunicationsDeviceName() + " - " + real_device_name;
}

// static
void AudioDeviceDescription::LocalizeDeviceDescriptions(
    AudioDeviceDescriptions* device_descriptions) {
  for (auto& description : *device_descriptions) {
    if (media::AudioDeviceDescription::IsDefaultDevice(description.unique_id)) {
      description.device_name =
          media::AudioDeviceDescription::GetDefaultDeviceName(
              description.device_name);
    } else if (media::AudioDeviceDescription::IsCommunicationsDevice(
                   description.unique_id)) {
      description.device_name =
          media::AudioDeviceDescription::GetCommunicationsDeviceName(
              description.device_name);
    }
  }
}

AudioDeviceDescription::AudioDeviceDescription(std::string device_name,
                                               std::string unique_id,
                                               std::string group_id)
    : device_name(std::move(device_name)),
      unique_id(std::move(unique_id)),
      group_id(std::move(group_id)) {}

}  // namespace media
