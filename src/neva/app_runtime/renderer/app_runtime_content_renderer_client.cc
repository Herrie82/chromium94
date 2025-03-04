// Copyright 2016-2019 LG Electronics, Inc.
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

#include "neva/app_runtime/renderer/app_runtime_content_renderer_client.h"

#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "components/watchdog/switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_thread_observer.h"
#include "content/public/renderer/render_view.h"
#include "net/base/filename_util.h"
#include "neva/app_runtime/app/app_runtime_main_delegate.h"
#include "neva/app_runtime/common/app_runtime_file_access_controller.h"
#include "neva/app_runtime/public/webview_info.h"
#include "neva/app_runtime/renderer/app_runtime_page_load_timing_render_frame_observer.h"
#include "neva/app_runtime/renderer/app_runtime_render_frame_observer.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_security_policy.h"
#include "third_party/blink/public/web/web_view.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/webui/jstemplate_builder.h"
#include "url/url_constants.h"

#if defined(USE_NEVA_MEDIA)
#include "components/cdm/renderer/neva/key_systems_util.h"
#include "content/renderer/render_thread_impl.h"
#include "media/neva/media_preferences.h"
#endif

#if defined(USE_NEVA_EXTENSIONS)
#include "content/public/common/content_constants.h"
#include "extensions/renderer/dispatcher.h"
#include "extensions/renderer/extension_frame_helper.h"
#include "extensions/renderer/guest_view/extensions_guest_view_container.h"
#include "extensions/renderer/guest_view/extensions_guest_view_container_dispatcher.h"
#include "extensions/renderer/guest_view/mime_handler_view/mime_handler_view_container.h"
#include "extensions/shell/common/shell_extensions_client.h"
#include "extensions/shell/renderer/shell_extensions_renderer_client.h"

namespace neva_app_runtime {

class AppRuntimeExtensionsRendererClient
    : public extensions::ShellExtensionsRendererClient {
 public:
  AppRuntimeExtensionsRendererClient() = default;
  ~AppRuntimeExtensionsRendererClient() override = default;
  AppRuntimeExtensionsRendererClient(
      const AppRuntimeExtensionsRendererClient&) = delete;
  AppRuntimeExtensionsRendererClient& operator=(
      const AppRuntimeExtensionsRendererClient&) = delete;

  void OnExtensionLoaded(const extensions::Extension& extension) override;
};

}  // namespace neva_app_runtime
#endif

using blink::mojom::FetchCacheMode;

namespace neva_app_runtime {

AppRuntimeContentRendererClient::AppRuntimeContentRendererClient() {}

AppRuntimeContentRendererClient::~AppRuntimeContentRendererClient() {}

void AppRuntimeContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
#if defined(USE_NEVA_EXTENSIONS)
  // ExtensionFrameHelper destroys itself when the RenderFrame is destroyed.
  new extensions::ExtensionFrameHelper(
      render_frame, extensions_renderer_client_->GetDispatcher());

  extensions_renderer_client_->GetDispatcher()->OnRenderFrameCreated(
      render_frame);
#endif
  // AppRuntimeRenderFrameObserver destroys itself when the RenderFrame is
  // destroyed.
  new AppRuntimeRenderFrameObserver(render_frame);
  // Only attach AppRuntimePageLoadTimingRenderFrameObserver to the main frame,
  // since we only want to observe page load timing for the main frame.
  if (render_frame->IsMainFrame()) {
    new AppRuntimePageLoadTimingRenderFrameObserver(render_frame);
  }
}

bool AppRuntimeContentRendererClient::IsAccessAllowedForURL(
    const blink::WebURL& url) {
  // Ignore non-file scheme requests
  if (!static_cast<GURL>(url).SchemeIsFile())
    return true;

  const neva_app_runtime::AppRuntimeFileAccessController*
      file_access_controller = GetFileAccessController();

  base::FilePath file_path;
  if (!net::FileURLToFilePath(url, &file_path))
    return false;

  if (file_access_controller)
    return file_access_controller->IsAccessAllowed(file_path, webview_info_);

  return false;
}

void AppRuntimeContentRendererClient::RegisterSchemes() {
  // webapp needs the file scheme to register a service worker. Used from
  // third_party/blink/renderer/modules/service_worker/service_worker_container.cc
  blink::WebString file_scheme(blink::WebString::FromASCII(url::kFileScheme));
  blink::WebSecurityPolicy::RegisterURLSchemeAsAllowingServiceWorkers(
      file_scheme);
}

void AppRuntimeContentRendererClient::WillSendRequest(
    blink::WebLocalFrame* frame,
    ui::PageTransition transition_type,
    const blink::WebURL& url,
    const net::SiteForCookies& site_for_cookies,
    const url::Origin* initiator_origin,
    GURL* new_url) {
  // Ignore non-file scheme requests
  if (!static_cast<GURL>(url).SchemeIsFile())
    return;

  const AppRuntimeFileAccessController* file_access_controller =
      GetFileAccessController();

  if (file_access_controller) {
    // Ignore navigations since they are handled on browser side
    if (!ui::PageTransitionTypeIncludingQualifiersIs(transition_type,
                                                     ui::PAGE_TRANSITION_FIRST))
      return;

    base::FilePath file_path;
    if (!net::FileURLToFilePath(GURL(url), &file_path) ||
        !file_access_controller->IsAccessAllowed(file_path, webview_info_)) {
      blink::WebConsoleMessage error_msg;
      error_msg.level = blink::mojom::ConsoleMessageLevel::kError;
      error_msg.text = blink::WebString::FromASCII(
          "Access is blocked to resource: " + url.GetString().Ascii());
      frame->AddMessageToConsole(error_msg);

      // Redirect to unreachable URL
      *new_url = GURL(url::kIllegalDataURL);
    }
  }
}

