// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/open_from_clipboard/clipboard_recent_content_generic.h"

#include <memory>
#include <string>
#include <utility>

#include "base/cxx17_backports.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "url/gurl.h"

namespace {

class HasDataCallbackWaiter {
 public:
  explicit HasDataCallbackWaiter(ClipboardRecentContentGeneric* recent_content)
      : received_(false) {
    std::set<ClipboardContentType> desired_types = {
        ClipboardContentType::URL, ClipboardContentType::Text,
        ClipboardContentType::Image};

    recent_content->HasRecentContentFromClipboard(
        desired_types, base::BindOnce(&HasDataCallbackWaiter::OnComplete,
                                      weak_ptr_factory_.GetWeakPtr()));
  }

  void WaitForCallbackDone() {
    if (received_)
      return;

    base::RunLoop run_loop;
    quit_closure_ = run_loop.QuitClosure();
    run_loop.Run();
  }

  std::set<ClipboardContentType> GetContentType() { return result; }

 private:
  void OnComplete(std::set<ClipboardContentType> matched_types) {
    result = std::move(matched_types);
    received_ = true;
    if (quit_closure_)
      std::move(quit_closure_).Run();
  }

  base::OnceClosure quit_closure_;
  bool received_;
  std::set<ClipboardContentType> result;

  base::WeakPtrFactory<HasDataCallbackWaiter> weak_ptr_factory_{this};
};

}  // namespace

class ClipboardRecentContentGenericTest : public testing::Test {
 protected:
  void SetUp() override {
    test_clipboard_ = ui::TestClipboard::CreateForCurrentThread();
  }

  void TearDown() override {
    ui::Clipboard::DestroyClipboardForCurrentThread();
  }

  ui::TestClipboard* test_clipboard_;
};

TEST_F(ClipboardRecentContentGenericTest, RecognizesURLs) {
  struct {
    std::string clipboard;
    const bool expected_get_recent_url_value;
  } test_data[] = {
      {"www", false},
      {"query string", false},
      {"www.example.com", false},
      {"http://www.example.com/", true},
      // The missing trailing slash shouldn't matter.
      {"http://www.example.com", true},
      {"https://another-example.com/", true},
      {"http://example.com/with-path/", true},
      {"about:version", true},
      {"data:,Hello%2C%20World!", true},
      // Certain schemes are not eligible to be suggested.
      {"ftp://example.com/", false},
      // Leading and trailing spaces are okay, other spaces not.
      {"  http://leading.com", true},
      {" http://both.com/trailing  ", true},
      {"http://malformed url", false},
      {"http://another.com/malformed url", false},
      // Internationalized domain names should work.
      {"http://xn--c1yn36f", true},
      {" http://xn--c1yn36f/path   ", true},
      {"http://xn--c1yn36f extra ", false},
      {"http://點看", true},
      {"http://點看/path", true},
      {"  http://點看/path ", true},
      {" http://點看/path extra word", false},
  };

  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  for (size_t i = 0; i < base::size(test_data); ++i) {
    test_clipboard_->WriteText(test_data[i].clipboard.data(),
                               test_data[i].clipboard.length());
    test_clipboard_->SetLastModifiedTime(now -
                                         base::TimeDelta::FromSeconds(10));
    EXPECT_EQ(test_data[i].expected_get_recent_url_value,
              recent_content.GetRecentURLFromClipboard().has_value())
        << "for input " << test_data[i].clipboard;
  }
}

TEST_F(ClipboardRecentContentGenericTest, OlderURLsNotSuggested) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = "http://example.com/";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromMinutes(9));
  EXPECT_TRUE(recent_content.GetRecentURLFromClipboard().has_value());
  // If the last modified time is 10 minutes ago, the URL shouldn't be
  // suggested.
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromMinutes(11));
  EXPECT_FALSE(recent_content.GetRecentURLFromClipboard().has_value());
}

TEST_F(ClipboardRecentContentGenericTest, GetClipboardContentAge) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = " whether URL or not should not matter here.";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(32));
  base::TimeDelta age = recent_content.GetClipboardContentAge();
  // It's possible the GetClipboardContentAge() took some time, so allow a
  // little slop (5 seconds) in this comparison; don't check for equality.
  EXPECT_LT(age - base::TimeDelta::FromSeconds(32),
            base::TimeDelta::FromSeconds(5));
}

