// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/accessibility/accessibility_controller_impl.h"
#include "ash/accessibility/ui/accessibility_confirmation_dialog.h"
#include "ash/shell.h"
#include "chrome/browser/ash/accessibility/accessibility_manager.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/settings_window_manager_chromeos.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/web_applications/system_web_apps/system_web_app_manager.h"
#include "chrome/browser/web_applications/web_app_provider.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/test/browser_test.h"
#include "extensions/test/result_catcher.h"
#include "ui/accessibility/accessibility_features.h"
#include "ui/base/ui_base_features.h"

namespace extensions {

using ::ash::AccessibilityManager;

using ContextType = ExtensionBrowserTest::ContextType;

class AccessibilityPrivateApiTest
    : public ExtensionApiTest,
      public testing::WithParamInterface<ContextType> {
 public:
  AccessibilityPrivateApiTest() = default;
  ~AccessibilityPrivateApiTest() override = default;
  AccessibilityPrivateApiTest& operator=(const AccessibilityPrivateApiTest&) =
      delete;
  AccessibilityPrivateApiTest(const AccessibilityPrivateApiTest&) = delete;

 protected:
  bool RunSubtest(const char* subtest) WARN_UNUSED_RESULT {
    return RunExtensionTest(
        "accessibility_private", {.custom_arg = subtest},
        {.load_as_service_worker = GetParam() == ContextType::kServiceWorker});
  }
};

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, SendSyntheticKeyEvent) {
  ASSERT_TRUE(RunSubtest("testSendSyntheticKeyEvent")) << message_;
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest,
                       GetDisplayNameForLocaleTest) {
  ASSERT_TRUE(RunSubtest("testGetDisplayNameForLocale")) << message_;
}

// Flaky on a debug build, see crbug.com/1030507.
#if !defined(NDEBUG)
#define MAYBE_OpenSettingsSubpage DISABLED_OpenSettingsSubpage
#else
#define MAYBE_OpenSettingsSubpage OpenSettingsSubpage
#endif
IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, OpenSettingsSubpage) {
  Profile* profile = AccessibilityManager::Get()->profile();

  // Install the Settings App.
  web_app::WebAppProvider::Get(profile)
      ->system_web_app_manager()
      .InstallSystemAppsForTesting();

  ASSERT_TRUE(RunSubtest("testOpenSettingsSubpage")) << message_;

  chrome::SettingsWindowManager* settings_manager =
      chrome::SettingsWindowManager::GetInstance();

  Browser* settings_browser = settings_manager->FindBrowserForProfile(profile);
  EXPECT_NE(nullptr, settings_browser);

  content::WebContents* web_contents =
      settings_browser->tab_strip_model()->GetWebContentsAt(0);

  EXPECT_TRUE(WaitForLoadStop(web_contents));

  EXPECT_EQ(GURL(chrome::GetOSSettingsUrl("manageAccessibility/tts")),
            web_contents->GetLastCommittedURL());
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest,
                       OpenSettingsSubpage_InvalidSubpage) {
  Profile* profile = AccessibilityManager::Get()->profile();

  // Install the Settings App.
  web_app::WebAppProvider::Get(profile)
      ->system_web_app_manager()
      .InstallSystemAppsForTesting();

  ASSERT_TRUE(RunSubtest("testOpenSettingsSubpageInvalidSubpage")) << message_;

  chrome::SettingsWindowManager* settings_manager =
      chrome::SettingsWindowManager::GetInstance();

  // Invalid subpage should not open settings window.
  Browser* settings_browser = settings_manager->FindBrowserForProfile(profile);
  EXPECT_EQ(nullptr, settings_browser);
}

