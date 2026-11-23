/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The Matter inputs and state that main/main.cpp publishes, from saved
// configurations: the file built whole, as in main_test, but with the real
// configuration codec of system_configuration.cpp, so it runs in the sdk tier.
// The original's hub is a real SystemControl that applies each save. Matter's
// side, which reports the attributes, is outside main.cpp.
#include "core_fakes.hpp"
#include "seams.hpp"
#include "main.cpp"

#include "scenario_main.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

namespace model = amity::system;
using amity_seam::install;

template <class Id> Id id(unsigned n) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", n);
    return *Id::parse(text);
}

const model::ConsortiumId consortium = id<model::ConsortiumId>(0xc0);
const model::OctopusId octopus = id<model::OctopusId>(0x0c);
const model::ActivityId watch = id<model::ActivityId>(0xa0), listen = id<model::ActivityId>(0xa1),
                        play = id<model::ActivityId>(0xa2);

// NVS for the configuration store.
struct Memory final : config_store::ConfigBackend {
    std::map<std::string, std::string> values;
    config_store::ReadResult read(const char *key, std::string &value) override {
        const auto found = values.find(key);
        if (found == values.end()) return config_store::ReadResult::missing;
        value = found->second;
        return config_store::ReadResult::found;
    }
    bool write(const char *key, const std::string &value) override {
        values[key] = value;
        return true;
    }
    bool erase(const char *key) override {
        values.erase(key);
        return true;
    }
} memory;
config_store::ConfigurationStore store(memory);
config_store::ConfigurationSnapshot saved;

// The activities the hub runs, in the order the last applied save gave them.
std::vector<amity::Activity> running;
// The states main.cpp published to Matter.
std::vector<amity_matter_state_t> published;

// The original's hub: it applies each configuration it is given.
void start_hub() {
    install<&amity::HubService::set_system_identity>([](amity::HubService &, model::ConsortiumId group,
                                                        model::OctopusId local) {
        CHECK(group == consortium && local == octopus);
    });
    install<&amity::HubService::snapshot>([](const amity::HubService &) { return amity::Snapshot{}; });
    install<&amity::HubService::configure_scoped>(
        [](amity::HubService &, std::vector<amity::Activity> activities, std::vector<model::ScopedSelector>,
           amity::HubService::OwnedSender, model::OctopusId controller, uint64_t, std::function<esp_err_t()> commit,
           uint64_t, std::vector<model::OctopusId>, std::function<bool(const amity::Snapshot &)>) {
            CHECK(controller == octopus);
            running = std::move(activities);
            return commit ? commit() : ESP_OK;
        });
    install<&amity_matter_update_state>([](const amity_matter_state_t *state) {
        published.push_back(*state);
        return ESP_OK;
    });
    hub.set_identity(consortium, octopus);
    original_interfaces_enabled.store(true);
}

config_store::ConfiguredActivity activity(model::ActivityId activity_id, const char *label) {
    config_store::ConfiguredActivity out;
    out.activity_id = activity_id;
    out.label = label;
    return out;
}
// This board's configuration: Watch in slot 2 and Listen in slot 1.
config_store::Configuration first_configuration() {
    config_store::Configuration out;
    out.consortium_id = consortium;
    out.controller_octopus_id = octopus;
    out.octopuses.push_back({octopus, 1});
    out.activities = {activity(watch, "Watch"), activity(listen, "Listen")};
    out.external_slots = {{2, watch, false, false, {}}, {1, listen, false, false, {}}};
    return out;
}
// Saves configuration through the real store, as the activity editor does.
// The store assigns its revisions.
void save(const config_store::Configuration &configuration) {
    std::string error;
    const auto staged =
        store.stage(configuration, saved ? saved->configuration.revisions() : config_store::RevisionPair{}, error);
    if (staged != config_store::ConfigResult::ok) amity_test::fail(__FILE__, __LINE__, error.c_str());
    CHECK(store.commit(saved, error) == config_store::ConfigResult::ok);
}
// The saved configuration, to edit.
config_store::Configuration current() { return saved->configuration; }
config_store::ExternalSlot &slot(config_store::Configuration &configuration, uint16_t number) {
    for (auto &each : configuration.external_slots) {
        if (each.slot == number) return each;
    }
    amity_test::fail(__FILE__, __LINE__, "no such slot");
    return configuration.external_slots.front();
}

