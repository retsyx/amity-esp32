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
    // A cancelled chord cannot turn into a second chord before all buttons
    // release, even after wait_for_release's fallback has expired.
    if (macro_selector_ >= 0) macro_executed_ = true;
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

void RemoteInputController::tick(uint64_t now) {
    if (now > now_) now_ = now;
    if (wait_for_release_ && now_ >= wait_until_) wait_for_release_ = false;
}

void RemoteInputController::set_wait_for_release(uint64_t now) {
    tick(now);
    wait_for_release_ = true;
    wait_until_ = now_ + wait_for_release_us;
}

bool RemoteInputController::activity_selected(uint32_t selection, uint64_t now) {
    tick(now);
    if (!selection_initialized_) {
        selection_initialized_ = true;
        selection_ = selection;
        local_selection_pending_ = false;
        return false;
    }
    if (selection == selection_) return false;
    const bool own_selection = local_selection_pending_ && selection == selection_ + uint32_t{1};
    selection_ = selection;
    local_selection_pending_ = false;
    if (own_selection) {
        // The originating release report already armed the gate and may have
        // cleared it by releasing the last button. Do not arm it a second time.
        return false;
    }
    set_wait_for_release(now);
    suppress_held();
    return true;
}

bool RemoteInputController::accepts(InputStamp stamp) const {
    return initialized_ && !transition_pending_ && !wait_for_release_ &&
        stamp.generation == generation_ && stamp.target == target_;
}

void RemoteInputController::reconcile() {
    std::array<int8_t, 256> sources;
    sources.fill(-1);
    if (active() && !transition_pending_) for (size_t i = 0; i < held_.size(); ++i) {
        const auto &key = held_[i];
        if (!key.down || key.suppressed || key.wait_blocked || key.button == RemoteButton::Power ||
            shortcut(key.button) >= 0 || static_cast<int>(i) == macro_selector_) continue;
        const auto button = static_cast<size_t>(key.button);
        if (button < sources.size() && sources[button] < 0) sources[button] = static_cast<int8_t>(i);
    }
    // Each logical key owns its hold. Releasing one button cannot cancel or
    // restart another, including when the buttons route to different devices.
    for (size_t button = 0; button < current_keys_.size(); ++button) {
        if (!current_keys_[button] || sources[button] >= 0) continue;
        emit(RemoteCommandType::Release, static_cast<int>(button));
        current_keys_[button] = false;
    }
    for (size_t i = 0; i < held_.size(); ++i) {
        const auto button = static_cast<size_t>(held_[i].button);
        if (button >= sources.size() || sources[button] != static_cast<int8_t>(i) || current_keys_[button]) continue;
        const auto saved_stamp = report_stamp_;
        report_stamp_ = held_[i].stamp;
        current_keys_[button] = emit(RemoteCommandType::Press, static_cast<int>(button));
        report_stamp_ = saved_stamp;
    }
}

void RemoteInputController::action(RemoteCommandType type, int value) {
    const bool changes_target = type == RemoteCommandType::PowerShort ||
        (type == RemoteCommandType::Activity && value != target_);
    if (emit(type, value)) {
        report_action_ = true;
        suppress_held();
        // A successful same-target activity selection needs this gate too.
        // Power-long only repairs the current activity in the Python reference.
        if (type == RemoteCommandType::Activity || type == RemoteCommandType::PowerShort) {
            set_wait_for_release(now_);
            local_selection_pending_ = true;
        }
        if (changes_target) transition_pending_ = true;
    }
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
    if (macro_executed_) return;
    // Waiting suppresses forwarded presses and macro recognition, but releases
    // still drive Power and standby activity selection, as in Python.
    if (key.wait_blocked && active() && button != RemoteButton::Power) return;
    const bool long_press = report_stamp_.timestamp >= started && report_stamp_.timestamp - started >= long_press_us;
    if (macro_selector_ >= 0) {
        if (static_cast<int>(index) != macro_selector_ && static_cast<int>(index) != macro_power_) return;
        const auto &function = held_[static_cast<size_t>(macro_selector_)];
        const auto macro_start = static_cast<int>(index) == macro_selector_ ? started : function.stamp.timestamp;
        const bool long_macro = report_stamp_.timestamp >= macro_start && report_stamp_.timestamp - macro_start >= long_press_us;
        // The two macro buttons are consumed regardless of activity admission.
        // Other held keys survive a rejected selection. Keep the executed
        // latch until every physical button is up, as in Python's in_macro.
        held_[static_cast<size_t>(macro_selector_)].suppressed = true;
        held_[static_cast<size_t>(macro_power_)].suppressed = true;
        macro_selector_ = macro_power_ = -1;
        macro_executed_ = true;
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
    tick(stamp.timestamp);
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
            if (key) {
                release(static_cast<size_t>(key - held_.data()), stale);
                if (!key->down) {
                    bool down = false;
                    for (const auto &held : held_) down = down || held.down;
                    if (!down) {
                        wait_for_release_ = false;
                        macro_executed_ = false;
                        macro_selector_ = macro_power_ = -1;
                    }
                }
            }
        } else if (!key) {
            if (!empty) { cancel(); return; }
            *empty = Held{true, stale || transition_pending_ || report_action_, wait_for_release_, profile,
                          event.physical, event.button, stamp};
        }
    }
    if (macro_selector_ < 0 && !macro_executed_ && active() && bindings(profile).power_chords && !transition_pending_) {
        for (size_t i = 0; i < held_.size(); ++i) {
            const auto &power = held_[i];
            if (!power.down || power.suppressed || power.wait_blocked || power.profile != profile || power.button != RemoteButton::Power) continue;
            for (int choice = 0; choice < 5 && macro_selector_ < 0; ++choice)
                for (size_t j = 0; j < held_.size(); ++j) {
                    const auto &key = held_[j];
                    if (key.down && !key.suppressed && !key.wait_blocked && key.profile == profile && selector(key.button) == choice) {
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
    tick(stamp.timestamp);
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
    macro_executed_ = false;
    reconcile();
}
} // namespace ble
