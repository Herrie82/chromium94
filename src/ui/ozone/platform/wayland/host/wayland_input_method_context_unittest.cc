// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <text-input-unstable-v1-server-protocol.h>
#include <wayland-server.h>
#include <memory>

#include "base/strings/utf_string_conversions.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/ime/linux/linux_input_method_context.h"
#include "ui/events/event.h"
#include "ui/gfx/range/range.h"
#include "ui/ozone/platform/wayland/host/wayland_input_method_context.h"
#include "ui/ozone/platform/wayland/host/wayland_input_method_context_factory.h"
#include "ui/ozone/platform/wayland/host/wayland_window.h"
#include "ui/ozone/platform/wayland/test/mock_surface.h"
#include "ui/ozone/platform/wayland/test/mock_zwp_text_input.h"
#include "ui/ozone/platform/wayland/test/test_wayland_server_thread.h"
#include "ui/ozone/platform/wayland/test/wayland_test.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Mock;
using ::testing::SaveArg;
using ::testing::Values;

namespace ui {

class TestInputMethodContextDelegate : public LinuxInputMethodContextDelegate {
 public:
  TestInputMethodContextDelegate() = default;
  TestInputMethodContextDelegate(const TestInputMethodContextDelegate&) =
      delete;
  TestInputMethodContextDelegate& operator=(
      const TestInputMethodContextDelegate&) = delete;
  ~TestInputMethodContextDelegate() override = default;

  void OnCommit(const std::u16string& text) override {
    was_on_commit_called_ = true;
  }
  void OnPreeditChanged(const ui::CompositionText& composition_text) override {
    was_on_preedit_changed_called_ = true;
  }
  void OnPreeditEnd() override {}
  void OnPreeditStart() override {}
  void OnDeleteSurroundingText(int32_t index, uint32_t length) override {
    on_delete_surrounding_text_range_ = gfx::Range(index, index + length);
  }

  bool was_on_commit_called() { return was_on_commit_called_; }

  bool was_on_preedit_changed_called() {
    return was_on_preedit_changed_called_;
  }

  absl::optional<gfx::Range> on_delete_surrounding_text_range() {
    return on_delete_surrounding_text_range_;
  }

 private:
  bool was_on_commit_called_ = false;
  bool was_on_preedit_changed_called_ = false;
  absl::optional<gfx::Range> on_delete_surrounding_text_range_;
};

class WaylandInputMethodContextTest : public WaylandTest {
 public:
  WaylandInputMethodContextTest() = default;
  WaylandInputMethodContextTest(const WaylandInputMethodContextTest&) = delete;
  WaylandInputMethodContextTest& operator=(
      const WaylandInputMethodContextTest&) = delete;

  void SetUp() override {
    WaylandTest::SetUp();

    Sync();

    input_method_context_delegate_ =
        std::make_unique<TestInputMethodContextDelegate>();

    WaylandInputMethodContextFactory factory(connection_.get());
    LinuxInputMethodContextFactory::SetInstance(&factory);

    input_method_context_ = factory.CreateWaylandInputMethodContext(
        input_method_context_delegate_.get(), false);
    input_method_context_->Init(true);
    connection_->ScheduleFlush();

    Sync();
    // Unset Keyboard focus.
    connection_->wayland_window_manager()->SetKeyboardFocusedWindow(nullptr);

    zwp_text_input_ = server_.text_input_manager_v1()->text_input();

    ASSERT_TRUE(connection_->text_input_manager_v1());
    ASSERT_TRUE(zwp_text_input_);
  }