TEST_F(ClipboardRecentContentGenericTest, SuppressClipboardContent) {
  // Make sure the URL is suggested.
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = "http://example.com/";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));
  EXPECT_TRUE(recent_content.GetRecentURLFromClipboard().has_value());
  EXPECT_TRUE(recent_content.GetRecentTextFromClipboard().has_value());
  EXPECT_FALSE(recent_content.HasRecentImageFromClipboard());

  // After suppressing it, it shouldn't be suggested.
  recent_content.SuppressClipboardContent();
  EXPECT_FALSE(recent_content.GetRecentURLFromClipboard().has_value());

  // If the clipboard changes, even if to the same thing again, the content
  // should be suggested again.
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now);
  EXPECT_TRUE(recent_content.GetRecentURLFromClipboard().has_value());
  EXPECT_TRUE(recent_content.GetRecentTextFromClipboard().has_value());
  EXPECT_FALSE(recent_content.HasRecentImageFromClipboard());
}

TEST_F(ClipboardRecentContentGenericTest, GetRecentTextFromClipboard) {
  // Make sure the Text is suggested.
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = "  Foo Bar   ";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));
  EXPECT_TRUE(recent_content.GetRecentTextFromClipboard().has_value());
  EXPECT_FALSE(recent_content.GetRecentURLFromClipboard().has_value());
  EXPECT_FALSE(recent_content.HasRecentImageFromClipboard());
  EXPECT_STREQ(
      "Foo Bar",
      base::UTF16ToUTF8(recent_content.GetRecentTextFromClipboard().value())
          .c_str());
}

TEST_F(ClipboardRecentContentGenericTest, ClearClipboardContent) {
  // Make sure the URL is suggested.
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = "http://example.com/";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));
  EXPECT_TRUE(recent_content.GetRecentURLFromClipboard().has_value());

  // After clear it, it shouldn't be suggested.
  recent_content.ClearClipboardContent();
  EXPECT_FALSE(recent_content.GetRecentURLFromClipboard().has_value());

  // If the clipboard changes, even if to the same thing again, the content
  // should be suggested again.
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now);
  EXPECT_TRUE(recent_content.GetRecentURLFromClipboard().has_value());
}

TEST_F(ClipboardRecentContentGenericTest, HasRecentImageFromClipboard) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  SkBitmap bitmap;
  bitmap.allocN32Pixels(3, 2);
  bitmap.eraseARGB(255, 0, 255, 0);

  EXPECT_FALSE(recent_content.HasRecentImageFromClipboard());
  test_clipboard_->WriteBitmap(bitmap);
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));
  EXPECT_TRUE(recent_content.HasRecentImageFromClipboard());
  EXPECT_FALSE(recent_content.GetRecentURLFromClipboard().has_value());
  EXPECT_FALSE(recent_content.GetRecentTextFromClipboard().has_value());
}

TEST_F(ClipboardRecentContentGenericTest, HasRecentContentFromClipboard_URL) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string title = "foo";
  std::string url_text = "http://example.com/";
#if defined(OS_LINUX) || defined(OS_CHROMEOS)
  // The linux and chromeos clipboard treats the presence of text on the
  // clipboard as the url format being available.
  test_clipboard_->WriteText(url_text.data(), url_text.length());
#else
  test_clipboard_->WriteBookmark(title.data(), title.length(), url_text.data(),
                                 url_text.length());
#endif  // defined(OS_LINUX) || defined(OS_CHROMEOS)
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));

  HasDataCallbackWaiter waiter(&recent_content);
  waiter.WaitForCallbackDone();
  std::set<ClipboardContentType> types = waiter.GetContentType();

  EXPECT_TRUE(types.find(ClipboardContentType::URL) != types.end());
}

TEST_F(ClipboardRecentContentGenericTest, HasRecentContentFromClipboard_Text) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  std::string text = "  Foo Bar   ";
  test_clipboard_->WriteText(text.data(), text.length());
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));

  HasDataCallbackWaiter waiter(&recent_content);
  waiter.WaitForCallbackDone();
  std::set<ClipboardContentType> types = waiter.GetContentType();

  EXPECT_TRUE(types.find(ClipboardContentType::Text) != types.end());
}

TEST_F(ClipboardRecentContentGenericTest, HasRecentContentFromClipboard_Image) {
  ClipboardRecentContentGeneric recent_content;
  base::Time now = base::Time::Now();
  SkBitmap bitmap;
  bitmap.allocN32Pixels(3, 2);
  bitmap.eraseARGB(255, 0, 255, 0);
  test_clipboard_->WriteBitmap(bitmap);
  test_clipboard_->SetLastModifiedTime(now - base::TimeDelta::FromSeconds(10));

  HasDataCallbackWaiter waiter(&recent_content);
  waiter.WaitForCallbackDone();
  std::set<ClipboardContentType> types = waiter.GetContentType();

  EXPECT_TRUE(types.find(ClipboardContentType::Image) != types.end());
}
