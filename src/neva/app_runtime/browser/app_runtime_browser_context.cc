// Copyright 2016 LG Electronics, Inc.
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

#include "neva/app_runtime/browser/app_runtime_browser_context.h"

#include "base/base_paths_posix.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/task/post_task.h"
#include "browser/app_runtime_browser_context_adapter.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/json_pref_store.h"
#include "components/prefs/pref_filter.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/resource_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"
#include "net/base/http_user_agent_settings.h"
#include "net/cookies/cookie_store.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "neva/app_runtime/browser/app_runtime_browser_switches.h"
#include "neva/app_runtime/browser/media/webrtc/device_media_stream_access_handler.h"
#include "neva/app_runtime/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "neva/app_runtime/browser/notifications/platform_notification_service_impl.h"
#include "neva/app_runtime/browser/permissions/permission_manager_factory.h"
#include "neva/app_runtime/browser/push_messaging/push_messaging_app_identifier.h"
#include "neva/app_runtime/browser/push_messaging/push_messaging_service_factory.h"
#include "neva/app_runtime/browser/push_messaging/push_messaging_service_impl.h"
#include "neva/user_agent/browser/client_hints.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"

namespace neva_app_runtime {

AppRuntimeBrowserContext::AppRuntimeBrowserContext(
    const BrowserContextAdapter* adapter)
    : adapter_(adapter),
      resource_context_(new content::ResourceContext()),
      path_(InitPath(adapter)) {
#if defined(USE_LOCAL_STORAGE_TRACKER)
  local_storage_tracker_ = content::LocalStorageTracker::Create().release();
#endif

  base::FilePath filepath = path_.AppendASCII("wam_prefs.json");
  LOG(INFO) << __func__ << " json_pref_store_path=" << filepath;
  scoped_refptr<JsonPrefStore> pref_store =
      base::MakeRefCounted<JsonPrefStore>(filepath);
  pref_store->ReadPrefs();  // Synchronous.

  PrefServiceFactory factory;
  factory.set_user_prefs(pref_store);

  user_prefs::PrefRegistrySyncable* pref_registry =
      new user_prefs::PrefRegistrySyncable;

  PlatformNotificationServiceImpl::RegisterProfilePrefs(pref_registry);
  PushMessagingAppIdentifier::RegisterProfilePrefs(pref_registry);
  HostContentSettingsMap::RegisterProfilePrefs(pref_registry);

  MediaCaptureDevicesDispatcher::RegisterProfilePrefs(pref_registry);
  DeviceMediaStreamAccessHandler::RegisterProfilePrefs(pref_registry);

  pref_service_ = factory.Create(pref_registry);
  user_prefs::UserPrefs::Set(this, pref_service_.get());

  PushMessagingServiceImpl::InitializeForProfile(this);

#if defined(__clang__)
  LOG(INFO) << "Compiler: clang";
#elif defined(COMPILER_GCC)
  LOG(INFO) << "Compiler: gcc";
#endif
}

AppRuntimeBrowserContext::~AppRuntimeBrowserContext() {}

base::FilePath AppRuntimeBrowserContext::InitPath(
    const BrowserContextAdapter* adapter) const {
  // Default value
  base::FilePath path;
  base::PathService::Get(base::DIR_CACHE, &path);

  // Overwrite default path value
  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch(kUserDataDir)) {
    base::FilePath new_path = cmd_line->GetSwitchValuePath(kUserDataDir);
    if (!new_path.empty()) {
      path = new_path;
      LOG(INFO) << "kUserDataDir is set.";
    } else {
      LOG(INFO) << "kUserDataDir is empty.";
    }
  } else {
    LOG(INFO) << "kUserDataDir isn't set.";
  }

  // Append storage name
  path = path.Append(adapter->GetStorageName());

  LOG(INFO) << "Will use path=" << path.value();

  return path;
}

base::FilePath AppRuntimeBrowserContext::GetPath() {
  return path_;
}

bool AppRuntimeBrowserContext::IsOffTheRecord() {
  return false;
}

content::ResourceContext* AppRuntimeBrowserContext::GetResourceContext() {
  return resource_context_.get();
}

content::DownloadManagerDelegate*
AppRuntimeBrowserContext::GetDownloadManagerDelegate() {
  return nullptr;
}

content::BrowserPluginGuestManager*
AppRuntimeBrowserContext::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy*
AppRuntimeBrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PushMessagingService*
AppRuntimeBrowserContext::GetPushMessagingService() {
  return PushMessagingServiceFactory::GetForProfile(this);
}

content::StorageNotificationService*
AppRuntimeBrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate*
AppRuntimeBrowserContext::GetSSLHostStateDelegate() {
  return nullptr;
}

std::unique_ptr<content::ZoomLevelDelegate>
AppRuntimeBrowserContext::CreateZoomLevelDelegate(const base::FilePath&) {
  return nullptr;
}

content::PermissionControllerDelegate*
AppRuntimeBrowserContext::GetPermissionControllerDelegate() {
  return PermissionManagerFactory::GetForBrowserContext(this);
}

content::BackgroundFetchDelegate* AppRuntimeBrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
AppRuntimeBrowserContext::GetBackgroundSyncController() {
  return nullptr;
}

content::BrowsingDataRemoverDelegate*
AppRuntimeBrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

void AppRuntimeBrowserContext::Initialize() {
#if defined(USE_LOCAL_STORAGE_TRACKER)
  local_storage_tracker_->Initialize(GetPath());
#endif
}

content::LocalStorageTracker*
AppRuntimeBrowserContext::GetLocalStorageTracker() {
#if defined(USE_LOCAL_STORAGE_TRACKER)
  return local_storage_tracker_.get();
#else
  return nullptr;
#endif
}

void AppRuntimeBrowserContext::FlushCookieStore() {
  GetDefaultStoragePartition()
      ->GetCookieManagerForBrowserProcess()
      ->FlushCookieStore(
          network::mojom::CookieManager::FlushCookieStoreCallback());
}

content::ClientHintsControllerDelegate*
AppRuntimeBrowserContext::GetClientHintsControllerDelegate() {
  return new neva_user_agent::ClientHints();
}

}  // namespace neva_app_runtime
