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

#include "neva/app_runtime/renderer/app_runtime_render_frame_observer.h"

#include "base/bind.h"
#include "base/memory/memory_pressure_listener.h"
#include "components/media_control/renderer/media_playback_options.h"
#include "content/public/common/page_visibility_state.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "neva/app_runtime/app/app_runtime_main_delegate.h"
#include "neva/app_runtime/common/app_runtime_file_access_controller.h"
#include "neva/app_runtime/renderer/app_runtime_content_renderer_client.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_frame_load_type.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/bindings/core/v8/serialization/serialized_script_value.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "ui/base/resource/resource_bundle.h"

namespace neva_app_runtime {

AppRuntimeRenderFrameObserver::AppRuntimeRenderFrameObserver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  render_frame->GetAssociatedInterfaceRegistry()->AddInterface(
      base::BindRepeating(&AppRuntimeRenderFrameObserver::BindPendingReceiver,
                          base::Unretained(this)));

  new media_control::MediaPlaybackOptions(render_frame);
}

AppRuntimeRenderFrameObserver::~AppRuntimeRenderFrameObserver() = default;

void AppRuntimeRenderFrameObserver::SetWebViewInfo(
    const std::string& app_path,
    const std::string& trust_level) {
  const AppRuntimeFileAccessController* file_access_controller =
      GetFileAccessController();
  if (!file_access_controller)
    return;

  auto* renderer_client = static_cast<AppRuntimeContentRendererClient*>(
      GetAppRuntimeContentClient()->renderer());
  renderer_client->SetWebViewInfo(app_path, trust_level);
}

void AppRuntimeRenderFrameObserver::BindPendingReceiver(
    mojo::PendingAssociatedReceiver<mojom::AppRuntimeWebViewClient>
        pending_receiver) {
  receiver_.Bind(std::move(pending_receiver));
}

void AppRuntimeRenderFrameObserver::OnDestruct() {
  delete this;
}

void AppRuntimeRenderFrameObserver::SuspendDOM() {
  if (dom_suspended_)
    return;
  dom_suspended_ = true;

  page_pauser_ = blink::WebScopedPagePauser::Create();
}

void AppRuntimeRenderFrameObserver::ResumeDOM() {
  if (!dom_suspended_)
    return;
  dom_suspended_ = false;

  page_pauser_.reset();

  mojo::AssociatedRemote<mojom::AppRuntimeWebViewHost> interface;
  render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(&interface);
  if (interface)
    interface->DidResumeDOM();
}

void AppRuntimeRenderFrameObserver::ResetStateToMarkNextPaint() {
  render_frame()->ResetStateToMarkNextPaint();
}

void AppRuntimeRenderFrameObserver::SetVisibilityState(
    mojom::VisibilityState visibility_state) {
  content::PageVisibilityState page_visibility_state;
  if (visibility_state == mojom::VisibilityState::kVisible)
    page_visibility_state = content::PageVisibilityState::kVisible;
  else
    page_visibility_state = content::PageVisibilityState::kHidden;
  render_frame()->GetWebFrame()->View()->SetVisibilityState(
      page_visibility_state,
      visibility_state == mojom::VisibilityState::kLaunching);
  render_frame()->GetWebFrame()->FrameWidget()->SetCompositorVisible(
      visibility_state != mojom::VisibilityState::kHidden);
}

void AppRuntimeRenderFrameObserver::ChangeLocale(const std::string& locale) {
  // Set LANGUAGE environment variable to required locale, so glib could return
  // it throught function g_get_language_names.
  // Correct way will be to set locale by std::setlocale function, but it
  // works only if corresponding locale is installed in linux, that is not
  // always the case.
  // This way works as glib does not check if locale in LANGUAGE variable is
  // actually installed in linux.
  setenv("LANGUAGE", locale.c_str(), 1);

  ui::ResourceBundle::GetSharedInstance().ReloadLocaleResources(locale);
  ui::ResourceBundle::GetSharedInstance().ReloadFonts();
}

void AppRuntimeRenderFrameObserver::SetNetworkQuietTimeout(double timeout) {
  render_frame()->GetRenderView()->GetWebView()->GetSettings()->
      SetNetworkQuietTimeout(timeout);
}

void AppRuntimeRenderFrameObserver::SetDisallowScrollbarsInMainFrame(
    bool disallow) {
  if (!render_frame()->IsMainFrame())
    return;

  render_frame()->GetRenderView()->GetWebView()->GetSettings()->
      SetDisallowScrollbarsInMainFrame(disallow);
}

void AppRuntimeRenderFrameObserver::DidClearWindowObject() {
  injection_loader_.Load(render_frame()->GetWebFrame());

  if (!render_frame()->IsMainFrame())
    return;

  mojo::AssociatedRemote<mojom::AppRuntimeWebViewHost> interface;
  render_frame()->GetRemoteAssociatedInterfaces()->GetInterface(&interface);
  interface->DidClearWindowObject();
}

void AppRuntimeRenderFrameObserver::GrantLoadLocalResources() {
  render_frame()->GetWebFrame()->GetDocument().GrantLoadLocalResources();
}

void AppRuntimeRenderFrameObserver::InsertStyleSheet(const std::string& css) {
  render_frame()->GetWebFrame()->GetDocument().InsertStyleSheet(
      blink::WebString::FromUTF8(css));
}

void AppRuntimeRenderFrameObserver::ReplaceBaseURL(const std::string& new_url) {
  blink::WebNavigationControl* navi_control =
      static_cast<blink::WebNavigationControl*>(render_frame()->GetWebFrame());
  if (!navi_control)
    return;
  navi_control->UpdateForSameDocumentNavigation(new_url);
}

void AppRuntimeRenderFrameObserver::AddInjectionToLoad(
    const std::string& injection, const std::string& arguments_json) {
  injection_loader_.Add(injection, arguments_json);
}

void AppRuntimeRenderFrameObserver::UnloadInjections() {
  injection_loader_.Unload();
}

void AppRuntimeRenderFrameObserver::SetUseVideoDecodeAccelerator(bool use) {
#if defined(USE_NEVA_MEDIA)
  auto* renderer_client = static_cast<AppRuntimeContentRendererClient*>(
      GetAppRuntimeContentClient()->renderer());
  renderer_client->SetUseVideoDecodeAccelerator(use);
#endif
}

}  // namespace neva_app_runtime
