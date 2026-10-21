/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec_hub_fixture.hpp"
#include <algorithm>
int main() {
    fixture_present = true;
    amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
    assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                          {"Second", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
    // Reserve before the owner runs; a competing change cannot queue behind it.
    assert(hub.request_activity(0) == ESP_OK);
    assert(hub.activity_target() == 0);
    assert(hub.request_activity(1) == ESP_ERR_TIMEOUT);
    assert(hub.request_activity(-1) == ESP_ERR_TIMEOUT);
    assert(!hub.press_key(0x40)); // Raw CEC power cannot bypass the guard.
    assert(hub.request_activity(0) == ESP_OK); // Repair can queue behind initial setup.
    fixture_completed.clear(); fixture_async_autocomplete = false; run_one_command();
    assert(hub.snapshot().current_activity == 0 && hub.snapshot().activity_settling);
    assert(fixture_completed.empty() && !fixture_pending.empty());
    fixture_tick = 0;
    fixture_time += 10000000;
    assert(hub.snapshot().activity_settling && hub.request_activity(1) == ESP_ERR_TIMEOUT);
    // Elapsed quiet time cannot finish setup while its admitted CEC frames
    // still await completion. The owner remains available for the repair.
    fixture_complete_all(); fixture_async_autocomplete = true; run_one_command();
    assert(!fixture_completed.empty() && fixture_pending.empty());
    assert(hub.snapshot().activity_settling);
    cec::Frame source; source.size = 4; source.data[0] = 0x4f; source.data[1] = 0x82; source.data[2] = 0x10;
    cec::Frame audio; audio.size = 3; audio.data[0] = 0x40; audio.data[1] = 0x7a; audio.data[2] = 30;
    fixture_incoming.push_back({2, source}); fixture_incoming.push_back({2, audio});
    const auto reserved = fixture_time;
    run_idle(1);
    assert(hub.request_activity(1) == ESP_ERR_TIMEOUT);
    assert(hub.request_activity(-1) == ESP_ERR_TIMEOUT);
    fixture_time += 4000000;
    assert(hub.request_activity(0) == ESP_OK); run_one_command(); // Repair doesn't restart settling.
    fixture_time = reserved + 6000000; // Five seconds after matched replies, despite the intervening repair.
    assert(hub.request_activity(1) == ESP_OK);
    // The ignored standby must not appear later in the queue.
    fixture_tick = 100000; run_one_command(); fixture_tick = 0;
    assert(hub.snapshot().current_activity == 1);
    assert(hub.request_activity(0) == ESP_ERR_TIMEOUT);
    // Even at queue saturation, an ignored transition consumes no slots.
    for (int i = 0; i < 32; ++i) assert(hub.release_key());
    assert(hub.request_activity(0) == ESP_ERR_TIMEOUT);
    for (int i = 0; i < 32; ++i) run_one_command();
    fixture_time += 8000000;
    assert(hub.request_activity(-1) == ESP_OK);
    fixture_tick = 100000; run_one_command(); fixture_tick = 0;
    assert(hub.snapshot().current_activity == -1 && hub.snapshot().activity_settling);
    const auto off = fixture_time;
    assert(hub.request_activity(0) == ESP_ERR_TIMEOUT);
    assert(hub.request_activity(-1) == ESP_OK); run_one_command(); // Standby repair bypasses guard.
    fixture_time = off + 5000000;
    assert(hub.request_activity(0) == ESP_OK); // Standby expects ACKs only, no reply timeout.
    fixture_tick = 100000; run_one_command(); fixture_tick = 0;
    assert(hub.snapshot().current_activity == 0);
    // Configuration replacement is administrative and clears stale reservations.
    assert(hub.configure({{"Replacement", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
    assert(hub.request_activity(0) == ESP_OK);
    fixture_tick = 100000; run_one_command(); fixture_tick = 0;
    assert(hub.snapshot().current_activity == 0);
    fixture_time += 10000000; run_idle(1);
    assert(!hub.snapshot().activity_settling);
    for (int i = 0; i < 32; ++i) assert(hub.release_key());
    assert(hub.request_activity(-1) == ESP_ERR_NO_MEM); // No reservation when admission fails.
    assert(hub.activity_target() == 0);
    for (int i = 0; i < 32; ++i) run_one_command();
    assert(!hub.snapshot().activity_settling);
    puts("Hub settling admission, ignored requests, repair bypass, standby and configuration tests passed");
}
