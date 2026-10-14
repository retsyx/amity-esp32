/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "remote_input.hpp"

namespace ble {
namespace {
struct Bindings { uint8_t long_activity_offset; bool standby_selectors, power_chords; };
constexpr Bindings bindings(RemoteProfile profile, uint8_t atomic_offset = 4) {
    return profile == RemoteProfile::Siri ? Bindings{5, true, true} : Bindings{atomic_offset, false, false};
}
constexpr int selector(RemoteButton button) {
    switch (button) {
    case RemoteButton::Select: return 0;
    case RemoteButton::Up: return 1;
    case RemoteButton::Right: return 2;
    case RemoteButton::Down: return 3;
    case RemoteButton::Left: return 4;
    default: return -1;
    }
}
constexpr int shortcut(RemoteButton button) {
    const int value = static_cast<int>(button) - static_cast<int>(RemoteButton::Activity0);
    return value >= 0 && value <= 3 ? value : -1;
}
bool same_activity(InputStamp a, InputStamp b) { return a.generation == b.generation && a.target == b.target; }
}

bool RemoteInputController::emit(RemoteCommandType type, int value, uint8_t count) {
    return callback_ && callback_(RemoteCommand{type, value, count, report_stamp_}, context_);
}

void RemoteInputController::suppress_held() {
    for (auto &key : held_) if (key.down) key.suppressed = true;
    macro_selector_ = macro_power_ = -1;
    reconcile();
}

bool RemoteInputController::synchronize(uint32_t generation, int target) {
    const bool changed = initialized_ && (generation != generation_ || target != target_);
    generation_ = generation;
    target_ = target;
    initialized_ = true;
    if (changed) {
        transition_pending_ = false;
        suppress_held();
    }
    return changed;
}

bool RemoteInputController::accepts(InputStamp stamp) const {
    return initialized_ && !transition_pending_ && stamp.generation == generation_ && stamp.target == target_;
}

void RemoteInputController::reconcile() {
    int latest = -1;
    if (active() && !transition_pending_) for (size_t i = 0; i < held_.size(); ++i) {
        const auto &key = held_[i];
        if (!key.down || key.suppressed || key.button == RemoteButton::Power ||
            shortcut(key.button) >= 0 || static_cast<int>(i) == macro_selector_) continue;
        if (latest < 0 || held_[static_cast<size_t>(latest)].order < key.order) latest = static_cast<int>(i);
    }
    const int next = latest < 0 ? -1 : static_cast<int>(held_[static_cast<size_t>(latest)].button);
    if (next == current_key_) return; // Aliases retain one hold until the last usage releases.
    if (current_key_ >= 0) emit(RemoteCommandType::Release);
    current_key_ = next;
    if (next >= 0) {
        const auto saved_stamp = report_stamp_;
        report_stamp_ = held_[static_cast<size_t>(latest)].stamp;
        if (!emit(RemoteCommandType::Press, next)) current_key_ = -1;
        report_stamp_ = saved_stamp;
    }
}

void RemoteInputController::action(RemoteCommandType type, int value) {
    report_action_ = true;
    suppress_held();
    const bool changes_target = type == RemoteCommandType::PowerShort ||
        (type == RemoteCommandType::Activity && value != target_);
    // Same-target repairs may never advance the generation. Only an actual
    // transition needs to block fresh reports until its reservation is visible.
    if (emit(type, value) && changes_target) transition_pending_ = true;
}

void RemoteInputController::release(size_t index, bool stale) {
    auto &key = held_[index];
    // An old queued release cannot cancel a newer press of this physical key.
    if (stale && !same_activity(key.stamp, report_stamp_)) return;
    const auto started = key.stamp.timestamp;
    const auto button = key.button;
    const auto profile = bindings(key.profile, atomic_long_activity_offset_);
    const bool suppressed = key.suppressed || stale || transition_pending_;
    key.down = false;
    if (suppressed) return;
    const bool long_press = report_stamp_.timestamp >= started && report_stamp_.timestamp - started >= long_press_us;
    if (macro_selector_ >= 0) {
        if (static_cast<int>(index) != macro_selector_ && static_cast<int>(index) != macro_power_) return;
        const auto &function = held_[static_cast<size_t>(macro_selector_)];
        const auto macro_start = static_cast<int>(index) == macro_selector_ ? started : function.stamp.timestamp;
        const bool long_macro = report_stamp_.timestamp >= macro_start && report_stamp_.timestamp - macro_start >= long_press_us;
        action(RemoteCommandType::Activity, selector(function.button) + (long_macro ? profile.long_activity_offset : 0));
    } else if (shortcut(button) >= 0) {
        action(RemoteCommandType::Activity, shortcut(button) + (long_press ? profile.long_activity_offset : 0));
    } else if (!active() && profile.standby_selectors && selector(button) >= 0) {
        action(RemoteCommandType::Activity, selector(button) + (long_press ? profile.long_activity_offset : 0));
    } else if (button == RemoteButton::Power) {
        if (!active() && profile.standby_selectors && !long_press) action(RemoteCommandType::Activity, 0);
        else action(long_press ? RemoteCommandType::PowerLong : RemoteCommandType::PowerShort);
    }
}

void RemoteInputController::buttons(RemoteProfile profile, const ButtonEvent *events, size_t count, InputStamp stamp) {
    if (!events && count) return;
    if (!initialized_) synchronize(stamp.generation, stamp.target);
    report_stamp_ = stamp;
    report_action_ = false;
    const bool stale = stamp.generation != generation_ || stamp.target != target_;
    for (size_t i = 0; i < count; ++i) {
        const auto &event = events[i];
        Held *key = nullptr, *empty = nullptr;
        for (auto &entry : held_) {
            if (entry.down && entry.profile == profile && entry.physical == event.physical) key = &entry;
            if (!entry.down && !empty) empty = &entry;
        }
        if (!event.down) {
            if (key) release(static_cast<size_t>(key - held_.data()), stale);
        } else if (!key) {
            if (!empty) { cancel(); return; }
            *empty = Held{true, stale || transition_pending_ || report_action_, profile,
                          event.physical, event.button, stamp, ++order_};
        }
    }
    if (macro_selector_ < 0 && active() && bindings(profile).power_chords && !transition_pending_) {
        for (size_t i = 0; i < held_.size(); ++i) {
            const auto &power = held_[i];
            if (!power.down || power.suppressed || power.profile != profile || power.button != RemoteButton::Power) continue;
            for (int choice = 0; choice < 5 && macro_selector_ < 0; ++choice)
                for (size_t j = 0; j < held_.size(); ++j) {
                    const auto &key = held_[j];
                    if (key.down && !key.suppressed && key.profile == profile && selector(key.button) == choice) {
                        macro_selector_ = static_cast<int>(j);
                        macro_power_ = static_cast<int>(i);
                        break;
                    }
                }
            break;
        }
    }
    reconcile();
}

void RemoteInputController::swipe(RemoteButton direction, uint8_t count, InputStamp stamp) {
    if (!count || direction < RemoteButton::Up || direction > RemoteButton::Right || !active() || !accepts(stamp)) return;
    for (const auto &key : held_) if (key.down) return;
    report_stamp_ = stamp;
    emit(RemoteCommandType::Swipe, static_cast<int>(direction), count);
}

void RemoteInputController::cancel() {
    // Adapters retain physical state (HID deltas / Siri full-state mask). A
    // missing queued release must not leave an owner-task hold stuck forever.
    for (auto &key : held_) key = {};
    macro_selector_ = macro_power_ = -1;
    reconcile();
}
} // namespace ble
