// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <utility>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/debugger.h"
#include "base/debug/leak_annotations.h"
#include "base/i18n/rtl.h"
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#include "base/metrics/histogram_macros.h"
#include "base/pending_task.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequence_manager/sequence_manager.h"
#include "base/threading/platform_thread.h"
#include "base/timer/hi_res_timer_manager.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "content/common/content_constants_internal.h"
#include "content/common/content_switches_internal.h"
#include "content/common/partition_alloc_support.h"
#include "content/common/skia_utils.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/main_function_params.h"
#include "content/public/renderer/content_renderer_client.h"
#include "content/public/renderer/render_thread.h"
#include "content/renderer/render_process_impl.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/renderer_main_platform_delegate.h"
#include "media/media_buildflags.h"
#include "mojo/public/cpp/bindings/mojo_buildflags.h"
#include "ppapi/buildflags/buildflags.h"
#include "sandbox/policy/switches.h"
#include "services/tracing/public/cpp/trace_startup.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/icu/source/common/unicode/unistr.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"
#include "third_party/webrtc_overrides/init_webrtc.h"  // nogncheck
#include "ui/base/ui_base_switches.h"

#if defined(OS_ANDROID)
#include "base/android/library_loader/library_loader_hooks.h"
#endif  // OS_ANDROID

#if defined(OS_MAC)
#include <Carbon/Carbon.h>
#include <signal.h>
#include <unistd.h>

#include "base/mac/scoped_nsautorelease_pool.h"
#include "base/message_loop/message_pump_mac.h"
#include "third_party/blink/public/web/web_view.h"
#endif  // OS_MAC

#if BUILDFLAG(IS_CHROMEOS_ASH)
#if defined(ARCH_CPU_X86_64)
#include "chromeos/memory/userspace_swap/userspace_swap_renderer_initialization_impl.h"
#endif  // defined(X86_64)
#include "chromeos/system/core_scheduling.h"
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

#if BUILDFLAG(ENABLE_PLUGINS)
#include "content/renderer/pepper/pepper_plugin_registry.h"
#endif

#if defined(USE_MEMORY_TRACE)
#include "base/trace_event/neva/memory_trace/memory_trace_manager.h"
#endif

#if BUILDFLAG(MOJO_RANDOM_DELAYS_ENABLED)
#include "mojo/public/cpp/bindings/lib/test_random_mojo_delays.h"
#endif

#if defined(USE_LTTNG)
#include "base/native_library.h"
#include "content/common/neva/lttng/lttng_init.h"
#endif

namespace content {
namespace {

// This function provides some ways to test crash and assertion handling
// behavior of the renderer.
void HandleRendererErrorTestParameters(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(switches::kWaitForDebugger))
    base::debug::WaitForDebugger(60, true);

  if (command_line.HasSwitch(switches::kRendererStartupDialog))
    WaitForDebugger("Renderer");
}

std::unique_ptr<base::MessagePump> CreateMainThreadMessagePump() {
#if defined(OS_MAC)
  // As long as scrollbars on Mac are painted with Cocoa, the message pump
  // needs to be backed by a Foundation-level loop to process NSTimers. See
  // http://crbug.com/306348#c24 for details.
  return base::MessagePump::Create(base::MessagePumpType::NS_RUNLOOP);
#elif defined(OS_WEBOS)
  // The main message loop of the renderer services for webOS should be UI
  // (luna bus require glib message pump).
  return base::MessagePump::Create(base::MessagePumpType::UI);
#elif defined(OS_FUCHSIA)
  // Allow FIDL APIs on renderer main thread.
  return base::MessagePump::Create(base::MessagePumpType::IO);
#else
  return base::MessagePump::Create(base::MessagePumpType::DEFAULT);
#endif
}

}  // namespace

