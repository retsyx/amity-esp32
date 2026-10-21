/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

#include "cec_hub_fixture.hpp"
static std::vector<amity::Activity> config() { return {{"Saved", "TV", "Source", "Audio", "", 0}}; }
int main() {
    { // An unstarted service must not persist a configuration it cannot run.
        amity::HubService hub; bool committed = false;
        assert(hub.configure_with_commit(config(), {}, [&] { committed = true; return ESP_OK; }) == ESP_ERR_INVALID_STATE);
        assert(!committed);
    }
    { // Every producer counts toward capacity; full queue forbids the commit.
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        for (int i = 0; i < 32; ++i) assert(hub.release_key());
        bool committed = false;
        assert(hub.configure_with_commit(config(), {}, [&] { committed = true; return ESP_OK; }) == ESP_ERR_NO_MEM);
        assert(!committed && !hub.standby());
        run_one_command();
        assert(hub.standby()); // Dequeue releases exactly one slot.
        assert(!hub.standby());
    }
    { // Storage failure leaves all queue slots and the old live config intact.
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        int commits = 0;
        assert(hub.configure_with_commit(config(), {}, [&] { ++commits; return ESP_FAIL; }) == ESP_FAIL);
        assert(commits == 1 && hub.snapshot().activities.empty());
        for (int i = 0; i < 32; ++i) assert(hub.press_key(0));
        assert(hub.release_key()); // Emergency path consumes no queue slot.
        assert(!hub.press_key(0));
    }
    { // A competing producer cannot steal the slot while commit is running.
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        int commits = 0;
        assert(hub.configure_with_commit(config(), {}, [&] {
            ++commits;
            bool competing_commit = false;
            assert(hub.configure_with_commit(config(), {}, [&] { competing_commit = true; return ESP_OK; }) == ESP_ERR_TIMEOUT);
            assert(!competing_commit);
            for (int i = 0; i < 31; ++i) assert(hub.release_key());
            assert(!hub.press_key(0));
            return ESP_OK;
        }) == ESP_OK);
        assert(commits == 1 && !hub.standby()); // Committed config occupies reserved 32nd slot.
    }
    { // A committed config becomes live only through the ordinary owner task.
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure_with_commit(config(), {}, [] { return ESP_OK; }) == ESP_OK);
        assert(hub.snapshot().activities.empty());
        run_one_command();
        assert(hub.snapshot().activities.size() == 1);
        assert(hub.snapshot().activities[0].name == "Saved");
        for (int i = 0; i < 32; ++i) assert(hub.release_key());
        assert(!hub.press_key(0));
    }
    { // The activity that caused discovery must still power and route newly
      // found devices; executing it before scanning used to lose these sends.
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Late devices", "TV", "Source", "Source", "", 0}}));
        run_one_command(); fixture_finish_discovery(hub);
        assert(hub.snapshot().devices.empty());
        const auto discovered = hub.snapshot().discovery_generation;
        fixture_present = true; fixture_sent.clear(); fixture_time += 120000000;
        assert(hub.select_activity(0)); run_one_command();
        fixture_finish_discovery(hub, discovered); run_idle(1);
        bool tv_on = false, source_on = false, route = false;
        for (const auto &sent : fixture_sent) {
            const auto &frame = sent.frame;
            tv_on |= sent.gpio == 1 && frame.size == 2 && frame.data[0] == 0x40 && frame.data[1] == 0x04;
            source_on |= sent.gpio == 2 && frame.size == 3 && frame.data[0] == 0x04 && frame.data[1] == 0x44 && frame.data[2] == 0x6d;
            route |= sent.gpio == 2 && frame.size == 4 && frame.data[0] == 0x0f && frame.data[1] == 0x86 && frame.data[2] == 0x10;
        }
        assert(tv_on && source_on && route);
        assert(hub.snapshot().current_activity == 0 && hub.snapshot().devices.size() == 2);
        fixture_present = false;
    }
    { // Release survives full admission, cancels stale repetitions, and does
      // not cancel a later key accepted once the first slot becomes free.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        assert(hub.press_key(0, true)); run_one_command();
        fixture_sent.clear();
        for (int i = 0; i < 32; ++i) assert(hub.press_key(1, true));
        assert(hub.release_key()); assert(!hub.press_key(2, true));
        run_one_command();
        assert(hub.press_key(2, true));
        for (int i = 0; i < 32; ++i) run_one_command();
        bool released = false, future_key = false, stale_key = false;
        for (const auto &sent : fixture_sent) {
            const auto &frame = sent.frame;
            if (sent.gpio != 2 || frame.data[0] != 0x04) continue;
            released |= frame.size == 2 && frame.data[1] == 0x45;
            stale_key |= frame.size == 3 && frame.data[1] == 0x44 && frame.data[2] == 1;
            future_key |= frame.size == 3 && frame.data[1] == 0x44 && frame.data[2] == 2;
        }
        assert(released && future_key && !stale_key);
        fixture_present = false;
    }
    { // A volume key may pass application admission while a switch is still
      // queued. The real hub worker must check its generation at execution.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        const std::vector<amity::Activity> activities{{"First", "TV", "Source", "Source", "", 0},
                                                     {"Second", "TV", "Source", "Source", "", 0}};
        assert(hub.configure(activities)); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        const auto first = hub.activity_generation();
        fixture_sent.clear();
        assert(hub.select_activity(1));
        assert(hub.press_key_for_activity(0x41, first));
        run_one_command(); run_one_command();
        assert(hub.activity_generation() != first);
        for (const auto &sent : fixture_sent)
            assert(!(sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x41));
        // Configuration replacement and returning to the original index also
        // invalidate the old generation; a new adjustment remains usable.
        assert(hub.configure(activities)); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        assert(hub.activity_generation() != first);
        fixture_sent.clear();
        assert(hub.press_key_for_activity(0x41, first));
        assert(hub.press_key_for_activity(0x42, hub.activity_generation()));
        run_one_command(); run_one_command();
        bool down = false;
        for (const auto &sent : fixture_sent) {
            assert(!(sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x41));
            down |= sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x42;
        }
        assert(down);
        fixture_present = false;
    }
    { // A held Siri press is also generation-bound: admission during a queued
      // activity transition cannot begin a held stream on its new source.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                              {"Second", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        const auto first = hub.activity_generation();
        fixture_sent.clear(); fixture_completed.clear();
        assert(hub.select_activity(1));
        assert(hub.press_key_for_activity(0x01, first, true));
        run_one_command(); run_one_command(); run_idle(3);
        for (const auto &sent : fixture_sent)
            assert(!(sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x01));
        // A current generation retains normal held repeats and reliable release.
        fixture_sent.clear(); fixture_completed.clear();
        fixture_async_autocomplete = false;
        assert(hub.press_key_for_activity(0x02, hub.activity_generation(), true)); run_one_command();
        for (unsigned n = 0; n < 2; ++n) { assert(fixture_complete_next()); run_idle(1); }
        assert(fixture_complete_next()); assert(hub.release_key(0x02)); run_one_command(); run_idle(2);
        fixture_async_autocomplete = true;
        assert(fixture_completed.size() == 3);
        std::vector<uint8_t> keys;
        for (const auto &sent : fixture_sent) {
            const auto &f = sent.frame;
            if (sent.gpio == 2 && f.data[0] == 0x04 && (f.data[1] == 0x44 || f.data[1] == 0x45)) {
                keys.push_back(f.data[1]); if (f.data[1] == 0x44) assert(f.data[2] == 0x02);
            }
        }
        assert((keys == std::vector<uint8_t>{0x44, 0x44, 0x44, 0x45}));
        // Failed admission must not start or retain a held key.
        fixture_sent.clear(); fixture_completed.clear();
        for (int i = 0; i < 32; ++i) assert(hub.release_key());
        assert(!hub.press_key_for_activity(0x03, hub.activity_generation(), true));
        for (int i = 0; i < 32; ++i) run_one_command();
        run_idle(3);
        assert(fixture_completed.empty());
        for (const auto &sent : fixture_sent)
            assert(!(sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x03));
        fixture_present = false;
    }
    { // Each key repeats only after its own outstanding transmission completes.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_completed.clear(); fixture_async_autocomplete = false;
        assert(hub.press_key(0x41, true)); run_one_command(); run_idle(3);
        assert(fixture_completed.empty() && fixture_pending.size() == 1);
        for (unsigned n = 0; n < 3; ++n) {
            assert(fixture_complete_next()); run_idle(1);
            assert(fixture_completed.size() == n + 1 && fixture_pending.size() == 1);
        }
        assert(fixture_complete_next()); assert(hub.release_key(0x41)); run_one_command(); run_idle(2);
        assert(fixture_completed.size() == 4 && fixture_pending.empty());
        std::vector<uint8_t> keys;
        for (const auto &sent : fixture_sent) {
            const auto &f = sent.frame;
            if (sent.gpio == 2 && f.data[0] == 0x04 && (f.data[1] == 0x44 || f.data[1] == 0x45)) {
                keys.push_back(f.data[1]); if (f.data[1] == 0x44) assert(f.data[2] == 0x41);
            }
        }
        assert((keys == std::vector<uint8_t>{0x44, 0x44, 0x44, 0x44, 0x45}));
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // Terminal failures end only that held key and retain one final cleanup.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command(); fixture_async_autocomplete = false;
        auto count = [&](uint8_t opcode) {
            return std::count_if(fixture_sent.begin(), fixture_sent.end(), [&](const auto &sent) {
                return sent.frame.size > 1 && sent.frame.data[1] == opcode;
            });
        };
        for (const auto failure : {cec::TxResult::Nack, cec::TxResult::ArbitrationLost, cec::TxResult::LineError, cec::TxResult::Timeout}) {
            fixture_sent.clear(); assert(hub.press_key(0x41, true)); run_one_command();
            assert(count(0x44) == 1 && count(0x45) == 0);
            assert(fixture_complete_next(failure)); run_idle(3);
            assert(count(0x44) == 1 && count(0x45) == 1);
            assert(hub.press_key(0x41, true)); run_one_command(); run_idle(2);
            assert(count(0x44) == 1 && count(0x45) == 1);
            assert(hub.release_key(0x41)); run_one_command();
            assert(count(0x44) == 1 && count(0x45) == 1);
        }
        fixture_sent.clear(); assert(hub.press_key(0x41, true)); run_one_command();
        assert(fixture_complete_next(cec::TxResult::Success)); run_idle(1);
        assert(fixture_complete_next(cec::TxResult::Nack)); run_idle(2);
        assert(count(0x44) == 2 && count(0x45) == 1);
        assert(hub.release_key(0x41)); run_one_command();
        fixture_sent.clear(); assert(hub.press_key(0x41, true)); run_one_command();
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(count(0x44) == 1 && count(0x45) == 0); // Repeated down does not create another operation.
        const auto before_scan = hub.snapshot().discovery_generation;
        assert(hub.discover()); assert(hub.discover()); run_one_command(); run_one_command();
        assert(count(0x44) == 1); // Other commands execute with that press still pending.
        assert(fixture_complete_next(cec::TxResult::Success, 2)); run_idle(1);
        assert(count(0x44) == 2);
        assert(hub.release_key(0x41)); run_one_command(); fixture_complete_all(); run_idle(2);
        assert(count(0x45) == 1);
        for (const auto &sent : fixture_sent) if (sent.frame.size > 1 && (sent.frame.data[1] == 0x44 || sent.frame.data[1] == 0x45)) {
            assert(sent.attempts == (sent.frame.data[1] == 0x44 ? 1u : 3u));
            assert(sent.allow_overflow == (sent.frame.data[1] == 0x45));
            assert(sent.timeout_ms == 0);
        }
        fixture_async_autocomplete = true; fixture_complete_all(); fixture_finish_discovery(hub, before_scan); fixture_present = false;
    }
    { // Key up stops future continuation; a repeat already admitted to CEC
      // finishes and receives one final release in FIFO order.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_completed.clear(); fixture_async_autocomplete = false;
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(fixture_complete_next()); run_idle(1);
        assert(fixture_pending.size() == 1 && fixture_completed.size() == 1);
        assert(hub.release_key(0x41)); run_one_command();
        assert(fixture_complete_next()); run_idle(3);
        assert(fixture_completed.size() == 2 && fixture_pending.empty());
        assert(std::count_if(fixture_sent.begin(), fixture_sent.end(), [](const auto &sent) {
            return sent.frame.size == 2 && sent.frame.data[1] == 0x45;
        }) == 1);
        // Release before owner execution still preserves a short tap.
        fixture_sent.clear(); fixture_completed.clear();
        assert(hub.press_key(0x42, true)); assert(hub.release_key(0x42));
        run_one_command(); run_one_command(); assert(fixture_complete_next()); run_idle(2);
        std::vector<uint8_t> sequence;
        for (const auto &sent : fixture_sent) if (sent.frame.size > 1 && (sent.frame.data[1] == 0x44 || sent.frame.data[1] == 0x45))
            sequence.push_back(sent.frame.data[1]);
        assert((sequence == std::vector<uint8_t>{0x44, 0x45}));
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // Every caller supplies one UCP attempt, including power quirks and
      // switch selection. Releases alone are eligible for the extra slot.
        fixture_present=true;amity::HubService hub;assert(hub.start(1,2)==ESP_OK);
        assert(hub.configure({{"Switch","TV","","Source","Source",2}},{{0,{0x44,0x6d},{0x44,0x6c}}}));run_one_command(); fixture_finish_discovery(hub);
        fixture_sent.clear();assert(hub.select_activity(0));run_one_command();assert(hub.standby());run_one_command();
        bool power=false,input=false;
        for(const auto& sent:fixture_sent)if(sent.frame.size>1){
            if(sent.frame.data[1]==0x44){assert(sent.attempts==1&&!sent.allow_overflow);power|=sent.frame.data[2]==0x6d;input|=sent.frame.size==4&&sent.frame.data[2]==0x69&&sent.frame.data[3]==2;}
            if(sent.frame.data[1]==0x45)assert(sent.attempts==3&&sent.allow_overflow&&sent.timeout_ms==0);
        }
        assert(power&&input);fixture_present=false;
    }
    { // With eight ordinary frames already buffered in the driver, denied
      // finite UCPs must not use the ninth slot for an unpaired release.
        fixture_present=true;amity::HubService hub;assert(hub.start(1,2)==ESP_OK);
        assert(hub.configure({{"Switch","TV","","Source","Source",2}},{{0,{0x44,0x6d},{0x44,0x6c}}}));run_one_command(); fixture_finish_discovery(hub);
        unsigned occupied=8,presses=0,releases=0;
        fixture_admission=[&](int,const cec::Frame& frame){if(frame.size>1&&frame.data[1]==0x44){++presses;return false;}if(frame.size>1&&frame.data[1]==0x45){++releases;return occupied++<9;}return true;};
        assert(hub.select_activity(0));run_one_command();assert(presses>=2&&releases==0&&occupied==8);
        fixture_admission={};fixture_present=false;
    }
    { // A full ninth slot consumes final release; the owner retains no TX
      // retry and later up cannot regenerate the dropped release.
        fixture_present=true;amity::HubService hub;assert(hub.start(1,2)==ESP_OK);
        assert(hub.configure({{"Watch","TV","Source","Source","",0}}));run_one_command(); fixture_finish_discovery(hub);assert(hub.select_activity(0));run_one_command();
        assert(hub.press_key(0x41,true));run_one_command();fixture_sent.clear();
        fixture_send=[](int,const cec::Frame& frame){return frame.size<2||frame.data[1]!=0x45;};
        assert(hub.release_key());run_one_command();run_idle(5);
        auto releases=[&]{return std::count_if(fixture_sent.begin(),fixture_sent.end(),[](const auto& sent){return sent.frame.size>1&&sent.frame.data[1]==0x45;});};
        assert(releases()==1);fixture_send={};run_idle(5);assert(releases()==1);
        assert(hub.release_key());run_one_command();run_idle(2);assert(releases()==1);
        fixture_present=false;
    }
    { // Releasing during the initial pending attempt preserves one tap.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_completed.clear(); fixture_async_autocomplete = false;
        assert(hub.press_key(1, true)); run_one_command();
        assert(hub.release_key(1)); run_one_command();
        assert(fixture_complete_next()); run_idle(3);
        assert(fixture_completed.size() == 1 && fixture_pending.empty());
        bool released = false;
        for (const auto &sent : fixture_sent) released |= sent.frame.size == 2 && sent.frame.data[1] == 0x45;
        assert(released);
        fixture_completed.clear(); assert(hub.press_key(2, true)); assert(hub.release_key(2));
        run_one_command(); run_one_command(); assert(fixture_complete_next()); run_idle(3);
        assert(fixture_completed.size() == 1 && fixture_pending.empty());
        // Ordinary one-shot commands never turn into a held stream.
        fixture_completed.clear(); assert(hub.press_key(0x42)); run_one_command(); run_idle(3);
        assert(fixture_pending.empty() && fixture_completed.empty());
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // An activity switch is admitted before an earlier key completes and
      // prevents its eventual ACK from continuing into the new activity.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                              {"Second", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_completed.clear(); fixture_async_autocomplete = false;
        assert(hub.press_key(0x42, true)); run_one_command();
        assert(fixture_completed.empty() && fixture_pending.size() == 1);
        assert(hub.select_activity(1)); run_one_command();
        assert(hub.snapshot().current_activity == 1 && fixture_completed.empty());
        assert(fixture_complete_next()); run_idle(3);
        assert(fixture_completed.size() == 1 && fixture_pending.empty());
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // Independent key state and command admission continue while the wire
      // has not acknowledged an earlier press, even on the same CEC bus.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                              {"Second", "TV", "Source", "Source", "", 0}})); run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_completed.clear(); fixture_async_autocomplete = false;
        const auto presses = [&](uint8_t key) {
            return std::count_if(fixture_sent.begin(), fixture_sent.end(), [&](const auto &sent) {
                return sent.gpio == 2 && sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == key;
            });
        };
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(presses(0x41) == 1 && fixture_pending.size() == 1 && fixture_completed.empty());
        assert(hub.press_key(0x04, true)); run_one_command();
        assert(presses(0x04) == 1 && fixture_pending.size() == 2 && fixture_completed.empty());
        assert(hub.release_key(0x04)); run_one_command();
        assert(presses(0x41) == 1 && presses(0x04) == 1);
        // Volume Up is still held and gets its own continuation. Right's
        // eventual ACK does not recreate the key that was just released.
        assert(fixture_complete_next(cec::TxResult::Success)); run_idle(1);
        assert(presses(0x41) == 2 && presses(0x04) == 1);
        assert(fixture_complete_next(cec::TxResult::Success)); run_idle(1);
        assert(presses(0x41) == 2 && presses(0x04) == 1);
        const auto pending_before_switch = fixture_pending.size();
        const auto sent_before_switch = fixture_sent.size();
        assert(pending_before_switch > 0 && hub.select_activity(1)); run_one_command();
        assert(hub.snapshot().current_activity == 1 && fixture_sent.size() > sent_before_switch);
        for (const auto &sent : fixture_sent) assert(sent.timeout_ms == 0);
        fixture_complete_all(); fixture_async_autocomplete = true; run_idle(2);
        assert(presses(0x41) == 2 && presses(0x04) == 1);
        fixture_present = false;
    }
    { // A button released during asynchronous device lookup becomes a finite
      // tap when discovery resolves it, rather than reviving a stale hold.
        fixture_present = false;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}}));
        run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_time += 120000000; fixture_present = true;
        const auto discovered = hub.snapshot().discovery_generation;
        fixture_sent.clear();
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(hub.release_key(0x41)); run_one_command();
        fixture_finish_discovery(hub, discovered); run_idle(5);
        unsigned presses = 0, releases = 0;
        for (const auto &sent : fixture_sent) {
            if (sent.frame.size == 3 && sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x41) ++presses;
            if (sent.frame.size == 2 && sent.frame.data[1] == 0x45) ++releases;
        }
        assert(presses == 1 && releases == 1);
        fixture_present = false;
    }
    { // A new hold on another wire cannot depend on an ended hold's pending
      // completion, even when both instances use the same logical key.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                              {"Second", "TV", "Source", "TV", "", 0}}));
        run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_async_autocomplete = false;
        const auto volume_presses = [&](int gpio) {
            return std::count_if(fixture_sent.begin(), fixture_sent.end(), [&](const auto &sent) {
                return sent.gpio == gpio && sent.frame.size == 3 &&
                    sent.frame.data[1] == 0x44 && sent.frame.data[2] == 0x41;
            });
        };
        const auto volume_pending = [&](int gpio) {
            return std::count_if(fixture_pending.begin(), fixture_pending.end(), [&](const auto &pending) {
                return pending.gpio == gpio && pending.frame.size == 3 &&
                    pending.frame.data[1] == 0x44 && pending.frame.data[2] == 0x41;
            });
        };
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(volume_presses(2) == 1 && volume_pending(2) == 1);
        assert(hub.release_key(0x41)); run_one_command();
        assert(hub.select_activity(1)); run_one_command();
        assert(hub.snapshot().current_activity == 1);
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(volume_presses(1) == 1 && volume_pending(1) == 1 && volume_pending(2) == 1);

        // The new front hold repeats as soon as its own TX completes; the old
        // back TX is still pending and must not contribute to its outstanding count.
        assert(fixture_complete_next(cec::TxResult::Success, 1)); run_idle(1);
        assert(volume_presses(1) == 2 && volume_pending(1) == 1 && volume_pending(2) == 1);
        assert(fixture_complete_next(cec::TxResult::Nack, 2)); run_idle(1);
        assert(volume_presses(2) == 1 && volume_presses(1) == 2 && volume_pending(1) == 1);
        assert(fixture_complete_next(cec::TxResult::Success, 1)); run_idle(1);
        assert(volume_presses(1) == 3 && volume_pending(1) == 1); // Old failure did not stop it.

        assert(hub.release_key(0x41)); run_one_command();
        fixture_complete_all(); run_idle(2);
        assert(fixture_pending.empty());
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // Command-queue overflow release remains keyed: ending Right through
      // the emergency path cannot cancel Volume Up or its pending completion.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}}));
        run_one_command(); fixture_finish_discovery(hub);
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_async_autocomplete = false;
        const auto presses = [&](uint8_t key) {
            return std::count_if(fixture_sent.begin(), fixture_sent.end(), [&](const auto &sent) {
                return sent.gpio == 2 && sent.frame.size == 3 &&
                    sent.frame.data[1] == 0x44 && sent.frame.data[2] == key;
            });
        };
        assert(hub.press_key(0x41, true)); run_one_command();
        assert(hub.press_key(0x04, true)); run_one_command();
        assert(fixture_pending.size() == 2);
        for (unsigned n = 0; n < 32; ++n) assert(hub.press_key(0));
        assert(!hub.press_key(0));
        assert(hub.release_key(0x04)); // No command slot: use only Right's emergency epoch.
        assert(!hub.press_key(0)); // Fallback consumed no ordinary command slot.
        run_one_command();
        assert(presses(0x41) == 1 && presses(0x04) == 1);

        assert(fixture_complete_next(cec::TxResult::Success, 2)); run_one_command();
        assert(presses(0x41) == 2 && presses(0x04) == 1);
        assert(fixture_complete_next(cec::TxResult::Success, 2)); run_one_command();
        assert(presses(0x41) == 2 && presses(0x04) == 1); // Right's old ACK cannot repeat it.
        assert(fixture_complete_next(cec::TxResult::Success, 2)); run_one_command();
        assert(presses(0x41) == 3 && presses(0x04) == 1);

        assert(hub.release_key(0x41)); run_idle(1); // Drain the benign queued commands and up.
        fixture_complete_all(); run_idle(2);
        assert(fixture_pending.empty());
        fixture_async_autocomplete = true; fixture_present = false;
    }
    { // Selection notifications include same-target activity requests and
      // standby, but not Power-long repairs or rejected requests/commits.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        const auto initial = hub.activity_selection();
        assert(hub.configure_with_commit(config(), {}, [] { return ESP_FAIL; }) == ESP_FAIL);
        assert(hub.activity_selection() == initial);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}}));
        assert(hub.activity_selection() == initial + 1);
        run_one_command(); fixture_finish_discovery(hub);
        assert(hub.request_activity(0) == ESP_OK);
        const auto selected = hub.activity_selection();
        assert(selected == initial + 2);
        run_one_command();
        const auto activity = hub.activity_generation();
        assert(hub.request_activity(0) == ESP_OK);
        assert(hub.activity_selection() == selected + 1);
        run_one_command();
        assert(hub.activity_generation() == activity);
        const auto repaired = hub.activity_selection();
        assert(hub.repair_activity() == ESP_OK); run_one_command();
        assert(hub.activity_selection() == repaired);
        assert(hub.request_activity(7) == ESP_ERR_INVALID_ARG);
        assert(hub.activity_selection() == repaired);
        for (unsigned n = 0; n < 32; ++n) assert(hub.release_key());
        assert(hub.request_activity(0) == ESP_ERR_NO_MEM);
        assert(hub.activity_selection() == repaired);
        for (unsigned n = 0; n < 32; ++n) run_one_command();
        assert(hub.standby());
        assert(hub.activity_selection() == repaired + 1);
        run_one_command();
        fixture_present = false;
    }
    std::puts("Hub admission, asynchronous held keys, keyed release, and activity-generation tests passed");
}