 protected:
  std::unique_ptr<TestInputMethodContextDelegate>
      input_method_context_delegate_;
  std::unique_ptr<WaylandInputMethodContext> input_method_context_;
  wl::MockZwpTextInput* zwp_text_input_ = nullptr;
};

TEST_P(WaylandInputMethodContextTest, ActivateDeactivate) {
  // Activate is called only when both InputMethod's TextInputClient focus and
  // Wayland's keyboard focus is met.

  // Scenario 1: InputMethod focus is set, then Keyboard focus is set.
  // Unset them in the reversed order.
  EXPECT_CALL(*zwp_text_input_, Activate(surface_->resource())).Times(0);
  EXPECT_CALL(*zwp_text_input_, ShowInputPanel()).Times(0);
  input_method_context_->Focus();
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Activate(surface_->resource()));
  EXPECT_CALL(*zwp_text_input_, ShowInputPanel());
  connection_->wayland_window_manager()->SetKeyboardFocusedWindow(
      window_.get());
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Deactivate());
  EXPECT_CALL(*zwp_text_input_, HideInputPanel());
  connection_->wayland_window_manager()->SetKeyboardFocusedWindow(nullptr);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Deactivate()).Times(0);
  EXPECT_CALL(*zwp_text_input_, HideInputPanel()).Times(0);
  input_method_context_->Blur();
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  // Scenario 2: Keyboard focus is set, then InputMethod focus is set.
  // Unset them in the reversed order.
  EXPECT_CALL(*zwp_text_input_, Activate(surface_->resource())).Times(0);
  EXPECT_CALL(*zwp_text_input_, ShowInputPanel()).Times(0);
  connection_->wayland_window_manager()->SetKeyboardFocusedWindow(
      window_.get());
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Activate(surface_->resource()));
  EXPECT_CALL(*zwp_text_input_, ShowInputPanel());
  input_method_context_->Focus();
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Deactivate());
  EXPECT_CALL(*zwp_text_input_, HideInputPanel());
  input_method_context_->Blur();
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);

  EXPECT_CALL(*zwp_text_input_, Deactivate()).Times(0);
  EXPECT_CALL(*zwp_text_input_, HideInputPanel()).Times(0);
  connection_->wayland_window_manager()->SetKeyboardFocusedWindow(nullptr);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);
}

TEST_P(WaylandInputMethodContextTest, Reset) {
  EXPECT_CALL(*zwp_text_input_, Reset());
  input_method_context_->Reset();
  connection_->ScheduleFlush();
  Sync();
}

TEST_P(WaylandInputMethodContextTest, SetCursorLocation) {
  EXPECT_CALL(*zwp_text_input_, SetCursorRect(50, 0, 1, 1));
  input_method_context_->SetCursorLocation(gfx::Rect(50, 0, 1, 1));
  connection_->ScheduleFlush();
  Sync();
}

TEST_P(WaylandInputMethodContextTest, SetSurroundingTextForShortText) {
  std::u16string text(50, u'あ');
  gfx::Range range(20, 30);

  std::string sent_text;
  gfx::Range sent_range;
  EXPECT_CALL(*zwp_text_input_, SetSurroundingText(_, _))
      .WillOnce(DoAll(SaveArg<0>(&sent_text), SaveArg<1>(&sent_range)));
  input_method_context_->SetSurroundingText(text, range);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);
  // The text and range sent as wayland protocol must be same to the original
  // text and range where the original text is shorter than 4000 byte.
  EXPECT_EQ(sent_text, base::UTF16ToUTF8(text));
  EXPECT_EQ(sent_range, gfx::Range(60, 90));

  // Test OnDeleteSurroundingText with this input.
  zwp_text_input_v1_send_delete_surrounding_text(
      zwp_text_input_->resource(), sent_range.start(), sent_range.length());
  Sync();
  absl::optional<gfx::Range> index =
      input_method_context_delegate_->on_delete_surrounding_text_range();
  EXPECT_TRUE(index.has_value());
  EXPECT_EQ(index, range);
}

TEST_P(WaylandInputMethodContextTest, SetSurroundingTextForLongText) {
  std::u16string text(5000, u'あ');
  gfx::Range range(2800, 3200);

  std::string sent_text;
  gfx::Range sent_range;
  EXPECT_CALL(*zwp_text_input_, SetSurroundingText(_, _))
      .WillOnce(DoAll(SaveArg<0>(&sent_text), SaveArg<1>(&sent_range)));
  input_method_context_->SetSurroundingText(text, range);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);
  // The text sent as wayland protocol must be at most 4000 byte and long
  // enough in the limitation.
  EXPECT_EQ(sent_text.size(), 3996UL);
  EXPECT_EQ(sent_text, base::UTF16ToUTF8(std::u16string(1332, u'あ')));
  // The selection range must be relocated accordingly to the sent text.
  EXPECT_EQ(sent_range, gfx::Range(1398, 2598));

  // Test OnDeleteSurroundingText with this input.
  zwp_text_input_v1_send_delete_surrounding_text(
      zwp_text_input_->resource(), sent_range.start(), sent_range.length());
  Sync();
  absl::optional<gfx::Range> index =
      input_method_context_delegate_->on_delete_surrounding_text_range();
  EXPECT_TRUE(index.has_value());
  EXPECT_EQ(index, range);
}