// mainline routine for running as the Renderer process
int RendererMain(const MainFunctionParams& parameters) {
  // Don't use the TRACE_EVENT0 macro because the tracing infrastructure doesn't
  // expect synchronous events around the main loop of a thread.
  TRACE_EVENT_NESTABLE_ASYNC_BEGIN1("startup", "RendererMain",
                                    TRACE_ID_WITH_SCOPE("RendererMain", 0),
                                    "zygote_child", false);
#if defined(USE_LTTNG)
  base::NativeLibrary lttng_native_library = neva::LttngInit();
#endif

  base::trace_event::TraceLog::GetInstance()->set_process_name("Renderer");
  base::trace_event::TraceLog::GetInstance()->SetProcessSortIndex(
      kTraceEventRendererProcessSortIndex);

  const base::CommandLine& command_line = parameters.command_line;

#if defined(OS_MAC)
  base::mac::ScopedNSAutoreleasePool* pool = parameters.autorelease_pool;
#endif  // OS_MAC

#if BUILDFLAG(IS_CHROMEOS_ASH)
  // As the Zygote process starts up earlier than the browser process, it gets
  // its own locale (at login time for Chrome OS). So we have to set the ICU
  // default locale for the renderer process here.
  // ICU locale will be used for fallback font selection, etc.
  if (command_line.HasSwitch(switches::kLang)) {
    const std::string locale =
        command_line.GetSwitchValueASCII(switches::kLang);
    base::i18n::SetICUDefaultLocale(locale);
  }

  // When we start the renderer on ChromeOS if the system has core scheduling
  // available we want to turn it on.
  chromeos::system::EnableCoreSchedulingIfAvailable();

#if defined(ARCH_CPU_X86_64)
  using UserspaceSwapInit =
      chromeos::memory::userspace_swap::UserspaceSwapRendererInitializationImpl;
  absl::optional<UserspaceSwapInit> swap_init;
  if (UserspaceSwapInit::UserspaceSwapSupportedAndEnabled()) {
    swap_init.emplace();

    PLOG_IF(ERROR, !swap_init->PreSandboxSetup())
        << "Unable to complete presandbox userspace swap initialization";
  }
#endif  // defined(ARCH_CPU_X86_64)
#endif  // defined(IS_CHROMEOS_ASH)

  if (command_line.HasSwitch(switches::kTimeZoneForTesting)) {
    std::string time_zone =
        command_line.GetSwitchValueASCII(switches::kTimeZoneForTesting);
    icu::TimeZone::adoptDefault(
        icu::TimeZone::createTimeZone(icu::UnicodeString(time_zone.c_str())));
  }

  InitializeSkia();

  // This function allows pausing execution using the --renderer-startup-dialog
  // flag allowing us to attach a debugger.
  // Do not move this function down since that would mean we can't easily debug
  // whatever occurs before it.
  HandleRendererErrorTestParameters(command_line);

  RendererMainPlatformDelegate platform(parameters);

  base::PlatformThread::SetName("CrRendererMain");

  // Force main thread initialization. When the implementation is based on a
  // better means of determining which is the main thread, remove.
  RenderThread::IsMainThread();

  blink::Platform::InitializeBlink();
  std::unique_ptr<blink::scheduler::WebThreadScheduler> main_thread_scheduler =
      blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
          CreateMainThreadMessagePump());

  platform.PlatformInitialize();

#if BUILDFLAG(ENABLE_PLUGINS)
  // Load pepper plugins before engaging the sandbox.
  PepperPluginRegistry::GetInstance();
