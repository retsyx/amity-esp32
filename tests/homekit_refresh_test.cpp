/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// HomeKit's input sources and their Home app edits, built from the whole of
// components/amity_homekit/homekit.cpp with the real saved-configuration
// readers (system_configuration.cpp) and SystemControl, which publishes each
// saved configuration as a save does.
#include "homekit.cpp"
#include "homekit_harness.hpp"
#include "intent_configuration_fixture.hpp"
#include "scenario_main.hpp"
#include <string>
#include <vector>

namespace {

namespace hap = amity_fake::hap;
using namespace homekit_harness;
using amity::system::ActivityId;
using amity_seam::count;
using amity_seam::install;

struct Saved {
    amity::SystemControl hub;
    Configuration configuration;
    unsigned activations = 0;
    esp_err_t activation = ESP_OK;
    RevisionPair expected;
    std::vector<unsigned> selected;

    void publish() { CHECK(hub.configure(saved_snapshot(configuration)) == ESP_OK); }
};
Saved &saved() {
    static Saved instance;
    return instance;
}

ActivityId activity(unsigned n) { return id<ActivityId>(n); }

// An activity in the next external slot.
void add(Configuration &configuration, unsigned n, const std::string &label) {
    ConfiguredActivity value;
    value.activity_id = activity(n);
    value.label = label;
    configuration.activities.push_back(value);
    configuration.external_slots.push_back({uint16_t(configuration.external_slots.size()), value.activity_id, false, false, {}});
}

Configuration watch_television() {
    auto configuration = empty_configuration();
    add(configuration, 3, "Watch television");
    return configuration;
}

// Activities 0 and 1 in slots 0 and 1, slot 2 retired, shown in the order 1, 0.
Configuration two_activities() {
    auto configuration = empty_configuration();
    for (unsigned n = 0; n < 3; ++n) add(configuration, 10 + n, "Activity " + std::to_string(n));
    configuration.external_slots[2].tombstone = true;
    configuration.activities.pop_back();
    configuration.activity_order = {activity(11), activity(10)};
    return configuration;
}

// Starts HomeKit with a saved setup and the given saved configuration. A
// Home app save publishes its candidate with the next presentation revision.
void setup(Configuration configuration) {
    install_defaults();
    auto &s = saved();
    install<&amity::HubService::set_system_identity>([](amity::HubService &, amity::system::ConsortiumId,
                                                          amity::system::OctopusId) {});
    install<&amity::HubService::snapshot>([](const amity::HubService &) { return amity::Snapshot{}; });
    install<&amity::HubService::configure_scoped>(
        [](amity::HubService &, std::vector<amity::Activity>, std::vector<amity::system::ScopedSelector>,
           amity::HubService::OwnedSender, amity::system::OctopusId, uint64_t, std::function<esp_err_t()> commit, uint64_t,
           std::vector<amity::system::OctopusId>, std::function<bool(const amity::Snapshot &)>) { return commit(); });
    install<&amity::activate_distributed_configuration>([](Configuration candidate, RevisionPair expected,
                                                           amity::system::MessageId request,
                                                           std::function<esp_err_t()> before_commit) {
        auto &s = saved();
        CHECK(request && !before_commit);
        ++s.activations;
        s.expected = expected;
        if (s.activation != ESP_OK) return s.activation;
        ++candidate.presentation_revision;
        s.configuration = std::move(candidate);
        s.publish();
        return ESP_OK;
    });
    s.hub.set_identity(configuration.consortium_id, *configuration.controller_octopus_id);
    s.configuration = std::move(configuration);
    s.publish();
    state().saved["hap_setup"] = setup_record();
    amity_matter_callbacks_t callbacks{};
    callbacks.activity = [](uint8_t index, void *context) {
        CHECK(context == &saved());
        saved().selected.push_back(index);
        return ESP_OK;
    };
    callbacks.context = &s;
    CHECK(start(s.hub, callbacks) == ESP_OK);
}

void refresh() { CHECK(hap::read(television().characteristic("B0")) == HAP_STATUS_SUCCESS); }

hap::Characteristic &name(unsigned slot) { return input(slot).characteristic("E3"); }
hap::Characteristic &label(unsigned slot) { return input(slot).characteristic("23"); }
hap::Characteristic &configured(unsigned slot) { return input(slot).characteristic("D6"); }
hap::Characteristic &visible(unsigned slot) { return input(slot).characteristic("135"); }
hap::Characteristic &target_visible(unsigned slot) { return input(slot).characteristic("134"); }

// Each rendering of the inputs reads every input's name to skip unchanged text.
unsigned renders() {
    const unsigned first = name(0).gets;
    for (unsigned slot = 0; slot < 16; ++slot) CHECK(name(slot).gets == first && label(slot).gets == first);
    return first;
}

// The input identifiers the Display Order TLV8 lists, in order.
std::vector<unsigned> display_order() {
    const auto bytes = television().characteristic("136").bytes();
    std::vector<unsigned> identifiers;
    for (size_t n = 0; n < bytes.size(); n += 3) {
        if (n) {
            CHECK(bytes[n] == 0 && bytes[n + 1] == 0);
            n += 2;
        }
        CHECK(n + 3 <= bytes.size() && bytes[n] == 1 && bytes[n + 1] == 1);
        identifiers.push_back(bytes[n + 2]);
    }
    return identifiers;
}
std::vector<unsigned> order(std::vector<unsigned> first) {
    for (unsigned n = 1; n <= 16; ++n)
        if (std::find(first.begin(), first.end(), n) == first.end()) first.push_back(n);
    return first;
}
unsigned order_notifications() { return television().characteristic("136").notifications; }

hap_status_t edit_name(unsigned slot, const char *value) { return hap::write(name(slot), text(value)); }
hap_status_t edit_visibility(unsigned slot, int value) { return hap::write(target_visible(slot), integer(value)); }

// Input names and visibility come only from the saved configuration: an
// activity's input shows its label, and unused inputs are hidden.
void saved_names() {
    setup(watch_television());
    CHECK(name(0).text == "Watch television" && label(0).text == "Watch television");
    CHECK(configured(0).number() == 1 && visible(0).number() == 0 && target_visible(0).number() == 0);
    for (unsigned slot = 1; slot < 16; ++slot) {
        CHECK(name(slot).text == "Unused " + std::to_string(slot + 1) && label(slot).text == name(slot).text);
        CHECK(configured(slot).number() == 0 && visible(slot).number() == 1 && target_visible(slot).number() == 1);
    }
    CHECK(renders() == 1);
    // config_store holds only the setup and the television name.
    CHECK(count("config_store::read") == 2);
}

// Reads stay cheap: inputs are rendered again only when the configuration or
// presentation revision changes, while playback, mute and the remote battery
// are read every time.
void cheap_refresh() {
    setup(watch_television());
    state().playback.current_activity = 0;
    state().playback.activity_id = activity(3);
    state().playback.audio_status_known = true;
    state().playback.muted = true;
    const auto playback = count("amity::HubService::playback_state"), battery = count("ble_remote_get_status");
    for (unsigned n = 0; n < 1000; ++n) refresh();
    CHECK(renders() == 1 && count("amity::HubService::playback_state") == playback + 1000);
    CHECK(count("ble_remote_get_status") == battery + 1000);
    CHECK(television().characteristic("B0").number() == 1 && television().characteristic("E7").number() == 1);
    CHECK(hap::read(speaker().characteristic("11A")) == HAP_STATUS_SUCCESS && speaker().characteristic("11A").number() == 1);
    CHECK(count("config_store::read") == 2);
}

// A new presentation revision renders an activity rename, a saved alias and
// a hidden input.
void presentation_edits() {
    setup(watch_television());
    auto &s = saved();
    ++s.configuration.presentation_revision;
    s.configuration.activities[0].label = "New name";
    s.publish();
    refresh();
    CHECK(renders() == 2 && name(0).text == "New name" && label(0).text == "New name");
    ++s.configuration.presentation_revision;
    s.configuration.external_slots[0].alias = "Saved alias";
    s.publish();
    refresh();
    CHECK(name(0).text == "Saved alias" && label(0).text == "Saved alias");
    ++s.configuration.presentation_revision;
    s.configuration.external_slots[0].hidden = true;
    s.publish();
    refresh();
    CHECK(visible(0).number() == 1 && target_visible(0).number() == 1 && configured(0).number() == 1);
    CHECK(renders() == 4);
    // Rendering writes only changed text: input 1 was renamed once, from its
    // initial name.
    CHECK(name(1).updates == 1 && label(1).updates == 1 && name(0).updates == 3);
}

// A new configuration revision renders a replaced activity, and a retired
// slot shows as unused again.
void replacement_and_tombstone() {
    setup(watch_television());
    auto &s = saved();
    s.configuration.external_slots[0].hidden = true;
    ++s.configuration.presentation_revision;
    s.publish();
    refresh();
    ++s.configuration.config_revision;
    s.configuration.activities[0].activity_id = activity(4);
    s.configuration.external_slots[0].activity_id = activity(4);
    s.configuration.external_slots[0].alias = "Replacement";
    s.configuration.external_slots[0].hidden = false;
    s.publish();
    refresh();
    CHECK(name(0).text == "Replacement" && visible(0).number() == 0 && configured(0).number() == 1);
    ++s.configuration.config_revision;
    s.configuration.external_slots[0].tombstone = true;
    s.publish();
    refresh();
    CHECK(configured(0).number() == 0 && visible(0).number() == 1 && name(0).text == "Unused 1");
    const unsigned rendered = renders();
    refresh();
    CHECK(renders() == rendered && rendered == 4);
}

// The Display Order follows the saved activity order, not slot order, and
// notifies controllers only when that order changes.
void display_order_follows_activities() {
    setup(watch_television());
    CHECK(order_notifications() == 1 && display_order() == order({}));
    auto &s = saved();
    ++s.configuration.config_revision;
    s.configuration.activities.clear();
    s.configuration.external_slots.clear();
    for (unsigned n = 0; n < 3; ++n) add(s.configuration, 10 + n, "Activity " + std::to_string(n));
    s.configuration.activity_order = {activity(12), activity(10), activity(11)};
    s.publish();
    refresh();
    CHECK(order_notifications() == 2 && display_order() == order({3, 1, 2}));
    ++s.configuration.presentation_revision;
    s.configuration.activity_order = {activity(11), activity(12), activity(10)};
    s.publish();
    refresh();
    CHECK(order_notifications() == 3 && display_order() == order({2, 3, 1}));
    ++s.configuration.presentation_revision;
    s.publish();
    refresh();
    CHECK(order_notifications() == 3);
    ++s.configuration.config_revision;
    s.configuration.external_slots[2].tombstone = true;
    s.configuration.activities.pop_back();
    s.configuration.activity_order = {activity(11), activity(10)};
    s.publish();
    refresh();
    CHECK(order_notifications() == 4 && display_order() == order({2, 1}));
}

// A Home app rename or visibility change saves the saved configuration's
// slot, which every reader of input names shares. A name equal to the
// activity's own clears the alias, so a later activity rename shows.
void home_app_edits() {
    setup(two_activities());
    auto &s = saved();
    const auto before = s.configuration.revisions();
    CHECK(edit_name(1, "Couch") == HAP_STATUS_SUCCESS && s.activations == 1 && s.expected == before);
    CHECK(s.configuration.external_slots[1].alias == "Couch" && name(1).text == "Couch" && label(1).text == "Couch");
    CHECK(s.configuration.presentation_revision == before.presentation_revision + 1);
    CHECK(edit_visibility(1, 1) == HAP_STATUS_SUCCESS && s.activations == 2);
    CHECK(s.configuration.external_slots[1].hidden && s.configuration.external_slots[1].alias == "Couch");
    CHECK(visible(1).number() == 1 && target_visible(1).number() == 1);
    CHECK(edit_visibility(1, 0) == HAP_STATUS_SUCCESS && s.activations == 3 && !s.configuration.external_slots[1].hidden);
    CHECK(visible(1).number() == 0 && target_visible(1).number() == 0);
    CHECK(edit_name(1, "Activity 1") == HAP_STATUS_SUCCESS && s.activations == 4);
    CHECK(s.configuration.external_slots[1].alias.empty() && name(1).text == "Activity 1");
    ++s.configuration.presentation_revision;
    s.configuration.activities[1].label = "Renamed";
    s.publish();
    refresh();
    CHECK(name(1).text == "Renamed");
    CHECK(count("config_store::read") == 2 && count("config_store::write") == 0);
}

// An unchanged edit saves nothing; an edit of an unused input or an invalid
// value is refused.
void edits_refused() {
    auto configuration = two_activities();
    configuration.external_slots[1].hidden = true;
    configuration.external_slots[1].alias = "Couch";
    setup(configuration);
    auto &s = saved();
    CHECK(edit_visibility(1, 1) == HAP_STATUS_SUCCESS && edit_name(1, "Couch") == HAP_STATUS_SUCCESS);
    CHECK(edit_name(2, "Unused") == HAP_STATUS_VAL_INVALID && edit_visibility(5, 0) == HAP_STATUS_VAL_INVALID);
    CHECK(edit_name(1, "") == HAP_STATUS_VAL_INVALID && edit_visibility(1, 2) == HAP_STATUS_VAL_INVALID);
    CHECK(edit_name(1, std::string(64, 'x').c_str()) == HAP_STATUS_VAL_INVALID);
    CHECK((s.activations == 0 && s.configuration.revisions() == RevisionPair{1, 1}));
    CHECK(edit_name(1, std::string(63, 'x').c_str()) == HAP_STATUS_SUCCESS && s.activations == 1);
}

// A refused save leaves the saved and displayed names unchanged.
void rejected_save() {
    setup(two_activities());
    auto &s = saved();
    s.activation = ESP_ERR_INVALID_STATE;
    const unsigned rendered = renders();
    CHECK(edit_name(0, "Den") == HAP_STATUS_COMM_ERR && s.activations == 1);
    CHECK(s.configuration.external_slots[0].alias.empty() && name(0).text == "Activity 0" && renders() == rendered);
    s.activation = ESP_ERR_INVALID_ARG;
    CHECK(edit_visibility(0, 1) == HAP_STATUS_VAL_INVALID && s.activations == 2 && !s.configuration.external_slots[0].hidden);
}

// A save needs a request ID from the random number generator.
void no_request_id() {
    setup(two_activities());
    install<&esp_fill_random>([](void *buffer, size_t size) { std::memset(buffer, 0, size); });
    CHECK(edit_name(0, "Den") == HAP_STATUS_COMM_ERR && saved().activations == 0);
}

// Selecting an input reaches the hub for a live slot only, even when a
// retired slot's activity still exists.
void input_selection() {
    auto configuration = two_activities();
    add(configuration, 13, "Activity 3");
    configuration.external_slots.back().tombstone = true;
    setup(configuration);
    auto &active = television().characteristic("E7");
    CHECK(hap::write(active, integer(1)) == HAP_STATUS_SUCCESS && hap::write(active, integer(2)) == HAP_STATUS_SUCCESS);
    CHECK(hap::write(active, integer(3)) == HAP_STATUS_VAL_INVALID && hap::write(active, integer(4)) == HAP_STATUS_VAL_INVALID);
    CHECK(hap::write(active, integer(17)) == HAP_STATUS_VAL_INVALID);
    CHECK((saved().selected == std::vector<unsigned>{0, 1}));
    CHECK(configured(3).number() == 0 && name(3).text == "Unused 4");
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"saved-names", saved_names},
                            {"cheap-refresh", cheap_refresh},
                            {"presentation-edits", presentation_edits},
                            {"replacement-and-tombstone", replacement_and_tombstone},
                            {"display-order", display_order_follows_activities},
                            {"home-app-edits", home_app_edits},
                            {"edits-refused", edits_refused},
                            {"rejected-save", rejected_save},
                            {"no-request-id", no_request_id},
                            {"input-selection", input_selection}});
}