template <bool enabled>
class AccessibilityPrivateApiFeatureTest : public AccessibilityPrivateApiTest {
 public:
  AccessibilityPrivateApiFeatureTest() {
    if (enabled) {
      scoped_feature_list_.InitAndEnableFeature(
          ::features::kSelectToSpeakNavigationControl);
    } else {
      scoped_feature_list_.InitAndDisableFeature(
          ::features::kSelectToSpeakNavigationControl);
    }
  }
  ~AccessibilityPrivateApiFeatureTest() override = default;
  AccessibilityPrivateApiFeatureTest& operator=(
      const AccessibilityPrivateApiFeatureTest&) = delete;
  AccessibilityPrivateApiFeatureTest(
      const AccessibilityPrivateApiFeatureTest&) = delete;

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

using AccessibilityPrivateApiFeatureDisabledTest =
    AccessibilityPrivateApiFeatureTest<false>;
using AccessibilityPrivateApiFeatureEnabledTest =
    AccessibilityPrivateApiFeatureTest<true>;

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiFeatureDisabledTest,
                       IsFeatureEnabled_FeatureDisabled) {
  ASSERT_TRUE(RunSubtest("testFeatureDisabled")) << message_;
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiFeatureEnabledTest,
                       IsFeatureEnabled_FeatureEnabled) {
  ASSERT_TRUE(RunSubtest("testFeatureEnabled")) << message_;
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, IsFeatureUnknown) {
  ASSERT_TRUE(RunSubtest("testFeatureUnknown")) << message_;
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, AcceptConfirmationDialog) {
  ASSERT_TRUE(RunSubtest("testAcceptConfirmationDialog")) << message_;

  // The test has requested to open the confirmation dialog. Check that
  // it was created, then confirm it.
  ash::AccessibilityConfirmationDialog* dialog_ =
      ash::Shell::Get()
          ->accessibility_controller()
          ->GetConfirmationDialogForTest();
  ASSERT_NE(dialog_, nullptr);

  EXPECT_EQ(dialog_->GetWindowTitle(), u"Confirm me! 🐶");

  // Accept the dialog and wait for the JS test to get the confirmation.
  ResultCatcher catcher;
  dialog_->Accept();
  EXPECT_TRUE(catcher.GetNextResult()) << catcher.message();
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, CancelConfirmationDialog) {
  ASSERT_TRUE(RunSubtest("testCancelConfirmationDialog")) << message_;

  // The test has requested to open the confirmation dialog. Check that
  // it was created, then cancel it.
  ash::AccessibilityConfirmationDialog* dialog_ =
      ash::Shell::Get()
          ->accessibility_controller()
          ->GetConfirmationDialogForTest();
  ASSERT_NE(dialog_, nullptr);

  EXPECT_EQ(dialog_->GetWindowTitle(), u"Cancel me!");

  // Cancel the dialog and wait for the JS test to get the callback.
  ResultCatcher catcher;
  dialog_->Cancel();
  EXPECT_TRUE(catcher.GetNextResult()) << catcher.message();
}

IN_PROC_BROWSER_TEST_P(AccessibilityPrivateApiTest, CloseConfirmationDialog) {
  ASSERT_TRUE(RunSubtest("testCancelConfirmationDialog")) << message_;

  // The test has requested to open the confirmation dialog. Check that
  // it was created, then close it.
  ash::AccessibilityConfirmationDialog* dialog_ =
      ash::Shell::Get()
          ->accessibility_controller()
          ->GetConfirmationDialogForTest();
  ASSERT_TRUE(dialog_ != nullptr);

  EXPECT_EQ(dialog_->GetWindowTitle(), u"Cancel me!");

  // Close the dialog and wait for the JS test to get the callback.
  ResultCatcher catcher;
  dialog_->Close();
  EXPECT_TRUE(catcher.GetNextResult()) << catcher.message();
}

INSTANTIATE_TEST_SUITE_P(PersistentBackground,
                         AccessibilityPrivateApiTest,
                         ::testing::Values(ContextType::kPersistentBackground));
INSTANTIATE_TEST_SUITE_P(PersistentBackground,
                         AccessibilityPrivateApiFeatureDisabledTest,
                         ::testing::Values(ContextType::kPersistentBackground));
INSTANTIATE_TEST_SUITE_P(PersistentBackground,
                         AccessibilityPrivateApiFeatureEnabledTest,
                         ::testing::Values(ContextType::kPersistentBackground));
INSTANTIATE_TEST_SUITE_P(ServiceWorker,
                         AccessibilityPrivateApiTest,
                         ::testing::Values(ContextType::kServiceWorker));
INSTANTIATE_TEST_SUITE_P(ServiceWorker,
                         AccessibilityPrivateApiFeatureDisabledTest,
                         ::testing::Values(ContextType::kServiceWorker));
INSTANTIATE_TEST_SUITE_P(ServiceWorker,
                         AccessibilityPrivateApiFeatureEnabledTest,
                         ::testing::Values(ContextType::kServiceWorker));

}  // namespace extensions
