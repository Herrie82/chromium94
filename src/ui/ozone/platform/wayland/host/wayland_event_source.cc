// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/wayland/host/wayland_event_source.h"

#include <algorithm>
#include <memory>

#include "base/bind.h"
#include "base/check.h"
#include "base/containers/cxx20_erase.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/events/ozone/layout/keyboard_layout_engine.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/events/pointer_details.h"
#include "ui/events/types/event_type.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d_f.h"
#include "ui/ozone/platform/wayland/host/wayland_connection.h"
#include "ui/ozone/platform/wayland/host/wayland_event_watcher.h"
#include "ui/ozone/platform/wayland/host/wayland_keyboard.h"
#include "ui/ozone/platform/wayland/host/wayland_window.h"
#include "ui/ozone/platform/wayland/host/wayland_window_manager.h"

#if defined(OS_WEBOS)
#include "ui/events/ozone/evdev/touch_evdev_types.h"
#include "ui/gfx/sequential_id_generator.h"
#endif  // defined(OS_WEBOS)

namespace ui {

namespace {

bool HasAnyPointerButtonFlag(int flags) {
  return (flags & (EF_LEFT_MOUSE_BUTTON | EF_MIDDLE_MOUSE_BUTTON |
                   EF_RIGHT_MOUSE_BUTTON | EF_BACK_MOUSE_BUTTON |
                   EF_FORWARD_MOUSE_BUTTON)) != 0;
}

// Number of fingers for scroll gestures.
constexpr int kGestureScrollFingerCount = 2;

// Maximum size of the stored recent pointer frame information.
constexpr int kRecentPointerFrameMaxSize = 20;

#if defined(OS_WEBOS)
static SequentialIDGenerator g_touch_point_id_generator(0);

PointerId GetTouchPointID(int device_id, PointerId id) {
  // We need to make the touch point ID unique to system, not only to device.
  // Otherwise gesture recognizer touch lock will fail. To achieve this we copy
  // the algorithm used in EventFactoryEvdev.
  return g_touch_point_id_generator.GetGeneratedID(
      device_id * kNumTouchEvdevSlots + id);
}

void ReleaseTouchPointID(PointerId touch_point_id) {
  g_touch_point_id_generator.ReleaseNumber(touch_point_id);
}
#endif  // defined(OS_WEBOS)
}  // namespace

struct WaylandEventSource::TouchPoint {
  TouchPoint(gfx::PointF location, WaylandWindow* current_window);
  ~TouchPoint() = default;