#endif
  // Initialize WebRTC before engaging the sandbox.
  // NOTE: On linux, this call could already have been made from
  // zygote_main_linux.cc.  However, calling multiple times from the same thread
  // is OK.
  InitializeWebRtcModule();

  {
    bool should_run_loop = true;
    bool need_sandbox =
        !command_line.HasSwitch(sandbox::policy::switches::kNoSandbox);

#if !defined(OS_WIN) && !defined(OS_MAC)
    // Sandbox is enabled before RenderProcess initialization on all platforms,
    // except Windows and Mac.
    // TODO(markus): Check if it is OK to remove ifdefs for Windows and Mac.
    if (need_sandbox) {
      should_run_loop = platform.EnableSandbox();
      need_sandbox = false;
    }
#endif

    std::unique_ptr<RenderProcess> render_process = RenderProcessImpl::Create();
    // It's not a memory leak since RenderThread has the same lifetime
    // as a renderer process.
    base::RunLoop run_loop;
    new RenderThreadImpl(run_loop.QuitClosure(),
                         std::move(main_thread_scheduler));

#if BUILDFLAG(IS_CHROMEOS_ASH) && defined(ARCH_CPU_X86_64)
    // Once the sandbox has been entered and initialization of render threads
    // complete we will transfer FDs to the browser, or close them on failure.
    // This should always be called because it will also transfer the errno that
    // prevented the creation of the userfaultfd if applicable.
    if (swap_init) {
      swap_init->TransferFDsOrCleanup(base::BindOnce(
          &RenderThread::BindHostReceiver,
          // Unretained is safe because TransferFDsOrCleanup is synchronous.
          base::Unretained(RenderThread::Get())));

      // No need to leave this around any further.
      swap_init.reset();
    }
#endif

#if defined(OS_POSIX) && !defined(OS_ANDROID) && !defined(OS_MAC)
    // Startup tracing is usually enabled earlier, but if we forked from a
    // zygote, we can only enable it after mojo IPC support is brought up
    // initialized by RenderThreadImpl, because the mojo broker has to create
    // the tracing SMB on our behalf due to the zygote sandbox.
    if (parameters.zygote_child) {
      tracing::EnableStartupTracingIfNeeded();
      TRACE_EVENT_NESTABLE_ASYNC_BEGIN1("startup", "RendererMain",
                                        TRACE_ID_WITH_SCOPE("RendererMain", 0),
                                        "zygote_child", true);
    }
#endif  // OS_POSIX && !OS_ANDROID && !OS_MAC

    if (need_sandbox)
      should_run_loop = platform.EnableSandbox();

#if BUILDFLAG(MOJO_RANDOM_DELAYS_ENABLED)
    mojo::BeginRandomMojoDelays();
#endif

#if defined(USE_MEMORY_TRACE)
    // Initialize MemoryTraceManager if --trace-memory-renderer is on.
    base::trace_event::neva::MemoryTraceManagerDelegate
        memory_trace_manager_delegate;
    memory_trace_manager_delegate.Initialize(false /* is_browser_process */);
#endif

    internal::PartitionAllocSupport::Get()->ReconfigureAfterTaskRunnerInit(
        switches::kRendererProcess);

    base::HighResolutionTimerManager hi_res_timer_manager;

    if (should_run_loop) {
#if defined(OS_MAC)
      if (pool)
        pool->Recycle();
#endif
      TRACE_EVENT_NESTABLE_ASYNC_BEGIN0(
          "toplevel", "RendererMain.START_MSG_LOOP",
          TRACE_ID_WITH_SCOPE("RendererMain.START_MSG_LOOP", 0));
      run_loop.Run();
      TRACE_EVENT_NESTABLE_ASYNC_END0(
          "toplevel", "RendererMain.START_MSG_LOOP",
          TRACE_ID_WITH_SCOPE("RendererMain.START_MSG_LOOP", 0));
    }

#if defined(LEAK_SANITIZER)
    // Run leak detection before RenderProcessImpl goes out of scope. This helps
    // ignore shutdown-only leaks.
    __lsan_do_leak_check();
#endif
  }
  platform.PlatformUninitialize();
#if defined(USE_LTTNG)
  if (lttng_native_library)
    base::UnloadNativeLibrary(lttng_native_library);
#endif
  TRACE_EVENT_NESTABLE_ASYNC_END0("startup", "RendererMain",
                                  TRACE_ID_WITH_SCOPE("RendererMain", 0));
  return 0;
}

}  // namespace content