// The hub applies the save and reports it, playing the activity at index.
amity::Snapshot applied(int index = -1) {
    CHECK(hub.configure(saved) == ESP_OK);
    amity::Snapshot snapshot;
    snapshot.applied_config_revision = saved->configuration.config_revision;
    snapshot.applied_presentation_revision = saved->configuration.presentation_revision;
    snapshot.activities = running;
    snapshot.current_activity = index;
    return snapshot;
}
// Publishes the snapshot and returns the one state main.cpp published.
amity_matter_state_t publish(const amity::Snapshot &snapshot) {
    published.clear();
    publish_matter_state(snapshot);
    CHECK(published.size() == 1);
    return published.front();
}
// Matter reads the inputs: their names, and the slots their indexes are.
struct ListedInput {
    unsigned index;
    std::string name;
    bool operator==(const ListedInput &other) const { return index == other.index && name == other.name; }
};
std::vector<ListedInput> inputs(size_t capacity = AMITY_MATTER_MAX_INPUTS) {
    std::vector<amity_matter_input_t> read(capacity);
    const size_t count = matter_inputs(read.data(), capacity, nullptr);
    CHECK(count <= capacity);
    std::vector<ListedInput> out;
    for (size_t n = 0; n < count; ++n) out.push_back({read[n].index, read[n].name});
    return out;
}
// The revisions each published state names.
void check_revisions(const amity_matter_state_t &state, uint64_t config, uint64_t presentation) {
    CHECK(state.config_revision == config && state.presentation_revision == presentation);
}

// Each applied save publishes its revisions, so Matter republishes the input
// list, and every read of the list uses the saved names: activity labels in
// slot order, with each slot as the input's index; a slot's alias in place of
// the label; neither hidden slots nor deleted activities. No web request is
// involved.
void inputs_follow_saves() {
    start_hub();
    save(first_configuration());
    check_revisions(publish(applied()), 1, 1);
    CHECK((inputs() == std::vector<ListedInput>{{1, "Listen"}, {2, "Watch"}}));
    // Renaming changes the presentation revision.
    auto renamed = current();
    renamed.activities[0].label = "Movies";
    save(renamed);
    check_revisions(publish(applied()), 1, 2);
    CHECK((inputs() == std::vector<ListedInput>{{1, "Listen"}, {2, "Movies"}}));
    auto aliased = current();
    slot(aliased, 2).alias = "Film";
    save(aliased);
    check_revisions(publish(applied()), 1, 3);
    CHECK((inputs() == std::vector<ListedInput>{{1, "Listen"}, {2, "Film"}}));
    auto hidden = current();
    slot(hidden, 1).hidden = true;
    save(hidden);
    check_revisions(publish(applied()), 1, 4);
    CHECK((inputs() == std::vector<ListedInput>{{2, "Film"}}));
    // Deleting Watch with its slot changes both revisions; the store keeps
    // the slot as a tombstone, which is not an input.
    auto deleted = current();
    const auto is_watch = [](const auto &each) { return each.activity_id == watch; };
    deleted.activities.erase(std::remove_if(deleted.activities.begin(), deleted.activities.end(), is_watch),
                             deleted.activities.end());
    deleted.external_slots.erase(
        std::remove_if(deleted.external_slots.begin(), deleted.external_slots.end(), is_watch),
        deleted.external_slots.end());
    save(deleted);
    const auto &kept = saved->configuration.external_slots;
    CHECK(std::any_of(kept.begin(), kept.end(), [](const config_store::ExternalSlot &each) {
        return each.slot == 2 && each.activity_id == watch && each.tombstone;
    }));
    check_revisions(publish(applied()), 2, 5);
    CHECK(inputs().empty());
}

