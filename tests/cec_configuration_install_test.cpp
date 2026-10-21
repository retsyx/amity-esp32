#include "cec_hub_fixture.hpp"
#include "amity/system_control.hpp"

int main() {
    amity::SystemControl control;
    assert(control.start(1, 2) == ESP_ERR_INVALID_STATE);
    control.set_identity(*amity::system::ConsortiumId::parse("10000000000000000000000000000001"),
                         *amity::system::OctopusId::parse("20000000000000000000000000000001"));
    fixture_present = true;
    assert(control.start(1, 2) == ESP_OK);
    std::vector<amity::Activity> activities{{"Watch", "TV", "Source", "", "", 0}};
    assert(control.configure(activities)); run_one_command();
    run_idle(200);
    assert(control.select_activity(0)); run_one_command();
    fixture_async_autocomplete = false;
    assert(control.press_key(1, true)); run_one_command();
    const auto generation = control.activity_generation();
    activities[0].name = "Renamed";
    fixture_sent.clear();
    assert(control.configure_with_commit(activities, {}, [] { return ESP_OK; }) == ESP_OK);
    run_one_command();
    assert(control.activity_generation() == generation);
    assert(control.snapshot().current_activity == 0);
    assert(control.snapshot().activities[0].name == "Renamed");
    for (const auto &s : fixture_sent) assert(s.frame.data[1] == 0x83 || s.frame.data[1] == 0x46 || s.frame.data[1] == 0x8c);
    // Cosmetic installation preserves the independently pending key. Its
    // own completion admits the next press without blocking configuration.
    fixture_completed.clear();
    assert(fixture_complete_next()); run_idle(1);
    assert(!fixture_completed.empty() && !fixture_pending.empty());
    assert(control.release_key()); run_one_command();
    fixture_complete_all(); fixture_async_autocomplete = true; run_idle(1);
    fixture_sent.clear();
    activities[0].source = "Replacement";
    assert(control.configure(activities)); run_one_command();
    assert(control.activity_generation() != generation);
    assert(control.snapshot().current_activity == -1);
    for (const auto &s : fixture_sent) assert(s.frame.data[1] == 0x83 || s.frame.data[1] == 0x46 || s.frame.data[1] == 0x8c);
    assert(control.port(amity::BusSide::Back).side == amity::system::Side::back);
    std::puts("PASS facade identity, cosmetic install/hold preservation, semantic no-actuation");
}
