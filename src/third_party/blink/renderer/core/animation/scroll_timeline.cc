// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/animation/scroll_timeline.h"

#include <tuple>

#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_scroll_timeline_options.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_csskeywordvalue_cssnumericvalue_scrolltimelineelementbasedoffset_string.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_cssnumericvalue_double.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_union_double_scrolltimelineautokeyword.h"
#include "third_party/blink/renderer/core/animation/scroll_timeline_offset.h"
#include "third_party/blink/renderer/core/animation/scroll_timeline_util.h"
#include "third_party/blink/renderer/core/css/css_to_length_conversion_data.h"
#include "third_party/blink/renderer/core/css/cssom/css_unit_values.h"
#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/parser/css_tokenizer.h"
#include "third_party/blink/renderer/core/css/properties/css_parsing_utils.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/compositing/paint_layer_compositor.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/scroll/scroll_types.h"
#include "third_party/blink/renderer/platform/geometry/length_functions.h"

namespace blink {

namespace {

using ScrollTimelineSet =
    HeapHashMap<WeakMember<Node>,
                Member<HeapHashSet<WeakMember<ScrollTimeline>>>>;
ScrollTimelineSet& GetScrollTimelineSet() {
  DEFINE_STATIC_LOCAL(Persistent<ScrollTimelineSet>, set,
                      (MakeGarbageCollected<ScrollTimelineSet>()));
  return *set;
}

using ActiveScrollTimelineSet = HeapHashCountedSet<WeakMember<Node>>;
ActiveScrollTimelineSet& GetActiveScrollTimelineSet() {
  DEFINE_STATIC_LOCAL(Persistent<ActiveScrollTimelineSet>, set,
                      (MakeGarbageCollected<ActiveScrollTimelineSet>()));
  return *set;
}

bool StringToScrollDirection(String scroll_direction,
                             ScrollTimeline::ScrollDirection& result) {
  if (scroll_direction == "block") {
    result = ScrollTimeline::Block;
    return true;
  }
  if (scroll_direction == "inline") {
    result = ScrollTimeline::Inline;
    return true;
  }
  if (scroll_direction == "horizontal") {
    result = ScrollTimeline::Horizontal;
    return true;
  }
  if (scroll_direction == "vertical") {
    result = ScrollTimeline::Vertical;
    return true;
  }
  return false;
}

ScrollOrientation ToPhysicalScrollOrientation(
    ScrollTimeline::ScrollDirection direction,
    const LayoutBox& source_box) {
  bool is_horizontal = source_box.IsHorizontalWritingMode();
  switch (direction) {
    case ScrollTimeline::Block:
      return is_horizontal ? kVerticalScroll : kHorizontalScroll;
    case ScrollTimeline::Inline:
      return is_horizontal ? kHorizontalScroll : kVerticalScroll;
    case ScrollTimeline::Horizontal:
      return kHorizontalScroll;
    case ScrollTimeline::Vertical:
      return kVerticalScroll;
  }
}

Node* ResolveScrollSource(Element* scroll_source) {
  if (scroll_source &&
      scroll_source ==
          scroll_source->GetDocument().ScrollingElementNoLayout()) {
    return &scroll_source->GetDocument();
  }
  return scroll_source;
}

}  // namespace

ScrollTimeline* ScrollTimeline::Create(Document& document,
                                       ScrollTimelineOptions* options,
                                       ExceptionState& exception_state) {
  absl::optional<Element*> scroll_source =
      options->hasScrollSource() ? absl::make_optional(options->scrollSource())
                                 : absl::nullopt;

  ScrollDirection orientation;
  if (!StringToScrollDirection(options->orientation(), orientation)) {
    exception_state.ThrowDOMException(DOMExceptionCode::kNotSupportedError,
                                      "Invalid orientation");
    return nullptr;
  }

  HeapVector<Member<ScrollTimelineOffset>> scroll_offsets;
  // https://drafts.csswg.org/scroll-animations-1/#set-the-offset-value
  for (auto& offset : options->scrollOffsets()) {
    ScrollTimelineOffset* scroll_offset = ScrollTimelineOffset::Create(offset);
    if (!scroll_offset) {
      exception_state.ThrowTypeError("Invalid scroll offset");
      return nullptr;
    }
    // 2.1 If val is a CSSKeywordValue and matches the grammar auto and pos
    // equals to 0 or size - 1: Return val.
    unsigned int pos = scroll_offsets.size();
    if (scroll_offset->IsDefaultValue() &&
        !(pos == 0 || pos == (options->scrollOffsets().size() - 1))) {
      exception_state.ThrowTypeError(
          "Invalid scrollOffsets: 'auto' can only be set as start or end "
          "offset");
      return nullptr;
    }
    scroll_offsets.push_back(scroll_offset);
  }

  absl::optional<double> time_range;
  if (options->timeRange()->IsDouble()) {
    time_range = absl::make_optional(options->timeRange()->GetAsDouble());
  }

  // The scrollingElement depends on style/layout-tree in quirks mode. Update
  // such that subsequent calls to ScrollingElementNoLayout returns up-to-date
  // information.
  if (document.InQuirksMode())
    document.UpdateStyleAndLayoutTree();

  return MakeGarbageCollected<ScrollTimeline>(
      &document, scroll_source, orientation, scroll_offsets, time_range);
}

ScrollTimeline::ScrollTimeline(
    Document* document,
    absl::optional<Element*> scroll_source,
    ScrollDirection orientation,
    HeapVector<Member<ScrollTimelineOffset>> scroll_offsets,
    absl::optional<double> time_range)
    : AnimationTimeline(document),
      scroll_source_(
          scroll_source.value_or(document->ScrollingElementNoLayout())),
      resolved_scroll_source_(ResolveScrollSource(scroll_source_)),
      orientation_(orientation),
      scroll_offsets_(std::move(scroll_offsets)),
      time_range_(time_range) {
  if (resolved_scroll_source_) {
    ScrollTimelineSet& set = GetScrollTimelineSet();
    if (!set.Contains(resolved_scroll_source_)) {
      set.insert(
          resolved_scroll_source_,
          MakeGarbageCollected<HeapHashSet<WeakMember<ScrollTimeline>>>());
    }
    auto it = set.find(resolved_scroll_source_);
    it->value->insert(this);
  }
  SnapshotState();
}

bool ScrollTimeline::IsActive() const {
  return timeline_state_snapshotted_.phase != TimelinePhase::kInactive;
}

void ScrollTimeline::Invalidate() {
  ScheduleNextServiceInternal(/* time_check = */ false);
}

bool ScrollTimeline::ComputeIsActive() const {
  LayoutBox* layout_box = resolved_scroll_source_
                              ? resolved_scroll_source_->GetLayoutBox()
                              : nullptr;
  return layout_box && layout_box->IsScrollContainer();
}

ScrollTimelineOffset* ScrollTimeline::StartScrollOffset() const {
  // Single entry offset in scrollOffsets is considered as 'end'. Thus,
  // resolving start offset only if there is at least 2 offsets.
  return scroll_offsets_.size() >= 2 ? scroll_offsets_.at(0) : nullptr;
}
ScrollTimelineOffset* ScrollTimeline::EndScrollOffset() const {
  // End offset is always the last offset in scrollOffsets if exists.
  return scroll_offsets_.size() >= 1
             ? scroll_offsets_.at(scroll_offsets_.size() - 1)
             : nullptr;
}

const std::vector<double> ScrollTimeline::GetResolvedScrollOffsets() const {
  std::vector<double> resolved_offsets;
  for (const auto& offset : timeline_state_snapshotted_.scroll_offsets)
    resolved_offsets.push_back(offset);
  return resolved_offsets;
}

// Resolves scroll offsets and stores them into resolved_offsets argument.
// Returns true if the offsets are resolved.
// https://drafts.csswg.org/scroll-animations-1/#effective-scroll-offsets-algorithm
bool ScrollTimeline::ResolveScrollOffsets(
    WTF::Vector<double>& resolved_offsets) const {
  // 1. Let effective scroll offsets be an empty list of effective scroll
  // offsets.
  DCHECK(resolved_offsets.IsEmpty());
  DCHECK(ComputeIsActive());
  LayoutBox* layout_box = resolved_scroll_source_->GetLayoutBox();
  DCHECK(layout_box);

  double current_offset;
  double max_offset;
  GetCurrentAndMaxOffset(layout_box, current_offset, max_offset);

  auto orientation = ToPhysicalScrollOrientation(orientation_, *layout_box);

  // 2. Let first offset be true.
  // first_offset signifies weather min or max scroll offset is pushed to
  // effective scroll offsets.
  bool first_offset = true;

  // 3. If scrollOffsets is empty
  if (scroll_offsets_.size() == 0) {
    // Start and end offsets resolve to 'auto'.
    // 3.1 Run the procedure to resolve a scroll timeline offset for auto with
    // the is first flag set to first offset and add the resulted value into
    // effective scroll offsets.
    resolved_offsets.push_back(0);
    // 3.2 Set first offset to false.
    // 3.3 Run the procedure to resolve a scroll timeline offset for auto with
    // the is first flag set to first offset and add the resulted value into
    // effective scroll offsets.
    resolved_offsets.push_back(max_offset);
    return true;
  }
  // Single entry offset in scrollOffsets is considered as 'end'.
  // 4. If scrollOffsets has exactly one element
  if (scroll_offsets_.size() == 1) {
    // 4.1 Run the procedure to resolve a scroll timeline offset for auto with
    // the is first flag set to first offset and add the resulted value into
    // effective scroll offsets.
    resolved_offsets.push_back(0);
    // 4.2 Set first offset to false.
    first_offset = false;
  }

  // 5. For each scroll offset in the list of scrollOffsets, perform the
  // following steps:
  for (auto& offset : scroll_offsets_) {
    // 5.1 Let effective offset be the result of applying the procedure to
    // resolve a scroll timeline offset for scroll offset with the is first flag
    // set to first offset.
    auto resolved_offset =
        offset->ResolveOffset(resolved_scroll_source_, orientation, max_offset,
                              first_offset ? 0 : max_offset);
    if (!resolved_offset) {
      // 5.2 If effective offset is null, the effective scroll offsets is empty
      // and abort the remaining steps.
      resolved_offsets.clear();
      return false;
    }
    // 5.3 Add effective offset into effective scroll offsets.
    resolved_offsets.push_back(resolved_offset.value());

    // 5.4 Set first offset to false.
    first_offset = false;
  }
  DCHECK_GE(resolved_offsets.size(), 2u);
  // 6. Return effective scroll offsets.
  return true;
}

AnimationTimeline::PhaseAndTime ScrollTimeline::CurrentPhaseAndTime() {
  return {timeline_state_snapshotted_.phase,
          timeline_state_snapshotted_.current_time};
}

bool ScrollTimeline::ScrollOffsetsEqual(
    const HeapVector<Member<ScrollTimelineOffset>>& other) const {
  if (scroll_offsets_.size() != other.size())
    return false;
  wtf_size_t size = scroll_offsets_.size();
  for (wtf_size_t i = 0; i < size; ++i) {
    if (!DataEquivalent(scroll_offsets_.at(i), other.at(i)))
      return false;
  }
  return true;
}

V8CSSNumberish* ScrollTimeline::ConvertTimeToProgress(
    AnimationTimeDelta time) const {
  return MakeGarbageCollected<V8CSSNumberish>(
      CSSUnitValues::percent((time / GetDuration().value()) * 100));
}

V8CSSNumberish* ScrollTimeline::currentTime() {
  // time returns either in milliseconds or a 0 to 100 value representing the
  // progress of the timeline
  auto current_time = timeline_state_snapshotted_.current_time;

  // TODO(crbug.com/1140602): Support progress based animations
  // We are currently abusing the intended use of the "auto" keyword. We are
  // using it here as a signal to use progress based timeline instead of having
  // a range based current time.
  // We are doing this to maintain backwards compatibility with existing tests.
  if (time_range_) {
    // not using progress based, return time as double
    if (current_time) {
      return MakeGarbageCollected<V8CSSNumberish>(
          current_time->InMillisecondsF());
    }
    return nullptr;
  } else {
    if (current_time) {
      return ConvertTimeToProgress(AnimationTimeDelta(current_time.value()));
    }
    return nullptr;
  }
}

V8CSSNumberish* ScrollTimeline::duration() {
  // TODO (crbug.com/1216655): Time range should be removed from ScrollTimeline.
  // Currently still left in for the sake of backwards compatibility with
  // existing tests.
  if (time_range_) {
    return MakeGarbageCollected<V8CSSNumberish>(time_range_.value());
  }
  return MakeGarbageCollected<V8CSSNumberish>(CSSUnitValues::percent(100));
}

// https://drafts.csswg.org/scroll-animations-1/#current-time-algorithm
ScrollTimeline::TimelineState ScrollTimeline::ComputeTimelineState() const {
  // 1. If scroll timeline is inactive, return an unresolved time value.
  // https://github.com/WICG/scroll-animations/issues/31
  // https://wicg.github.io/scroll-animations/#current-time-algorithm
  WTF::Vector<double> resolved_offsets;
  if (!ComputeIsActive()) {
    return {TimelinePhase::kInactive, /*current_time*/ absl::nullopt,
            resolved_offsets};
  }
  LayoutBox* layout_box = resolved_scroll_source_->GetLayoutBox();
  // 2. Otherwise, let current scroll offset be the current scroll offset of
  // scrollSource in the direction specified by orientation.

  double current_offset;
  double max_offset;
  GetCurrentAndMaxOffset(layout_box, current_offset, max_offset);

  bool resolved = ResolveScrollOffsets(resolved_offsets);

  if (!resolved) {
    DCHECK(resolved_offsets.IsEmpty());
    return {TimelinePhase::kInactive, /*current_time*/ absl::nullopt,
            resolved_offsets};
  }

  double start_offset = resolved_offsets[0];
  double end_offset = resolved_offsets[resolved_offsets.size() - 1];

  // TODO(crbug.com/1060384): Once the spec has been updated to state what the
  // expected result is when startScrollOffset >= endScrollOffset, we might need
  // to add a special case here. See
  // https://github.com/WICG/scroll-animations/issues/20

  // 3. The current time is the result corresponding to the first matching
  // condition from below:
  // 3.1 If current scroll offset is less than effective start offset:
  //     The current time is 0.
  if (current_offset < start_offset) {
    return {TimelinePhase::kBefore, base::TimeDelta(), resolved_offsets};
  }

  // TODO (crbug.com/1216655): Time range should be removed from ScrollTimeline.
  // Currently still left in for the sake of backwards compatibility with
  // existing tests.
  base::TimeDelta duration =
      time_range_ ? base::TimeDelta::FromMillisecondsD(time_range_.value())
                  : base::TimeDelta::FromSecondsD(GetDuration()->InSecondsF());

  // 3.2 If current scroll offset is greater than or equal to effective end
  // offset:
  //    The current time is the effective time range.
  if (current_offset >= end_offset) {
    // If end_offset is greater than or equal to the maximum scroll offset of
    // scrollSource in orientation then return active phase, otherwise return
    // after phase.
    TimelinePhase phase = end_offset >= max_offset ? TimelinePhase::kActive
                                                   : TimelinePhase::kAfter;
    return {phase, duration, resolved_offsets};
  }

  // 3.3 Otherwise,
  // 3.3.1 Let progress be a result of applying calculate scroll timeline
  // progress procedure for current scroll offset.
  // 3.3.2 The current time is the result of evaluating the following
  // expression:
  //     progress × effective time range
  absl::optional<base::TimeDelta> calculated_current_time =
      base::TimeDelta::FromMillisecondsD(scroll_timeline_util::ComputeProgress(
                                             current_offset, resolved_offsets) *
                                         duration.InMillisecondsF());
  return {TimelinePhase::kActive, calculated_current_time, resolved_offsets};
}

// Scroll-linked animations are initialized with the start time of zero.
absl::optional<base::TimeDelta>
ScrollTimeline::InitialStartTimeForAnimations() {
  return base::TimeDelta();
}

AnimationTimeDelta ScrollTimeline::CalculateIntrinsicIterationDuration(
    const Timing& timing) {
  absl::optional<AnimationTimeDelta> duration = GetDuration();

  // Only run calculation for progress based scroll timelines
  if (duration) {
    // if iteration_duration == "auto" and iterations > 0
    if (!timing.iteration_duration && timing.iteration_count > 0) {
      // duration represents 100% so we divide it by iteration count to
      // calculate the iteration duration. TODO: (crbug.com/1216527) Once
      // delays can be percentages we will include them in the calculation:
      // ((100% - start_delay% - end_delay%) / iterations) * duration
      return duration.value() / timing.iteration_count;
    }
  }
  return AnimationTimeDelta();
}

void ScrollTimeline::ServiceAnimations(TimingUpdateReason reason) {
  // Snapshot timeline state once at top of animation frame.
  if (reason == kTimingUpdateForAnimationFrame)
    SnapshotState();
  // When scroll timeline goes from inactive to active the animations may need
  // to be started and possibly composited.
  bool was_active =
      last_current_phase_and_time_ &&
      last_current_phase_and_time_.value().phase == TimelinePhase::kActive;
  if (!was_active && IsActive())
    MarkAnimationsCompositorPending();

  AnimationTimeline::ServiceAnimations(reason);
}

void ScrollTimeline::ScheduleNextServiceInternal(bool time_check) {
  if (AnimationsNeedingUpdateCount() == 0)
    return;

  if (time_check) {
    auto state = ComputeTimelineState();
    PhaseAndTime current_phase_and_time{state.phase, state.current_time};
    if (current_phase_and_time == last_current_phase_and_time_)
      return;
  }
  ScheduleServiceOnNextFrame();
}

void ScrollTimeline::ScheduleNextService() {
  ScheduleNextServiceInternal(/* time_check = */ true);
}

void ScrollTimeline::SnapshotState() {
  timeline_state_snapshotted_ = ComputeTimelineState();
}

Element* ScrollTimeline::scrollSource() const {
  return scroll_source_.Get();
}

String ScrollTimeline::orientation() {
  switch (orientation_) {
    case Block:
      return "block";
    case Inline:
      return "inline";
    case Horizontal:
      return "horizontal";
    case Vertical:
      return "vertical";
    default:
      NOTREACHED();
      return "";
  }
}

const HeapVector<Member<V8ScrollTimelineOffset>> ScrollTimeline::scrollOffsets()
    const {
  HeapVector<Member<V8ScrollTimelineOffset>> scroll_offsets;
  for (auto& offset : scroll_offsets_) {
    scroll_offsets.push_back(offset->ToV8ScrollTimelineOffset());
    // 'auto' can only be the end offset.
    DCHECK(!offset->IsDefaultValue() || scroll_offsets.size() == 2);
  }
  return scroll_offsets;
}

V8UnionDoubleOrScrollTimelineAutoKeyword* ScrollTimeline::timeRange() const {
  // TODO(crbug.com/1140602): Support progress based animations
  // We are currently abusing the intended use of the "auto" keyword. We are
  // using it here as a signal to use progress based timeline instead of having
  // a range based current time.
  // We are doing this maintain backwards compatibility with existing tests.
  if (time_range_) {
    return MakeGarbageCollected<V8UnionDoubleOrScrollTimelineAutoKeyword>(
        time_range_.value());
  }
  return MakeGarbageCollected<V8UnionDoubleOrScrollTimelineAutoKeyword>(
      V8ScrollTimelineAutoKeyword(V8ScrollTimelineAutoKeyword::Enum::kAuto));
}

void ScrollTimeline::GetCurrentAndMaxOffset(const LayoutBox* layout_box,
                                            double& current_offset,
                                            double& max_offset) const {
  DCHECK(layout_box);
  DCHECK(layout_box->GetScrollableArea());

  // Depending on the writing-mode and direction, the scroll origin shifts and
  // the scroll offset may be negative. The easiest way to deal with this is to
  // use only the magnitude of the scroll offset, and compare it to (max_offset
  // - min_offset).
  PaintLayerScrollableArea* scrollable_area = layout_box->GetScrollableArea();

  // Using the absolute value of the scroll offset only makes sense if either
  // the max or min scroll offset for a given axis is 0. This should be
  // guaranteed by the scroll origin code, but these DCHECKs ensure that.
  DCHECK(scrollable_area->MaximumScrollOffset().Height() == 0 ||
         scrollable_area->MinimumScrollOffset().Height() == 0);
  DCHECK(scrollable_area->MaximumScrollOffset().Width() == 0 ||
         scrollable_area->MinimumScrollOffset().Width() == 0);
  ScrollOffset scroll_offset = scrollable_area->GetScrollOffset();
  ScrollOffset scroll_dimensions = scrollable_area->MaximumScrollOffset() -
                                   scrollable_area->MinimumScrollOffset();

  auto physical_orientation =
      ToPhysicalScrollOrientation(orientation_, *layout_box);

  if (physical_orientation == kHorizontalScroll) {
    current_offset = scroll_offset.Width();
    max_offset = scroll_dimensions.Width();
  } else {
    current_offset = scroll_offset.Height();
    max_offset = scroll_dimensions.Height();
  }
  // When using a rtl direction, current_offset grows correctly from 0 to
  // max_offset, but is negative. Since our offsets are all just deltas along
  // the orientation direction, we can just take the absolute current_offset and
  // use that everywhere.
  current_offset = std::abs(current_offset);
}

void ScrollTimeline::AnimationAttached(Animation* animation) {
  if (resolved_scroll_source_ && !HasAnimations())
    resolved_scroll_source_->RegisterScrollTimeline(this);

  AnimationTimeline::AnimationAttached(animation);
}

void ScrollTimeline::AnimationDetached(Animation* animation) {
  AnimationTimeline::AnimationDetached(animation);

  if (resolved_scroll_source_ && !HasAnimations())
    resolved_scroll_source_->UnregisterScrollTimeline(this);
}

void ScrollTimeline::WorkletAnimationAttached() {
  if (!resolved_scroll_source_)
    return;
  GetActiveScrollTimelineSet().insert(resolved_scroll_source_);
}

void ScrollTimeline::WorkletAnimationDetached() {
  if (!resolved_scroll_source_)
    return;
  GetActiveScrollTimelineSet().erase(resolved_scroll_source_);
}

void ScrollTimeline::Trace(Visitor* visitor) const {
  visitor->Trace(scroll_source_);
  visitor->Trace(resolved_scroll_source_);
  visitor->Trace(scroll_offsets_);
  AnimationTimeline::Trace(visitor);
}

bool ScrollTimeline::HasActiveScrollTimeline(Node* node) {
  ActiveScrollTimelineSet& worklet_animations_set =
      GetActiveScrollTimelineSet();
  auto worklet_animations_it = worklet_animations_set.find(node);
  if (worklet_animations_it != worklet_animations_set.end() &&
      worklet_animations_it->value > 0)
    return true;

  ScrollTimelineSet& set = GetScrollTimelineSet();
  auto it = set.find(node);
  if (it == set.end())
    return false;

  for (auto& timeline : *it->value) {
    if (timeline->HasAnimations())
      return true;
  }
  return false;
}

void ScrollTimeline::Invalidate(Node* node) {
  ScrollTimelineSet& set = GetScrollTimelineSet();
  auto it = set.find(node);

  if (it == set.end())
    return;

  for (auto& timeline : *it->value) {
    timeline->Invalidate();
  }
}

void ScrollTimeline::InvalidateEffectTargetStyle() {
  for (Animation* animation : GetAnimations())
    animation->InvalidateEffectTargetStyle();
}

void ScrollTimeline::ValidateState() {
  auto state = ComputeTimelineState();
  if (timeline_state_snapshotted_ == state)
    return;
  timeline_state_snapshotted_ = state;
  InvalidateEffectTargetStyle();
}

CompositorAnimationTimeline* ScrollTimeline::EnsureCompositorTimeline() {
  if (compositor_timeline_)
    return compositor_timeline_.get();

  compositor_timeline_ = std::make_unique<CompositorAnimationTimeline>(
      scroll_timeline_util::ToCompositorScrollTimeline(this));
  return compositor_timeline_.get();
}

void ScrollTimeline::UpdateCompositorTimeline() {
  if (!compositor_timeline_)
    return;
  compositor_timeline_->UpdateCompositorTimeline(
      scroll_timeline_util::GetCompositorScrollElementId(
          resolved_scroll_source_),
      GetResolvedScrollOffsets());
}

}  // namespace blink