TEST_P(WaylandInputMethodContextTest, SetSurroundingTextForLongTextInLeftEdge) {
  std::u16string text(5000, u'あ');
  gfx::Range range(0, 500);

  std::string sent_text;
  gfx::Range sent_range;
  EXPECT_CALL(*zwp_text_input_, SetSurroundingText(_, _))
      .WillOnce(DoAll(SaveArg<0>(&sent_text), SaveArg<1>(&sent_range)));
  input_method_context_->SetSurroundingText(text, range);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);
  // The text sent as wayland protocol must be at most 4000 byte and large
  // enough in the limitation.
  EXPECT_EQ(sent_text.size(), 3999UL);
  EXPECT_EQ(sent_text, base::UTF16ToUTF8(std::u16string(1333, u'あ')));
  // The selection range must be relocated accordingly to the sent text.
  EXPECT_EQ(sent_range, gfx::Range(0, 1500));

  // Test OnDeleteSurroundingText with this input.
  zwp_text_input_v1_send_delete_surrounding_text(
      zwp_text_input_->resource(), sent_range.start(), sent_range.length());
  Sync();
  absl::optional<gfx::Range> index =
      input_method_context_delegate_->on_delete_surrounding_text_range();
  EXPECT_TRUE(index.has_value());
  EXPECT_EQ(index, range);
}

TEST_P(WaylandInputMethodContextTest,
       SetSurroundingTextForLongTextInRightEdge) {
  std::u16string text(5000, u'あ');
  gfx::Range range(4500, 5000);

  std::string sent_text;
  gfx::Range sent_range;
  EXPECT_CALL(*zwp_text_input_, SetSurroundingText(_, _))
      .WillOnce(DoAll(SaveArg<0>(&sent_text), SaveArg<1>(&sent_range)));
  input_method_context_->SetSurroundingText(text, range);
  connection_->ScheduleFlush();
  Sync();
  Mock::VerifyAndClearExpectations(zwp_text_input_);
  // The text sent as wayland protocol must be at most 4000 byte and large
  // enough in the limitation.
  EXPECT_EQ(sent_text.size(), 3999UL);
  EXPECT_EQ(sent_text, base::UTF16ToUTF8(std::u16string(1333, u'あ')));
  // The selection range must be relocated accordingly to the sent text.
  EXPECT_EQ(sent_range, gfx::Range(2499, 3999));

  // Test OnDeleteSurroundingText with this input.
  zwp_text_input_v1_send_delete_surrounding_text(
      zwp_text_input_->resource(), sent_range.start(), sent_range.length());
  Sync();
  absl::optional<gfx::Range> index =
      input_method_context_delegate_->on_delete_surrounding_text_range();
  EXPECT_TRUE(index.has_value());
  EXPECT_EQ(index, range);
}

TEST_P(WaylandInputMethodContextTest, SetSurroundingTextForLongRange) {
  std::u16string text(5000, u'あ');
  gfx::Range range(1000, 4000);

  // set_surrounding_text request should be skipped when the selection range in
  // UTF8 form is longer than 4000 byte.
  EXPECT_CALL(*zwp_text_input_, SetSurroundingText(_, _)).Times(0);
  input_method_context_->SetSurroundingText(text, range);
  connection_->ScheduleFlush();
  Sync();
}

TEST_P(WaylandInputMethodContextTest, OnPreeditChanged) {
  zwp_text_input_v1_send_preedit_string(zwp_text_input_->resource(), 0,
                                        "PreeditString", "");
  Sync();
  EXPECT_TRUE(input_method_context_delegate_->was_on_preedit_changed_called());
}

TEST_P(WaylandInputMethodContextTest, OnCommit) {
  zwp_text_input_v1_send_commit_string(zwp_text_input_->resource(), 0,
                                       "CommitString");
  Sync();
  EXPECT_TRUE(input_method_context_delegate_->was_on_commit_called());
}

INSTANTIATE_TEST_SUITE_P(XdgVersionStableTest,
                         WaylandInputMethodContextTest,
                         Values(wl::ServerConfig{
                             .shell_version = wl::ShellVersion::kStable}));
INSTANTIATE_TEST_SUITE_P(XdgVersionV6Test,
                         WaylandInputMethodContextTest,
                         Values(wl::ServerConfig{
                             .shell_version = wl::ShellVersion::kV6}));

}  // namespace ui
