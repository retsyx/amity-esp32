/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ble {
enum class RemoteProfile { Atomic, Siri };
// Logical buttons, independent of HID usages or wire bit order.
enum class RemoteButton : uint16_t {
    Select = 0x00, Up = 0x01, Down = 0x02, Left = 0x03, Right = 0x04,
    Home = 0x09, Back = 0x0d, Siri = 0x35, Power = 0x40,
    VolumeUp = 0x41, VolumeDown = 0x42, Mute = 0x43, PlayPause = 0x61,
    Activity0 = 0x100, Activity1, Activity2, Activity3,
};
struct InputStamp {
    uint64_t timestamp = 0;
    uint32_t generation = 0;
    int target = -1;
};
struct ButtonEvent {
    uint32_t physical;
    RemoteButton button;
    bool down;
};
enum class RemoteCommandType { Press, Release, Activity, PowerShort, PowerLong, Swipe };
struct RemoteCommand {
    RemoteCommandType type;
    // Press and Release identify the logical CEC key. Aliased physical usages
    // share one press until their last usage releases.
    int value = 0;
    uint8_t count = 0;
    InputStamp stamp{};
};

// Both adapters feed this single button policy on one owner task. A physical
// report is a batch: ordered releases, presses, then held-key reconciliation.
class RemoteInputController {
public:
    using Callback = bool (*)(RemoteCommand command, void *context);
    explicit RemoteInputController(Callback callback, void *context = nullptr, uint8_t atomic_long_activity_offset = 4)
        : callback_(callback), context_(context), atomic_long_activity_offset_(atomic_long_activity_offset) {}
    // Includes reserved-target changes before publication, and active -> active.
    bool synchronize(uint32_t generation, int target);
    // Successful activity selections (including same-target selections) gate
    // fresh input until every button is up, with Python's one-second fallback.
    bool activity_selected(uint32_t selection, uint64_t now);
    void set_wait_for_release(uint64_t now);
    void tick(uint64_t now);
    bool waiting_for_release() const { return wait_for_release_; }
    void buttons(RemoteProfile profile, const ButtonEvent *events, size_t count, InputStamp stamp);
    void swipe(RemoteButton direction, uint8_t count, InputStamp stamp);
    // Cancel actions after link/queue loss; adapters retain physical state so
    // an unchanged hold cannot replay, and a lost release cannot stick forever.
    void cancel();
    bool accepts(InputStamp stamp) const;
    bool active() const { return target_ >= 0; }
    static constexpr uint64_t long_press_us = 500000;
    static constexpr uint64_t wait_for_release_us = 1000000;
private:
    struct Held {
        bool down = false, suppressed = false, wait_blocked = false;
        RemoteProfile profile = RemoteProfile::Atomic;
        uint32_t physical = 0;
        RemoteButton button = RemoteButton::Select;
        InputStamp stamp{};
    };
    std::array<Held, 32> held_{};
    std::array<bool, 256> current_keys_{};
    Callback callback_;
    void *context_;
    uint8_t atomic_long_activity_offset_;
    uint32_t generation_ = 0;
    int target_ = -1, macro_selector_ = -1, macro_power_ = -1;
    bool macro_executed_ = false;
    bool initialized_ = false, transition_pending_ = false, report_action_ = false;
    bool wait_for_release_ = true, selection_initialized_ = false, local_selection_pending_ = false;
    uint32_t selection_ = 0;
    uint64_t now_ = 0, wait_until_ = wait_for_release_us;
    InputStamp report_stamp_{};
    bool emit(RemoteCommandType type, int value = 0, uint8_t count = 0);
    void suppress_held();
    void reconcile();
    void release(size_t index, bool stale);
    void action(RemoteCommandType type, int value = 0);
};
} // namespace ble
