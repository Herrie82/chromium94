// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_APP_CONTENT_MAIN_RUNNER_IMPL_H_
#define CONTENT_APP_CONTENT_MAIN_RUNNER_IMPL_H_

#include <memory>

#include "base/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "base/metrics/field_trial.h"
#include "base/threading/hang_watcher.h"
#include "build/build_config.h"
#include "content/browser/startup_data_impl.h"
#include "content/common/content_export.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/common/main_function_params.h"
#include "mojo/core/embedder/scoped_ipc_support.h"

#if defined(OS_WIN)
#include "sandbox/win/src/sandbox_types.h"
#elif defined(OS_MAC)
#include "base/mac/scoped_nsautorelease_pool.h"
#endif  // OS_WIN

#if defined(USE_LTTNG)
#include "base/native_library.h"
#endif

namespace base {
class AtExitManager;
}  // namespace base

namespace discardable_memory {
class DiscardableSharedMemoryManager;
}

namespace content {
class ContentClient;
class ContentMainDelegate;
class MojoIpcSupport;
struct ContentMainParams;

class ContentMainRunnerImpl : public ContentMainRunner {
 public:
  static std::unique_ptr<ContentMainRunnerImpl> Create();

  ContentMainRunnerImpl();
  ~ContentMainRunnerImpl() override;

  int TerminateForFatalInitializationError();

  // ContentMainRunner:
  int Initialize(const ContentMainParams& params) override;
  int Run(bool start_minimal_browser) override;
  void Shutdown() override;

 private:
  int RunBrowser(MainFunctionParams& main_function_params,
                 bool start_minimal_browser);

  bool is_browser_main_loop_started_ = false;

  // The hang watcher is leaked to make sure it survives all watched threads.
  base::HangWatcher* hang_watcher_;

  // Unregisters UI thread from hang watching on destruction.
  // NOTE: The thread should be unregistered before HangWatcher stops so this
  // member must be after |hang_watcher|.
  base::ScopedClosureRunner unregister_thread_closure_;

  std::unique_ptr<discardable_memory::DiscardableSharedMemoryManager>
      discardable_shared_memory_manager_;
  std::unique_ptr<StartupDataImpl> startup_data_;
  std::unique_ptr<MojoIpcSupport> mojo_ipc_support_;

  // True if the runner has been initialized.
  bool is_initialized_ = false;

  // True if the runner has been shut down.
  bool is_shutdown_ = false;

  // True if basic startup was completed.
  bool completed_basic_startup_ = false;

  // The delegate will outlive this object.
  ContentMainDelegate* delegate_ = nullptr;

  std::unique_ptr<base::AtExitManager> exit_manager_;

#if defined(OS_WIN)
  sandbox::SandboxInterfaceInfo sandbox_info_;
#elif defined(OS_MAC)
  base::mac::ScopedNSAutoreleasePool* autorelease_pool_ = nullptr;
#endif
#if defined(USE_LTTNG)
  base::NativeLibrary lttng_native_library_ = nullptr;
#endif

  base::OnceClosure* ui_task_ = nullptr;

  CreatedMainPartsClosure* created_main_parts_closure_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ContentMainRunnerImpl);
};

// The BrowserTestBase on Android does not call ContentMain(). It tries instead
// to reproduce it more or less accurately. This requires to use
// GetContentMainDelegateForTesting() and GetContentClientForTesting().
// BrowserTestBase is implemented in content/public and GetContentClient() is
// only available to the implementation of content. Hence these functions.
CONTENT_EXPORT ContentClient* GetContentClientForTesting();
#if defined(OS_ANDROID)
CONTENT_EXPORT ContentMainDelegate* GetContentMainDelegateForTesting();
#endif

}  // namespace content

#endif  // CONTENT_APP_CONTENT_MAIN_RUNNER_IMPL_H_