  WaylandWindow* const window;
  gfx::PointF last_known_location;
};

WaylandEventSource::TouchPoint::TouchPoint(gfx::PointF location,
                                           WaylandWindow* current_window)
    : window(current_window), last_known_location(location) {
  DCHECK(window);
}

WaylandEventSource::PointerFrame::PointerFrame() = default;
WaylandEventSource::PointerFrame::PointerFrame(const PointerFrame&) = default;
WaylandEventSource::PointerFrame::PointerFrame(PointerFrame&&) = default;
WaylandEventSource::PointerFrame::~PointerFrame() = default;

WaylandEventSource::PointerFrame& WaylandEventSource::PointerFrame::operator=(
    const PointerFrame&) = default;
WaylandEventSource::PointerFrame& WaylandEventSource::PointerFrame::operator=(
    PointerFrame&&) = default;

// WaylandEventSource implementation

WaylandEventSource::WaylandEventSource(wl_display* display,
                                       wl_event_queue* event_queue,
                                       WaylandWindowManager* window_manager,
                                       WaylandConnection* connection)
    : window_manager_(window_manager),
      connection_(connection),
      event_watcher_(
          std::make_unique<WaylandEventWatcher>(display, event_queue)) {
  DCHECK(window_manager_);

  // Observes remove changes to know when touch points can be removed.
  window_manager_->AddObserver(this);
}

WaylandEventSource::~WaylandEventSource() = default;

void WaylandEventSource::SetShutdownCb(base::OnceCallback<void()> shutdown_cb) {
  event_watcher_->SetShutdownCb(std::move(shutdown_cb));
}

void WaylandEventSource::StartProcessingEvents() {
  event_watcher_->StartProcessingEvents();
}

void WaylandEventSource::StopProcessingEvents() {
  event_watcher_->StopProcessingEvents();
}

void WaylandEventSource::OnKeyboardFocusChanged(WaylandWindow* window,
                                                bool focused) {
  DCHECK(window);
#if DCHECK_IS_ON()
  if (!focused)
    DCHECK_EQ(window, window_manager_->GetCurrentKeyboardFocusedWindow());
#endif
  window_manager_->SetKeyboardFocusedWindow(focused ? window : nullptr);
}

void WaylandEventSource::OnKeyboardModifiersChanged(int modifiers) {
  keyboard_modifiers_ = modifiers;
}

uint32_t WaylandEventSource::OnKeyboardKeyEvent(
    EventType type,
    DomCode dom_code,
    bool repeat,
    base::TimeTicks timestamp,
    int device_id,
    WaylandKeyboard::KeyEventKind kind) {
  DCHECK(type == ET_KEY_PRESSED || type == ET_KEY_RELEASED);

  DomKey dom_key;
  KeyboardCode key_code;
  auto* layout_engine = KeyboardLayoutEngineManager::GetKeyboardLayoutEngine();
  if (!layout_engine || !layout_engine->Lookup(dom_code, keyboard_modifiers_,
                                               &dom_key, &key_code)) {
    LOG(ERROR) << "Failed to decode key event.";
    return POST_DISPATCH_NONE;
  }

  if (!repeat) {
    int flag = ModifierDomKeyToEventFlag(dom_key);
    UpdateKeyboardModifiers(flag, type == ET_KEY_PRESSED);
  }

  KeyEvent event(type, key_code, dom_code,
                 keyboard_modifiers_ | (repeat ? EF_IS_REPEAT : 0), dom_key,
                 timestamp);
  event.set_source_device_id(device_id);
  if (kind == WaylandKeyboard::KeyEventKind::kKey) {
    // Mark that this is the key event which IME did not consume.
    event.SetProperties({{
        kPropertyKeyboardImeFlag,
        std::vector<uint8_t>{kPropertyKeyboardImeIgnoredFlag},
    }});
  }
  return DispatchEvent(&event);
}

void WaylandEventSource::OnPointerFocusChanged(WaylandWindow* window,
                                               const gfx::PointF& location
#if defined(OS_WEBOS)
                                               ,
                                               int device_id
#endif  // defined(OS_WEBOS)
) {
  // Save new pointer location.
  pointer_location_ = location;

  bool focused = !!window;
  if (focused)
    window_manager_->SetPointerFocusedWindow(window);

  EventType type = focused ? ET_MOUSE_ENTERED : ET_MOUSE_EXITED;
  MouseEvent event(type, location, location, EventTimeForNow(), pointer_flags_,
                   0);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);

  if (!focused)
    window_manager_->SetPointerFocusedWindow(nullptr);
}

void WaylandEventSource::OnPointerButtonEvent(EventType type,
                                              int changed_button,
                                              WaylandWindow* window
#if defined(OS_WEBOS)
                                              ,
                                              int device_id
#endif  // defined(OS_WEBOS)
) {
  DCHECK(type == ET_MOUSE_PRESSED || type == ET_MOUSE_RELEASED);
  DCHECK(HasAnyPointerButtonFlag(changed_button));

  WaylandWindow* prev_focused_window =
      window_manager_->GetCurrentPointerFocusedWindow();
  if (window)
    window_manager_->SetPointerFocusedWindow(window);

  pointer_flags_ = type == ET_MOUSE_PRESSED
                       ? (pointer_flags_ | changed_button)
                       : (pointer_flags_ & ~changed_button);
  last_pointer_button_pressed_ = changed_button;
  // MouseEvent's flags should contain the button that was released too.
  int flags = pointer_flags_ | keyboard_modifiers_ | changed_button;
  MouseEvent event(type, pointer_location_, pointer_location_,
                   EventTimeForNow(), flags, changed_button);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);

  if (window)
    window_manager_->SetPointerFocusedWindow(prev_focused_window);
}

void WaylandEventSource::OnPointerMotionEvent(const gfx::PointF& location
#if defined(OS_WEBOS)
                                              ,
                                              int device_id
#endif  // defined(OS_WEBOS)
) {
  pointer_location_ = location;
  int flags = pointer_flags_ | keyboard_modifiers_;
  MouseEvent event(ET_MOUSE_MOVED, pointer_location_, pointer_location_,
                   EventTimeForNow(), flags, 0);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);
}

