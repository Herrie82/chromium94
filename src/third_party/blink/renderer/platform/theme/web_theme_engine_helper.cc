// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/theme/web_theme_engine_helper.h"

#include "build/build_config.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"

#if defined(OS_ANDROID)
#include "third_party/blink/renderer/platform/theme/web_theme_engine_android.h"
#elif defined(OS_MAC)
#include "third_party/blink/renderer/platform/theme/web_theme_engine_mac.h"
#else
#include "third_party/blink/renderer/platform/theme/web_theme_engine_default.h"
#endif

#if defined(USE_NEVA_APPRUNTIME)
// TODO(neva, 92.0.4515.0): Read the comment below
#include "third_party/blink/public/platform/web_security_origin.h"
#endif

#if defined(USE_NEVA_MEDIA)
#include "media/neva/media_preferences.h"
#endif

namespace blink {

namespace {
std::unique_ptr<WebThemeEngine> CreateWebThemeEngine() {
#if defined(OS_ANDROID)
  return std::make_unique<WebThemeEngineAndroid>();
#elif defined(OS_MAC)
  return std::make_unique<WebThemeEngineMac>();
#else
  return std::make_unique<WebThemeEngineDefault>();
#endif
}

}  // namespace

WebThemeEngine* WebThemeEngineHelper::GetNativeThemeEngine() {
  DEFINE_STATIC_LOCAL(std::unique_ptr<WebThemeEngine>, theme_engine,
                      {CreateWebThemeEngine()});
  return theme_engine.get();
}

void WebThemeEngineHelper::DidUpdateRendererPreferences(
    const blink::RendererPreferences& renderer_prefs) {
#if defined(OS_WIN)
  // Update Theme preferences on Windows.
  WebThemeEngineDefault::cacheScrollBarMetrics(
      renderer_prefs.vertical_scroll_bar_width_in_dips,
      renderer_prefs.horizontal_scroll_bar_height_in_dips,
      renderer_prefs.arrow_bitmap_height_vertical_scroll_bar_in_dips,
      renderer_prefs.arrow_bitmap_width_horizontal_scroll_bar_in_dips);
#endif

#if defined(USE_NEVA_MEDIA)
  std::string media_codec_capability =
      renderer_prefs.media_codec_capability;
  if (!media_codec_capability.empty())
    media::MediaPreferences::Get()->SetMediaCodecCapabilities(
        media_codec_capability);

  std::string media_preferences = renderer_prefs.media_preferences;
  if (!media_preferences.empty())
    media::MediaPreferences::Get()->Update(media_preferences);
#endif  // defined(USE_NEVA_MEDIA)
}

}  // namespace blink
