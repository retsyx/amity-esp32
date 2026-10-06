#include "reset_button.hpp"
#include "device_settings.hpp"
#include <cassert>
#include <iostream>

using device_settings::ResetButton;

static void release(ResetButton &button, uint64_t now) {
    assert(!button.update(false, now));
    assert(!button.update(false, now + ResetButton::debounce_ms));
}

static void press(ResetButton &button, uint64_t now) {
    assert(!button.update(true, now));
    assert(!button.update(true, now + ResetButton::debounce_ms));
}

int main() {
    // Disabled builds link without GPIO, timer or task implementations, and
    // repeated startup calls have no effect on any pin.
    assert(device_settings::start_reset_button(5) == ESP_OK);
    assert(device_settings::start_reset_button(5) == ESP_OK);

    // A stuck-low input or a button held through boot must never wipe settings.
    ResetButton boot_held;
    press(boot_held, 0);
    assert(!boot_held.update(true, 60000));
    release(boot_held, 60010);
    press(boot_held, 60100);
    assert(boot_held.update(true, 70150));
    assert(!boot_held.update(true, 90000));
    assert(!boot_held.update(false, 90010)); // A release glitch cannot rearm.
    assert(!boot_held.update(true, 90020));
    assert(!boot_held.update(true, 110000));

    // An absent button held high has no effect, even after a long uptime.
    ResetButton idle;
    release(idle, 0);
    assert(!idle.update(false, 10000000000ULL));
    assert(idle.next_check_ms(10000000000ULL) == ResetButton::no_deadline);

    // The worker needs only GPIO edges and one-shot debounce/hold deadlines.
    ResetButton deadlines;
    assert(deadlines.next_check_ms(0) == 0);
    assert(!deadlines.update(false, 0));
    assert(deadlines.next_check_ms(0) == 50);
    assert(deadlines.next_check_ms(49) == 1);
    assert(!deadlines.update(false, 50));
    assert(deadlines.next_check_ms(50) == ResetButton::no_deadline);
    press(deadlines, 100);
    assert(deadlines.next_check_ms(150) == 10000);
    assert(deadlines.next_check_ms(10149) == 1);
    assert(deadlines.update(true, 10150));
    assert(deadlines.next_check_ms(10150) == ResetButton::no_deadline);
    release(deadlines, 10200);
    assert(deadlines.next_check_ms(10250) == ResetButton::no_deadline);

    ResetButton button;
    release(button, 0);
    press(button, 100);
    assert(!button.update(true, 10149));
    assert(button.update(true, 10150));
    assert(!button.update(true, 30150)); // One reset request per press.
    release(button, 30200);
    press(button, 30300);
    assert(button.update(true, 40350)); // A failed request may be retried.

    // Several short holds do not accumulate toward the destructive threshold.
    ResetButton short_holds;
    release(short_holds, 0);
    for (uint64_t now = 100; now < 40000; now += 6000) {
        press(short_holds, now);
        assert(!short_holds.update(true, now + 5000));
        release(short_holds, now + 5100);
    }

    // Contact bounce does not arm at boot or count as a stable press.
    ResetButton bouncing;
    assert(!bouncing.update(true, 0));
    assert(!bouncing.update(false, 100));
    assert(!bouncing.update(true, 120));
    assert(!bouncing.update(true, 170));
    assert(!bouncing.update(true, 20000));
    release(bouncing, 20100);
    assert(!bouncing.update(true, 20200));
    assert(!bouncing.update(false, 20220));
    press(bouncing, 20240);
    assert(!bouncing.update(true, 30250));
    assert(bouncing.update(true, 30290));

    // Releasing exactly at the threshold cancels instead of triggering.
    ResetButton cancelled;
    release(cancelled, 0);
    press(cancelled, 100);
    assert(!cancelled.update(false, 10150));
    assert(!cancelled.update(false, 10200));
    press(cancelled, 10300);
    assert(!cancelled.update(true, 20200));
    assert(cancelled.update(true, 20350));

    // The monotonic timestamp must work beyond a 32-bit millisecond uptime.
    ResetButton long_uptime;
    constexpr uint64_t start = 1ULL << 33;
    release(long_uptime, start);
    press(long_uptime, start + 100);
    assert(!long_uptime.update(true, start + 10149));
    assert(long_uptime.update(true, start + 10150));

    std::cout << "PASS FW_RESET debounce, boot interlock, hold threshold, cancellation, retry, long uptime and interrupt deadlines\n";
}
