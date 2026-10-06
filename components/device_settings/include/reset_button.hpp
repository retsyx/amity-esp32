#pragma once
#include <cstdint>

namespace device_settings {

// GPIO-independent policy, driven by monotonic milliseconds. The input is
// active-high here: the GPIO adapter converts the active-low button level.
class ResetButton {
public:
    static constexpr uint64_t debounce_ms = 50;
    static constexpr uint64_t hold_ms = 10000;
    static constexpr uint64_t no_deadline = UINT64_MAX;

    // Returns true once per held press. A debounced release is required both
    // after boot and after triggering (including a failed reset request).
    bool update(bool pressed, uint64_t now_ms) {
        if (!initialized_ || candidate_ != pressed) {
            initialized_ = true;
            candidate_ = pressed;
            changed_at_ = now_ms;
        }
        if (now_ms - changed_at_ < debounce_ms) return false;

        if (stable_ != candidate_) {
            stable_ = candidate_;
            if (stable_) pressed_at_ = now_ms;
        }
        if (!stable_) {
            armed_ = true;
            fired_ = false;
            return false;
        }
        if (armed_ && !fired_ && now_ms - pressed_at_ >= hold_ms) {
            fired_ = true;
            return true;
        }
        return false;
    }

    // After update(), wait for a GPIO edge or this deadline. Stable released,
    // boot-held and already-triggered inputs need no periodic wakeups.
    uint64_t next_check_ms(uint64_t now_ms) const {
        if (!initialized_) return 0;
        const auto since_change = now_ms - changed_at_;
        if (since_change < debounce_ms) return debounce_ms - since_change;
        if (stable_ && armed_ && !fired_) {
            const auto held = now_ms - pressed_at_;
            return held < hold_ms ? hold_ms - held : 0;
        }
        return no_deadline;
    }

private:
    bool initialized_ = false;
    bool candidate_ = false;
    bool stable_ = false;
    bool armed_ = false;
    bool fired_ = false;
    uint64_t changed_at_ = 0;
    uint64_t pressed_at_ = 0;
};

} // namespace device_settings