void WaylandEventSource::OnPointerAxisEvent(const gfx::Vector2dF& offset) {
  current_pointer_frame_.dx += offset.x();
  current_pointer_frame_.dy += offset.y();

#if defined(USE_NEVA_APPRUNTIME)
  // Workaround for LSM & weston 3.0.0 not emitting wl_pointer.axis_source event
  OnPointerAxisSourceEvent(WL_POINTER_AXIS_SOURCE_WHEEL);
#if defined(OS_WEBOS)
  // Workaround for LSM not emitting wl_pointer.frame event
  // See wayland core protocol wl_pointer v5 additions
  OnPointerFrameEvent();
#endif  // defined(OS_WEBOS)
#endif  // defined(USE_NEVA_APPRUNTIME)
}

void WaylandEventSource::OnResetPointerFlags() {
  ResetPointerFlags();
}

const gfx::PointF& WaylandEventSource::GetPointerLocation() const {
  return pointer_location_;
}

void WaylandEventSource::OnPointerFrameEvent() {
  base::TimeTicks now = EventTimeForNow();
  current_pointer_frame_.dt = now - last_pointer_frame_time_;
  last_pointer_frame_time_ = now;

  int flags = pointer_flags_ | keyboard_modifiers_;

  static constexpr bool supports_trackpad_kinetic_scrolling =
#if BUILDFLAG(IS_CHROMEOS_LACROS)
      true;
#else
      false;
#endif

  // Dispatch Fling event if pointer.axis_stop is notified and the recent
  // pointer.axis events meets the criteria to start fling scroll.
  if (current_pointer_frame_.dx == 0 && current_pointer_frame_.dy == 0 &&
      current_pointer_frame_.is_axis_stop &&
      supports_trackpad_kinetic_scrolling) {
    gfx::Vector2dF initial_velocity = ComputeFlingVelocity();
    float vx = initial_velocity.x();
    float vy = initial_velocity.y();
    ScrollEvent event(
        vx == 0 && vy == 0 ? ET_SCROLL_FLING_CANCEL : ET_SCROLL_FLING_START,
        pointer_location_, pointer_location_, now, flags, vx, vy, vx, vy,
        kGestureScrollFingerCount);
    DispatchEvent(&event);
    recent_pointer_frames_.clear();
  } else if (current_pointer_frame_.axis_source) {
    if (*current_pointer_frame_.axis_source == WL_POINTER_AXIS_SOURCE_WHEEL) {
      MouseWheelEvent event(
          gfx::Vector2d(current_pointer_frame_.dx, current_pointer_frame_.dy),
          pointer_location_, pointer_location_, EventTimeForNow(), flags, 0);
      DispatchEvent(&event);
    } else if (*current_pointer_frame_.axis_source ==
               WL_POINTER_AXIS_SOURCE_FINGER) {
      ScrollEvent event(ET_SCROLL, pointer_location_, pointer_location_,
                        EventTimeForNow(), flags, current_pointer_frame_.dx,
                        current_pointer_frame_.dy, current_pointer_frame_.dx,
                        current_pointer_frame_.dy, kGestureScrollFingerCount);
      DispatchEvent(&event);
    }

    if (recent_pointer_frames_.size() + 1 > kRecentPointerFrameMaxSize)
      recent_pointer_frames_.pop_back();
    recent_pointer_frames_.push_front(current_pointer_frame_);
  }

  // Reset |current_pointer_frame_|.
  current_pointer_frame_.dx = 0;
  current_pointer_frame_.dy = 0;
  current_pointer_frame_.is_axis_stop = false;
  current_pointer_frame_.axis_source.reset();
}

void WaylandEventSource::OnPointerAxisSourceEvent(uint32_t axis_source) {
  current_pointer_frame_.axis_source = axis_source;
}

void WaylandEventSource::OnPointerAxisStopEvent(uint32_t axis) {
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    current_pointer_frame_.dy = 0;
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    current_pointer_frame_.dx = 0;
  }
  current_pointer_frame_.is_axis_stop = true;
}