// Playback publishes the current input, the slot of the playing activity, with
// the saved revisions unchanged, so Matter does not republish the input list.
void inputs_playback() {
    start_hub();
    save(first_configuration());
    const auto stopped = publish(applied());
    CHECK(!stopped.power && stopped.current_input == 0);
    CHECK(running.size() == 2 && running[0].activity_id == watch && running[1].activity_id == listen);
    const auto watching = publish(applied(0));
    CHECK(watching.power && watching.current_input == 2);
    check_revisions(watching, 1, 1);
    const auto listening = publish(applied(1));
    CHECK(listening.power && listening.current_input == 1);
    check_revisions(listening, 1, 1);
}

// A slot saved as a tombstone is not an input, even while its activity exists.
void inputs_tombstone() {
    start_hub();
    auto configuration = first_configuration();
    configuration.activities.push_back(activity(play, "Play"));
    configuration.external_slots.push_back({3, play, true, false, {}});
    save(configuration);
    applied();
    CHECK((inputs() == std::vector<ListedInput>{{1, "Listen"}, {2, "Watch"}}));
}

// Matter reads at most the inputs it has room for, in slot order, and names
// are cut to its buffer.
void inputs_capacity() {
    start_hub();
    auto configuration = first_configuration();
    const std::string long_name(100, 'p');
    configuration.activities.push_back(activity(play, long_name.c_str()));
    configuration.external_slots.push_back({0, play, false, false, {}});
    save(configuration);
    applied();
    CHECK((inputs(2) == std::vector<ListedInput>{{0, long_name.substr(0, AMITY_MATTER_INPUT_NAME_LENGTH - 1)}, {1, "Listen"}}));
    CHECK(inputs().size() == 3);
}

// The saved configuration keeps its slots in slot order, and the inputs are
// in slot order whatever order a configuration lists its slots in.
void inputs_slot_order() {
    const auto configuration = first_configuration();
    CHECK(configuration.external_slots.front().slot == 2);
    const auto listed = config_store::external_inputs(configuration);
    CHECK(listed.size() == 2 && listed[0].slot == 1 && listed[0].name == "Listen" && listed[1].slot == 2 &&
          listed[1].name == "Watch");
}

// Before any configuration is saved there are no inputs.
void inputs_unconfigured() {
    start_hub();
    CHECK(inputs().empty());
}

// Audio status is published only once observed: mute, and the CEC volume
// 0..100 scaled to Matter's 0..254; 127 means the volume is unknown.
void state_audio() {
    start_hub();
    save(first_configuration());
    auto snapshot = applied(0);
    const auto unknown = publish(snapshot);
    CHECK(!unknown.mute_known && !unknown.muted && !unknown.volume_known && unknown.volume == 0);
    snapshot.audio_status_known = true;
    snapshot.muted = true;
    snapshot.volume = 1;
    const auto quiet = publish(snapshot);
    CHECK(quiet.mute_known && quiet.muted && quiet.volume_known && quiet.volume == 3);
    snapshot.muted = false;
    snapshot.volume = 100;
    const auto loud = publish(snapshot);
    CHECK(loud.mute_known && !loud.muted && loud.volume_known && loud.volume == 254);
    snapshot.volume = 127;
    const auto unsupported = publish(snapshot);
    CHECK(unsupported.mute_known && !unsupported.volume_known && unsupported.volume == 0);
}

// Nothing is published while the original's interfaces are off.
void state_while_disabled() {
    start_hub();
    save(first_configuration());
    const auto snapshot = applied(0);
    original_interfaces_enabled.store(false);
    publish_matter_state(snapshot);
    CHECK(published.empty() && amity_seam::count("amity_matter_update_state") == 0);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv, {
        {"inputs-follow-saves", inputs_follow_saves},
        {"inputs-playback", inputs_playback},
        {"inputs-tombstone", inputs_tombstone},
        {"inputs-capacity", inputs_capacity},
        {"inputs-slot-order", inputs_slot_order},
        {"inputs-unconfigured", inputs_unconfigured},
        {"state-audio", state_audio},
        {"state-while-disabled", state_while_disabled},
    });
}
