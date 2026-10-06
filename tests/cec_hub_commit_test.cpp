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
        run_one_command();
        assert(hub.snapshot().devices.empty());
        fixture_present = true; fixture_sent.clear();
        assert(hub.select_activity(0)); run_one_command();
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
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command();
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
        assert(hub.configure(activities)); run_one_command();
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
        assert(hub.configure(activities)); run_one_command();
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
    { // Completion drives repeats without a fixed wait or queued repeat work.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command();
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_waited.clear();
        fixture_wire_complete = [&] { if (fixture_waited.size() == 4) assert(hub.release_key()); };
        assert(hub.press_key(0x41, true)); run_one_command(); run_idle(8);
        assert(fixture_waited.size() == 4);
        assert(idle_waits.size() >= 3 && idle_waits[0] == 0 && idle_waits[1] == 0 && idle_waits[2] == 0);
        std::vector<uint8_t> keys;
        for (const auto &sent : fixture_sent) {
            const auto &f = sent.frame;
            if (sent.gpio == 2 && f.data[0] == 0x04 && (f.data[1] == 0x44 || f.data[1] == 0x45)) {
                keys.push_back(f.data[1]); if (f.data[1] == 0x44) assert(f.data[2] == 0x41);
            }
        }
        assert((keys == std::vector<uint8_t>{0x44, 0x44, 0x44, 0x44, 0x45}));
        fixture_wire_complete = {}; fixture_present = false;
    }
    { // A release during the very first attempt preserves a tap, with no repeat.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"Watch", "TV", "Source", "Source", "", 0}})); run_one_command();
        assert(hub.select_activity(0)); run_one_command();
        fixture_sent.clear(); fixture_waited.clear();
        fixture_wire_complete = [&] { assert(hub.release_key()); };
        assert(hub.press_key(1, true)); run_one_command(); run_idle(3);
        assert(fixture_waited.size() == 1);
        bool released = false;
        for (const auto &sent : fixture_sent) released |= sent.frame.size == 2 && sent.frame.data[1] == 0x45;
        assert(released); fixture_wire_complete = {};
        // Release before owner execution also preserves one initial press.
        fixture_waited.clear(); assert(hub.press_key(2, true)); assert(hub.release_key());
        run_one_command(); run_one_command(); run_idle(3); assert(fixture_waited.size() == 1);
        // Ordinary one-shot commands never turn into a held stream.
        fixture_waited.clear(); assert(hub.press_key(0x42)); run_one_command(); run_idle(3);
        assert(fixture_waited.empty()); fixture_present = false;
    }
    { // Switching activity during a frame cancels continuation before the next.
        fixture_present = true;
        amity::HubService hub; assert(hub.start(1, 2) == ESP_OK);
        assert(hub.configure({{"First", "TV", "Source", "Source", "", 0},
                              {"Second", "TV", "Source", "Source", "", 0}})); run_one_command();
        assert(hub.select_activity(0)); run_one_command(); fixture_waited.clear();
        fixture_wire_complete = [&] { assert(hub.select_activity(1)); };
        assert(hub.press_key(0x42, true)); run_one_command(); run_one_command(); run_idle(3);
        assert(fixture_waited.size() == 1 && hub.snapshot().current_activity == 1);
        fixture_wire_complete = {}; fixture_present = false;
    }
    std::puts("Hub admission, completion-driven held keys, release recovery, and activity-generation tests passed");
}