void WaylandEventSource::OnTouchPressEvent(WaylandWindow* window,
                                           const gfx::PointF& location,
                                           base::TimeTicks timestamp,
                                           PointerId id
#if defined(OS_WEBOS)
                                           ,
                                           int device_id
#endif  // defined(OS_WEBOS)
) {
  DCHECK(window);
  HandleTouchFocusChange(window, true);

#if defined(OS_WEBOS)
  id = GetTouchPointID(device_id, id);
#endif  // defined(OS_WEBOS)
  // Make sure this touch point wasn't present before.
  auto success = touch_points_.try_emplace(
      id, std::make_unique<TouchPoint>(location, window));
  if (!success.second) {
    LOG(WARNING) << "Touch down fired with wrong id";
    return;
  }

  PointerDetails details(EventPointerType::kTouch, id);
  TouchEvent event(ET_TOUCH_PRESSED, location, location, timestamp, details);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);
}

void WaylandEventSource::OnTouchReleaseEvent(base::TimeTicks timestamp,
                                             PointerId id
#if defined(OS_WEBOS)
                                             ,
                                             int device_id
#endif  // defined(OS_WEBOS)
) {
#if defined(OS_WEBOS)
  id = GetTouchPointID(device_id, id);
#endif  // defined(OS_WEBOS)
  // Make sure this touch point was present before.
  const auto it = touch_points_.find(id);
  if (it == touch_points_.end()) {
    LOG(WARNING) << "Touch up fired with no matching touch down";
    return;
  }

  TouchPoint* touch_point = it->second.get();
  gfx::PointF location = touch_point->last_known_location;
  PointerDetails details(EventPointerType::kTouch, id);

  TouchEvent event(ET_TOUCH_RELEASED, location, location, timestamp, details);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
  ReleaseTouchPointID(id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);

  HandleTouchFocusChange(touch_point->window, false, id);
  touch_points_.erase(it);
}

void WaylandEventSource::OnTouchMotionEvent(const gfx::PointF& location,
                                            base::TimeTicks timestamp,
                                            PointerId id
#if defined(OS_WEBOS)
                                            ,
                                            int device_id
#endif  // defined(OS_WEBOS)
) {
#if defined(OS_WEBOS)
  id = GetTouchPointID(device_id, id);
#endif  // defined(OS_WEBOS)
  const auto it = touch_points_.find(id);
  // Make sure this touch point was present before.
  if (it == touch_points_.end()) {
    LOG(WARNING) << "Touch event fired with wrong id";
    return;
  }
  it->second->last_known_location = location;
  PointerDetails details(EventPointerType::kTouch, id);
  TouchEvent event(ET_TOUCH_MOVED, location, location, timestamp, details);
#if defined(OS_WEBOS)
  event.set_source_device_id(device_id);
#endif  // defined(OS_WEBOS)
  DispatchEvent(&event);
}

void WaylandEventSource::OnTouchCancelEvent(
#if defined(OS_WEBOS)
    int device_id
#endif  // defined(OS_WEBOS)
) {
  // Some compositors emit a TouchCancel event when a drag'n drop
  // session is started on the server, eg Exo.
  // On Chrome, this event would actually abort the whole drag'n drop
  // session on the client side.
  if (connection_->IsDragInProgress())
    return;

  gfx::PointF location;
  base::TimeTicks timestamp = base::TimeTicks::Now();
  for (auto& touch_point : touch_points_) {
#if defined(OS_WEBOS)
    if (touch_point.second->window->touch_device_id() == device_id) {
#endif  // defined(OS_WEBOS)
      PointerId id = touch_point.first;
      TouchEvent event(ET_TOUCH_CANCELLED, location, location, timestamp,
                       PointerDetails(EventPointerType::kTouch, id));
#if defined(OS_WEBOS)
      event.set_source_device_id(device_id);
      ReleaseTouchPointID(id);
#endif  // defined(OS_WEBOS)
      DispatchEvent(&event);
      HandleTouchFocusChange(touch_point.second->window, false);
#if defined(OS_WEBOS)
    }
#endif  // defined(OS_WEBOS)
  }
#if defined(OS_WEBOS)
  base::EraseIf(touch_points_, [device_id](const auto& point) {
    return point.second->window->touch_device_id() == device_id;
  });
#else   // !defined(OS_WEBOS)
  touch_points_.clear();
#endif  // defined(OS_WEBOS)
}

std::vector<PointerId> WaylandEventSource::GetActiveTouchPointIds() {
  std::vector<PointerId> pointer_ids;
  for (auto& touch_point : touch_points_)
    pointer_ids.push_back(touch_point.first);
  return pointer_ids;
}

