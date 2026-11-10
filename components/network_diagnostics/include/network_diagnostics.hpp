// Copyright 2026. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace amity::network_diagnostics {
struct PathSnapshot {
    uint32_t attempts=0,ok=0,no_mem=0,other=0;
    uint32_t last_call_ms=0,last_ok_ms=0,last_error_ms=0,last_duration_ms=0;
    int32_t last_error_code=0;
    uint32_t active=0,slow_calls=0,last_slow_ms=0,last_slow_duration_ms=0;
};
struct TimerSnapshot {
    uint32_t calls=0,completed=0,last_entry_ms=0,last_complete_ms=0,active=0,last_duration_ms=0;
    uint32_t late_entries=0,last_late_gap_ms=0,last_late_ms=0;
};
struct Snapshot {
    PathSnapshot tx,rx;
    TimerSnapshot tcp_timer;
    uint32_t now_ms=0;
    uint32_t tx_done_ok=0,tx_done_fail=0,tx_done_last_ms=0,tx_done_last_ok_ms=0,tx_done_last_fail_ms=0;
    uint32_t tx_done_registered=0,registration_attempts=0,registration_pending=0,event_registered=0;
    int32_t registration_error=0,event_error=0;
    uint32_t power_save_checked=0;
    int32_t power_save_mode=-1,power_save_error=0;
    uint32_t console_calls=0,console_elapsed_ms=0,console_slow_calls=0;
    uint32_t console_last_slow_began_ms=0,console_last_slow_duration_ms=0;
};

// Call once after Wi-Fi initialization, before the existing background poller.
// Diagnostics never alter admission, retries, power saving or delivery policy.
void start();
// Board-wide observations, not per-link delivery proof. Independent lock-free
// loads are best effort; counters and millisecond timestamps wrap at 32 bits.
// Slow calls span at least 100ms, including descheduling; last_slow_ms is the
// completion time. Active is a separate concurrent-call counter, not inferred.
// TCP timer entry gaps >=200ms are retained after the first entry. The SDK's
// timer runs on demand at 50ms while TCP PCBs exist; an idle restart can also
// produce a gap. These observations alone do not prove task starvation.
Snapshot snapshot();
// Sum caller durations, including overlaps. No console output is emitted here
// or by poll(), so the existing console sink can record itself safely.
void record_console_call(uint32_t began_ms,uint32_t duration_ms);
// One background caller. Consume pending lifecycle events and defer all logs.
void poll();
} // namespace amity::network_diagnostics
