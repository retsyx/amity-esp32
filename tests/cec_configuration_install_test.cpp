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
    assert(control.select_activity(0)); run_one_command();
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
    fixture_waited.clear(); run_idle(1);
    assert(!fixture_waited.empty());
    assert(control.release_key()); run_one_command();
    fixture_sent.clear();
    activities[0].source = "Replacement";
    assert(control.configure(activities)); run_one_command();
    assert(control.activity_generation() != generation);
    assert(control.snapshot().current_activity == -1);
    for (const auto &s : fixture_sent) assert(s.frame.data[1] == 0x83 || s.frame.data[1] == 0x46 || s.frame.data[1] == 0x8c);
    assert(control.port(amity::BusSide::Back).side == amity::system::Side::back);
    std::puts("PASS facade identity, cosmetic install/hold preservation, semantic no-actuation");
}