void WaylandEventSource::OnPinchEvent(EventType event_type,
                                      const gfx::Vector2dF& delta,
                                      base::TimeTicks timestamp,
                                      int device_id,
                                      absl::optional<float> scale) {
  GestureEventDetails details(event_type);
  details.set_device_type(GestureDeviceType::DEVICE_TOUCHPAD);
  if (scale)
    details.set_scale(*scale);

  auto location = pointer_location_ + delta;
  GestureEvent event(location.x(), location.y(), 0 /* flags */, timestamp,
                     details);
  event.set_source_device_id(device_id);
  DispatchEvent(&event);
}

void WaylandEventSource::SetRelativePointerMotionEnabled(bool enabled) {
  if (enabled)
    relative_pointer_location_ = pointer_location_;
  else
    relative_pointer_location_.reset();
}

void WaylandEventSource::OnRelativePointerMotion(const gfx::Vector2dF& delta) {
  DCHECK(relative_pointer_location_.has_value());

  relative_pointer_location_ = *relative_pointer_location_ + delta;
  OnPointerMotionEvent(*relative_pointer_location_);
}

bool WaylandEventSource::IsPointerButtonPressed(EventFlags button) const {
  DCHECK(HasAnyPointerButtonFlag(button));
  return pointer_flags_ & button;
}

void WaylandEventSource::ResetPointerFlags() {
  pointer_flags_ = 0;
}

void WaylandEventSource::UseSingleThreadedPollingForTesting() {
  event_watcher_->UseSingleThreadedPollingForTesting();
}

void WaylandEventSource::OnDispatcherListChanged() {
  StartProcessingEvents();
}

void WaylandEventSource::OnWindowRemoved(WaylandWindow* window) {
  // Clear touch-related data.
  base::EraseIf(touch_points_, [window](const auto& point) {
    return point.second->window == window;
  });
}

// Currently EF_MOD3_DOWN means that the CapsLock key is currently down, and
// EF_CAPS_LOCK_ON means the caps lock state is enabled (and the key may or
// may not be down, but usually isn't). There does need to be two different
// flags, since the physical CapsLock key is subject to remapping, but the
// caps lock state (which can be triggered in a variety of ways) is not.
//
// TODO(crbug.com/1076661): This is likely caused by some CrOS-specific code.
// Get rid of this function once it is properly guarded under OS_CHROMEOS.
void WaylandEventSource::UpdateKeyboardModifiers(int modifier, bool down) {
  if (modifier == EF_NONE)
    return;

  if (modifier == EF_CAPS_LOCK_ON) {
    modifier = (modifier & ~EF_CAPS_LOCK_ON) | EF_MOD3_DOWN;
  }
  keyboard_modifiers_ = down ? (keyboard_modifiers_ | modifier)
                             : (keyboard_modifiers_ & ~modifier);
}

void WaylandEventSource::HandleTouchFocusChange(WaylandWindow* window,
                                                bool focused,
                                                absl::optional<PointerId> id) {
  DCHECK(window);
  bool actual_focus = id ? !ShouldUnsetTouchFocus(window, id.value()) : focused;
  window->set_touch_focus(actual_focus);
}

// Focus must not be unset if there is another touch point within |window|.
bool WaylandEventSource::ShouldUnsetTouchFocus(WaylandWindow* win,
                                               PointerId id) {
  auto result = std::find_if(
      touch_points_.begin(), touch_points_.end(),
      [win, id](auto& p) { return p.second->window == win && p.first != id; });
  return result == touch_points_.end();
}

gfx::Vector2dF WaylandEventSource::ComputeFlingVelocity() {
  // Return average velocity in the last 200ms.
  // TODO(fukino): Make the formula similar to libgestures's
  // RegressScrollVelocity(). crbug.com/1129263.
  base::TimeDelta dt;
  float dx = 0.0f;
  float dy = 0.0f;
  for (auto& frame : recent_pointer_frames_) {
    if (frame.axis_source &&
        *frame.axis_source != WL_POINTER_AXIS_SOURCE_FINGER) {
      break;
    }
    if (frame.dx == 0 && frame.dy == 0)
      break;
    if (dt + frame.dt > base::TimeDelta::FromMilliseconds(200))
      break;

    dx += frame.dx;
    dy += frame.dy;
    dt += frame.dt;
  }
  float dt_inv = 1.0f / dt.InSecondsF();
  return dt.is_zero() ? gfx::Vector2dF()
                      : gfx::Vector2dF(dx * dt_inv, dy * dt_inv);
}

}  // namespace ui