void AppRuntimeContentRendererClient::SetWebViewInfo(
    const std::string& app_path, const std::string& trust_level) {
  webview_info_.app_path = app_path;
  webview_info_.trust_level = trust_level;
}

#if defined(USE_NEVA_MEDIA)
void AppRuntimeContentRendererClient::SetUseVideoDecodeAccelerator(bool use) {
  if (content::RenderThreadImpl::current())
    content::RenderThreadImpl::current()->SetUseVideoDecodeAccelerator(use);
}

void AppRuntimeContentRendererClient::AddSupportedKeySystems(
    std::vector<std::unique_ptr<media::KeySystemProperties>>* key_systems) {
  if (key_systems)
    cdm::AddSupportedKeySystems(*key_systems);
}

bool AppRuntimeContentRendererClient::IsSupportedAudioType(
    const media::AudioType& type) {
  return media::MediaPreferences::Get()->IsSupportedAudioType(type);
}

bool AppRuntimeContentRendererClient::IsSupportedVideoType(
    const media::VideoType& type) {
  return media::MediaPreferences::Get()->IsSupportedVideoType(type);
}
#endif

#if defined(USE_NEVA_EXTENSIONS)
void AppRuntimeExtensionsRendererClient::OnExtensionLoaded(
    const extensions::Extension& extension) {
  URLPattern pattern(URLPattern::SCHEME_FILE);
  pattern.SetMatchAllURLs(true);
  const_cast<extensions::Extension*>(&extension)->AddWebExtentPattern(pattern);
}
#endif  // defined(USE_NEVA_EXTENSIONS)

void AppRuntimeContentRendererClient::RenderThreadStarted() {
  content::RenderThread* thread = content::RenderThread::Get();

#if defined(USE_NEVA_EXTENSIONS)
  extensions_client_.reset(new ShellExtensionsClient);
  extensions::ExtensionsClient::Set(extensions_client_.get());

  extensions_renderer_client_.reset(new AppRuntimeExtensionsRendererClient);
  extensions::ExtensionsRendererClient::Set(extensions_renderer_client_.get());
  thread->AddObserver(extensions_renderer_client_->GetDispatcher());

  guest_view_container_dispatcher_.reset(
      new extensions::ExtensionsGuestViewContainerDispatcher());
  thread->AddObserver(guest_view_container_dispatcher_.get());
#endif  // defined(USE_NEVA_EXTENSIONS)

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(watchdog::switches::kEnableWatchdog)) {
    watchdog_.reset(new watchdog::Watchdog());

    std::string env_timeout = command_line->GetSwitchValueASCII(
        watchdog::switches::kWatchdogRendererTimeout);
    if (!env_timeout.empty()) {
      int timeout;
      if (base::StringToInt(env_timeout, &timeout))
        watchdog_->SetTimeout(timeout);
    }

    std::string env_period = command_line->GetSwitchValueASCII(
        watchdog::switches::kWatchdogRendererPeriod);
    if (!env_period.empty()) {
      int period;
      if (base::StringToInt(env_period, &period))
        watchdog_->SetPeriod(period);
    }

    watchdog_->StartWatchdog();

    // Check it's currently running on RenderThread.
    CHECK(thread);
    scoped_refptr<base::SingleThreadTaskRunner> task_runner =
        base::ThreadTaskRunnerHandle::Get();
    task_runner->PostTask(
        FROM_HERE, base::BindOnce(&AppRuntimeContentRendererClient::ArmWatchdog,
                                  base::Unretained(this)));
  }
}

void AppRuntimeContentRendererClient::ArmWatchdog() {
  watchdog_->Arm();
  if (!watchdog_->HasThreadInfo())
    watchdog_->SetCurrentThreadInfo();

  // Check it's currently running on RenderThread.
  CHECK(content::RenderThread::Get());
  scoped_refptr<base::SingleThreadTaskRunner> task_runner =
      base::ThreadTaskRunnerHandle::Get();
  task_runner->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AppRuntimeContentRendererClient::ArmWatchdog,
                     base::Unretained(this)),
      base::TimeDelta::FromSeconds(watchdog_->GetPeriod()));
}

#if defined(USE_NEVA_EXTENSIONS)
void AppRuntimeContentRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  extensions_renderer_client_->GetDispatcher()->RunScriptsAtDocumentStart(
      render_frame);
}

void AppRuntimeContentRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
  extensions_renderer_client_->GetDispatcher()->RunScriptsAtDocumentEnd(
      render_frame);
}
#endif  // defined(USE_NEVA_EXTENSIONS)

}  // namespace neva_app_runtime
