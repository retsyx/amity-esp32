/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The system administration API, built from the whole of
// components/web_admin/system_admin.cpp. The test includes the file, so its
// router, operation table and helpers are the firmware's own. It links the real
// request, device settings, configuration journal, replacement, configuration
// synchronization and SystemControl code. The hub behind SystemControl, the
// consortium runtime, system services and the NVS journal entry points are
// strict seams, scripted below.
#include "../components/web_admin/system_admin.cpp"
#include "core_fakes.hpp"
#include "httpd_fake.hpp"
#include "intent_configuration_fixture.hpp"
#include "seams.hpp"
#include "stall.hpp"
#include <map>
#include <tuple>

namespace {
namespace m = amity::system;
namespace c = amity::consortium;
using amity_fake::HttpRequest;
using amity_seam::install;
using web_admin::Json;

const esp_partition_t running_partition{};
const char boot_identity[] = "0000000000000000000000000000000c";

// What the router did with one request.
struct Reply {
    bool handled = false;
    esp_err_t result = ESP_OK;
    amity_fake::HttpResponse response;
    // How much of the request body the router read.
    std::size_t content_len = 0, offset = 0;
    unsigned receive_calls = 0;
    Json body{nullptr, cJSON_Delete};

    cJSON *item(const char *key) const { return cJSON_GetObjectItemCaseSensitive(body.get(), key); }
    std::string text(const char *key) const {
        const auto *value = item(key);
        return cJSON_IsString(value) ? value->valuestring : "<not a string>";
    }
    bool flag(const char *key) const { return cJSON_IsTrue(item(key)); }
    // Nothing of the request body was read: no receive and no body deadline.
    bool unread() const {
        return receive_calls == 0 && offset == 0 && amity_fake::timers().empty();
    }
};

// Hands one request to the administration router, as web_admin.cpp hands it
// every authenticated API request. content_len, when given, replaces the
// body's length, so a request can claim more than it carries.
Reply send(int method, const std::string &path, const std::string &body = {},
           std::optional<std::size_t> content_len = {}) {
    HttpRequest req(method, path, body);
    req.socket = 7;
    if (content_len) req.content_len = *content_len;
    Reply reply;
    reply.handled = web_admin::admin_request(&req, path, reply.result);
    reply.response = *req.response;
    reply.content_len = req.content_len;
    reply.offset = req.offset;
    reply.receive_calls = req.receive_calls;
    reply.body.reset(cJSON_Parse(reply.response.body.c_str()));
    return reply;
}

// A structured rejection: the status, a closed connection, and the JSON body
// with the code and the same message twice.
bool rejected(const Reply &reply, const char *code, const char *status, bool retryable = false) {
    const auto *affected = reply.item("affected_ids");
    return reply.handled && reply.result == ESP_FAIL && reply.response.status == status &&
           reply.response.header("Connection") == "close" && reply.response.type == "application/json" &&
           reply.text("code") == code && !reply.text("message").empty() && reply.text("error") == reply.text("message") &&
           cJSON_IsBool(reply.item("retryable")) && reply.flag("retryable") == retryable && cJSON_IsArray(affected) &&
           cJSON_GetArraySize(affected) == 0 && reply.response.sends == 1;
}

// An operation's progress, as POST and GET /api/operations/<id> report it.
bool tracked(const Reply &reply, const std::string &operation, const char *stage) {
    return reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK" &&
           !reply.response.has_header("Connection") && reply.text("operation_id") == operation &&
           reply.text("stage") == stage && cJSON_IsFalse(reply.item("retryable")) &&
           reply.text("boot_id") == boot_identity && cJSON_IsBool(reply.item("rebooting"));
}

// The original main as main.cpp starts it: a SystemControl facade over a
// scripted hub, the saved configuration journal behind the NVS entry points,
// and a consortium runtime that reports this Amity as the coordinator.
struct System {
    amity::SystemControl control;

    // The hub, idle, with nothing applied until it admits a configuration.
    // Like HubService, configure_scoped fails with reservation when that
    // names a failure, refuses a configuration unless the facade's commit
    // condition holds for its state, and otherwise runs the facade's commit.
    // The owner then applies the new revision at once unless apply is false.
    amity::Snapshot hub;
    esp_err_t reservation = ESP_OK, authority = ESP_OK;
    bool apply = true;
    // What happened, in order: reserve and publish from the hub, commit from
    // the journal, enable and disable from coordinator authority changes, and
    // whatever a scenario's own seams add.
    std::vector<std::string> events;

    // The NVS journal: a real ConfigurationStore over memory.
    struct Backend : Memory {
        bool fail_marker = false;
        bool write(const char *key, const std::string &value) override {
            if (fail_marker && (std::string(key) == "cm0" || std::string(key) == "cm1")) {
                ++writes;
                return false;
            }
            return Memory::write(key, value);
        }
    } memory;
    std::unique_ptr<ConfigurationStore> journal = std::make_unique<ConfigurationStore>(memory);
    // The saved record load_system_configuration reads.
    ConfigurationSnapshot saved;
    esp_err_t read_error = ESP_OK, stage_error = ESP_OK, commit_error = ESP_OK;
    std::function<void()> on_commit;

    // This image: pending verification after an update, and its health.
    bool pending_image = false, healthy = true;

    // The consortium runtime.
    c::RuntimeStatus status;
    std::vector<c::RuntimePeerStatus> peers;
    std::vector<std::pair<m::OctopusId, std::string>> names;
    c::RuntimeAdmissionStatus admission;

    // Runs while a response is sent, after its bytes are fixed.
    std::function<void()> during_send;

    explicit System(uint8_t tentacles = 1) {
        status.initialized = status.coordinator = status.authority_ready = true;
        status.membership_revision = 7;
        install_hub();
        install_storage();
        install<&c::runtime_status>([this] { return status; });
        install<&c::runtime_peers>([this] { return peers; });
        install<&c::runtime_known_hostnames>([this] { return names; });
        install<&c::runtime_admission_status>([this] { return admission; });
        install<&system_services::boot_id>([] { return boot_identity; });
        install<&system_services::firmware_ready>([this] { return healthy; });
        install<&esp_ota_get_running_partition>([] { return &running_partition; });
        install<&esp_ota_get_state_partition>([this](const esp_partition_t *partition, esp_ota_img_states_t *state) {
            CHECK(partition == &running_partition);
            *state = pending_image ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;
            return ESP_OK;
        });
        install<&amity_seam::constructor<bool, web::stall::Response, httpd_req_t *, size_t>>(
            [this](web::stall::Response &, httpd_req_t *, size_t) {
                if (during_send) during_send();
                return false;
            });
        control.set_identity(id<m::ConsortiumId>(1), id<m::OctopusId>(2));
        if (tentacles != control.supported_tentacles()) {
            const cec::BoardProfile profile{cec::BoardProfileId::Development4Tentacle, 1, tentacles, {}, -1, -1};
            CHECK(control.start(profile, device_settings::default_adapter_physical_addresses()) == ESP_OK);
        }
    }

    void install_hub() {
        using amity::HubService;
        install<&HubService::set_system_identity>([](HubService &, m::ConsortiumId, m::OctopusId) {});
        install<&HubService::start>(
            [](HubService &, const cec::BoardProfile &, const std::array<uint16_t, amity::max_tentacles> &) {
                return ESP_OK;
            });
        install<&HubService::snapshot>([this](const HubService &) { return hub; });
        install<&HubService::playback_state>([this](const HubService &) {
            amity::PlaybackState state;
            state.applied_config_revision = hub.applied_config_revision;
            state.applied_presentation_revision = hub.applied_presentation_revision;
            state.current_activity = hub.current_activity;
            return state;
        });
        install<&HubService::activity_selection>([](const HubService &) { return uint32_t(0); });
        install<&HubService::owned_snapshots>([](const HubService &) { return std::vector<m::OwnedAdapterSnapshot>{}; });
        install<&HubService::configure_scoped>(
            [this](HubService &, std::vector<amity::Activity>, std::vector<m::ScopedSelector>, HubService::OwnedSender,
                   m::OctopusId controller, uint64_t config_revision, std::function<esp_err_t()> commit,
                   uint64_t presentation_revision, std::vector<m::OctopusId>,
                   std::function<bool(const amity::Snapshot &)> can_commit) {
                events.push_back("reserve");
                if (!controller || !config_revision) return ESP_ERR_INVALID_ARG;
                if (reservation != ESP_OK) return reservation;
                if (can_commit && !can_commit(hub)) return ESP_ERR_INVALID_STATE;
                const auto before = control.configuration_snapshot();
                const auto result = commit ? commit() : ESP_OK;
                if (control.configuration_snapshot() != before) events.push_back("publish");
                if (result == ESP_OK && apply) {
                    hub.applied_config_revision = config_revision;
                    hub.applied_presentation_revision = presentation_revision;
                }
                return result;
            });
        install<&HubService::transition_coordinator_authority>([this](HubService &, bool enabled) {
            events.push_back(enabled ? "enable" : "disable");
            return authority;
        });
    }

    void install_storage() {
        install<&config_store::load_system_configuration>([this](ConfigurationSnapshot &out, std::string &) {
            if (read_error != ESP_OK) return read_error;
            out = saved;
            return out ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
        });
        install<&config_store::stage_system_configuration>(
            [this](Configuration candidate, RevisionPair expected, std::string &error) {
                if (stage_error != ESP_OK) return stage_error;
                return journal->stage(std::move(candidate), expected, error) == ConfigResult::ok ? ESP_OK : ESP_FAIL;
            });
        install<&config_store::staged_system_configuration>([this](ConfigurationSnapshot &out) {
            out = journal->staged();
            return out ? ESP_OK : ESP_ERR_INVALID_STATE;
        });
        install<&config_store::activate_system_configuration>([this](ConfigurationSnapshot &out, std::string &error) {
            events.push_back("commit");
            if (on_commit) on_commit();
            if (commit_error != ESP_OK) return commit_error;
            if (journal->commit(out, error) != ConfigResult::ok) return ESP_FAIL;
            saved = out;
            return ESP_OK;
        });
        install<&config_store::cancel_staged_configuration>([this] {
            journal->cancel_stage();
            return ESP_OK;
        });
    }

    // Saves value as the record on flash, outside any scenario seam.
    void save(Configuration value) {
        std::string error;
        CHECK(journal->stage(std::move(value), saved ? saved->configuration.revisions() : RevisionPair{}, error) ==
              ConfigResult::ok);
        CHECK(journal->commit(saved, error) == ConfigResult::ok);
    }

    // Loads the saved configuration as the validated original main does, and
    // forgets the calls that made.
    void boot() {
        CHECK(web_admin::load_configuration(control) == ESP_OK);
        CHECK(control.configuration_snapshot() == saved);
        events.clear();
        amity_seam::clear_calls();
    }

    // Starts configuration synchronization, which saves go through.
    void synchronize() {
        using amity::HubService;
        install<&HubService::set_owned_observer>(
            [](HubService &, std::function<void(const m::OwnedAdapterSnapshot &)> observer) { CHECK(bool(observer)); });
        install<&HubService::set_owned_authorizer>(
            [](HubService &, std::function<bool(const m::OwnedAuthorization &)> check) { CHECK(bool(check)); });
        install<&HubService::on_configuration_applied>([](HubService &, std::function<void()> callback) {
            CHECK(bool(callback));
        });
        install<&c::runtime_control_hooks>([](c::RuntimeControlHooks hooks) { CHECK(bool(hooks.control)); });
        CHECK(amity::start_configuration_synchronization(control) == ESP_OK);
        amity_seam::clear_calls();
    }

    std::shared_ptr<const Configuration> active() const { return control.active_configuration(); }
};

// Structured rejections

void error_response() {
    System system;
    HttpRequest req(HTTP_POST, "/api/membership/scan");
    req.content_len = 16000000;
    const std::string message = "A quoted \"device\"\nneeds review";
    CHECK(web_admin::error(&req, "coordinator_required", message, "403 Forbidden", true) == ESP_FAIL);
    const auto &response = *req.response;
    CHECK(response.status == "403 Forbidden" && response.header("Connection") == "close");
    CHECK(response.type == "application/json" && response.header("Cache-Control") == "no-store");
    CHECK(req.receive_calls == 0 && req.offset == 0 && req.content_len == 16000000 && amity_fake::timers().empty());
    Json body(cJSON_Parse(response.body.c_str()), cJSON_Delete);
    CHECK(body && web_admin::string(body.get(), "code") == "coordinator_required");
    CHECK(web_admin::string(body.get(), "message") == message && web_admin::string(body.get(), "error") == message);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body.get(), "retryable")));
    const auto *affected = cJSON_GetObjectItemCaseSensitive(body.get(), "affected_ids");
    CHECK(cJSON_IsArray(affected) && cJSON_GetArraySize(affected) == 0);
    CHECK(cJSON_GetArraySize(body.get()) == 5);

    HttpRequest missing(HTTP_GET, "/api/operations/0");
    CHECK(web_admin::error(&missing, "not_found", "Unknown operation.", "404 Not Found") == ESP_FAIL);
    CHECK(missing.response->status == "404 Not Found" && missing.response->header("Connection") == "close");
    CHECK(missing.receive_calls == 0);
    Json second(cJSON_Parse(missing.response->body.c_str()), cJSON_Delete);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(second.get(), "retryable")));
}

void rejection_before_body() {
    System system;
    system.save(direct_configuration());
    const std::string body = "{\"request_id\":\"" + id<m::MessageId>(1).str() + "\"}";
    // Before startup loads the configuration.
    auto reply = send(HTTP_POST, "/api/membership/scan", body, 16000000);
    CHECK(rejected(reply, "not_ready", "503 Service Unavailable", true) && reply.unread());
    CHECK(reply.content_len == 16000000);
    // While the configuration waits for this image's health.
    CHECK(web_admin::load_configuration(system.control, true) == ESP_OK);
    reply = send(HTTP_POST, "/api/membership/scan", body, 16000000);
    CHECK(rejected(reply, "coordinator_required", "403 Forbidden") && reply.unread());
    CHECK(amity_seam::count("amity::consortium::runtime_scan_admission") == 0);
}

// Configuration loading, deferred activation and reads

unsigned calls(const char *seam) { return unsigned(amity_seam::count(seam)); }
unsigned serializations() { return calls("config_store::serialize_configuration"); }
unsigned stages() { return calls("config_store::stage_system_configuration"); }
unsigned commits() { return calls("config_store::activate_system_configuration"); }

// An unchanged saved record is published as the very snapshot storage
// returned: nothing staged, committed or serialized, whatever the image state,
// and nothing is left to activate.
void retained_configuration(bool pending_image) {
    System system;
    system.save(direct_configuration());
    system.pending_image = pending_image;
    const auto current = system.saved;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    CHECK(calls("config_store::load_system_configuration") == 1 && stages() == 0 && commits() == 0);
    CHECK(system.control.configuration_snapshot() == current);
    CHECK(system.control.active_configuration().get() == &current->configuration);
    CHECK(!web_admin::deferred_configuration && !web_admin::configuration_passive && !web_admin::configuration_is_passive());
    CHECK(serializations() == 0 && calls("esp_ota_get_state_partition") == 0);
    CHECK(system.events == std::vector<std::string>({"reserve", "publish"}));
    CHECK(web_admin::finish_configuration_activation() == ESP_OK && system.events.size() == 2 && commits() == 0);
}
void retained() { retained_configuration(false); }
void retained_pending_image() { retained_configuration(true); }

// More hardware capacity is staged and committed before it is published.
void hardware_change() {
    System system(4);
    system.save(direct_configuration());
    const auto old = system.saved;
    ConfigurationSnapshot staged;
    system.on_commit = [&] {
        staged = system.journal->staged();
        CHECK(staged && !system.control.configuration_snapshot());
    };
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    CHECK(stages() == 1 && commits() == 1);
    CHECK(system.events == std::vector<std::string>({"reserve", "commit", "publish"}));
    const auto published = system.control.configuration_snapshot();
    CHECK(published == staged && published == system.saved && published != old);
    CHECK(old->configuration.octopuses.front().supported_tentacles == 1);
    CHECK(published->configuration.octopuses.front().supported_tentacles == 4);
    CHECK(!web_admin::deferred_configuration && !web_admin::configuration_passive);
}

// First boot creates the configuration for this controller and publishes the
// committed record.
void initial_configuration() {
    System system;
    ConfigurationSnapshot staged;
    system.on_commit = [&] { staged = system.journal->staged(); };
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    CHECK(stages() == 1 && commits() == 1);
    const auto published = system.control.configuration_snapshot();
    CHECK(published && published == system.saved && published == staged);
    CHECK(published->configuration.controller_octopus_id == system.control.octopus_id());
    CHECK(published->configuration.consortium_id == system.control.consortium_id());
    CHECK(system.events == std::vector<std::string>({"reserve", "commit", "publish"}));
}

// A staged configuration waits for an updated image's health. Coordinator
// authority is withdrawn first, the candidate is published only as passive
// metadata and the saved record keeps its revision. Until the image is
// healthy, activation changes nothing.
void pending_image() {
    System system(4);
    system.save(direct_configuration());
    const auto old = system.saved;
    system.pending_image = true;
    system.healthy = false;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    const auto candidate = system.journal->staged();
    CHECK(candidate && stages() == 1 && commits() == 0 && calls("esp_ota_get_state_partition") == 1);
    CHECK(system.events == std::vector<std::string>({"disable"}));
    CHECK(web_admin::configuration_passive && web_admin::configuration_is_passive() && !system.control.accepts_local_input());
    CHECK(web_admin::deferred_configuration == candidate && system.control.configuration_snapshot() == candidate);
    CHECK(candidate->configuration.octopuses.front().supported_tentacles == 4);
    CHECK(system.saved == old && old->configuration.octopuses.front().supported_tentacles == 1);
    CHECK(web_admin::finish_configuration_activation() == ESP_ERR_INVALID_STATE);
    CHECK(commits() == 0 && system.saved == old && system.events.size() == 1);
    CHECK(web_admin::deferred_configuration == candidate && web_admin::configuration_passive);
}

// Once the updated image is healthy, activation restores coordinator
// authority and commits the deferred record, which the owner then applies,
// and changes are accepted again. A failed commit withdraws authority again
// and keeps the record for the next activation. Activation happens once.
void pending_image_healthy() {
    System system(4);
    system.save(direct_configuration());
    const auto old = system.saved;
    system.pending_image = true;
    system.healthy = false;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    const auto candidate = system.journal->staged();
    CHECK(candidate && web_admin::deferred_configuration == candidate && system.hub.applied_config_revision == 0);
    system.healthy = true;
    system.commit_error = ESP_FAIL;
    system.events.clear();
    CHECK(web_admin::finish_configuration_activation() == ESP_FAIL);
    CHECK(commits() == 1 && system.saved == old && system.hub.applied_config_revision == 0);
    CHECK(system.events == std::vector<std::string>({"enable", "reserve", "commit", "disable"}));
    CHECK(web_admin::configuration_passive && web_admin::deferred_configuration == candidate);
    CHECK(system.control.configuration_snapshot() == candidate && !system.control.accepts_local_input());
    system.commit_error = ESP_OK;
    system.events.clear();
    CHECK(web_admin::finish_configuration_activation() == ESP_OK);
    CHECK(commits() == 2 && system.saved == candidate && system.control.configuration_snapshot() == candidate);
    CHECK(system.events == std::vector<std::string>({"enable", "reserve", "commit"}));
    CHECK(!web_admin::configuration_passive && !web_admin::deferred_configuration);
    CHECK(!web_admin::configuration_is_passive() && system.control.accepts_local_input());
    CHECK(system.hub.applied_config_revision == candidate->configuration.config_revision);
    CHECK(web_admin::finish_configuration_activation() == ESP_OK && commits() == 2 && system.events.size() == 3);
    CHECK(old->configuration.octopuses.front().supported_tentacles == 1);
}

// The same for a first configuration staged on an updated image.
void pending_initial_healthy() {
    System system;
    system.pending_image = true;
    system.healthy = false;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    const auto candidate = system.journal->staged();
    CHECK(candidate && !system.saved && web_admin::deferred_configuration == candidate);
    system.healthy = true;
    CHECK(web_admin::finish_configuration_activation() == ESP_OK);
    CHECK(commits() == 1 && system.saved == candidate && system.control.configuration_snapshot() == candidate);
    CHECK(!web_admin::configuration_passive && !web_admin::deferred_configuration);
    CHECK(system.hub.applied_config_revision == candidate->configuration.config_revision);
}

// A failed reservation or commit keeps the snapshot already published and
// the saved record.
void failed_activation(bool reservation) {
    System system(4);
    system.save(direct_configuration());
    const auto old = system.saved;
    CHECK(system.control.configure(old) == ESP_OK && system.control.configuration_snapshot() == old);
    if (reservation) system.reservation = ESP_FAIL;
    else system.commit_error = ESP_FAIL;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_FAIL);
    CHECK(system.control.configuration_snapshot() == old && system.saved == old);
    CHECK(stages() == 1 && commits() == (reservation ? 0u : 1u));
}
void reservation_failure() { failed_activation(true); }
void commit_failure() { failed_activation(false); }

// A record that cannot be read publishes nothing and changes nothing.
void read_failure() {
    System system;
    system.save(direct_configuration());
    system.read_error = ESP_FAIL;
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_FAIL);
    CHECK(!system.control.configuration_snapshot() && stages() == 0 && commits() == 0 && system.events.empty());
}

// Only the original main loads the active configuration: a record that names
// another controller is refused, even on an Amity it lists, and nothing is
// published.
void foreign_controller() {
    System system;
    auto config = direct_configuration();
    config.octopuses.push_back({id<m::OctopusId>(9), 1});
    system.save(config);
    system.control.set_identity(id<m::ConsortiumId>(1), id<m::OctopusId>(9));
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control) == ESP_ERR_INVALID_STATE);
    CHECK(!system.control.configuration_snapshot() && stages() == 0 && system.events.empty());
}

// A deferred load reads nothing and leaves the API passive.
void deferred_load() {
    System system;
    system.save(direct_configuration());
    amity_seam::clear_calls();
    CHECK(web_admin::load_configuration(system.control, true) == ESP_OK);
    CHECK(web_admin::configuration_passive && web_admin::configuration_is_passive());
    CHECK(calls("config_store::load_system_configuration") == 0 && !system.control.configuration_snapshot());
    CHECK(system.events.empty());
}

// Changes are refused while startup defers the configuration, and while this
// Amity is not the coordinator with authority.
void passive_states() {
    System system;
    system.save(direct_configuration());
    system.boot();
    CHECK(!web_admin::configuration_is_passive());
    system.status.authority_ready = false;
    CHECK(web_admin::configuration_is_passive());
    system.status.authority_ready = true;
    system.status.coordinator = false;
    CHECK(web_admin::configuration_is_passive());
    system.status.coordinator = true;
    CHECK(!web_admin::configuration_is_passive());
    CHECK(web_admin::load_configuration(system.control, true) == ESP_OK && web_admin::configuration_is_passive());
}

// GET /api/config serves the published record's canonical bytes without
// serializing. One read keeps its snapshot while a later save is published.
void config_read() {
    System system;
    system.save(direct_configuration());
    system.boot();
    auto renamed = system.saved->configuration;
    renamed.activities[0].label = "Renamed";
    ++renamed.presentation_revision;
    const auto next = saved_snapshot(renamed);
    auto previous = system.control.configuration_snapshot();
    const auto bytes = previous->canonical;
    std::weak_ptr<const PreparedConfiguration> retained = previous;
    amity_seam::clear_calls();
    system.during_send = [&] {
        CHECK(system.control.configure(next) == ESP_OK && system.control.configuration_snapshot() == next);
        system.saved.reset();
        system.journal.reset();
        previous.reset();
        CHECK(!retained.expired());
        system.during_send = nullptr;
    };
    auto reply = send(HTTP_GET, "/api/config");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK");
    CHECK(reply.response.body == bytes && retained.expired() && serializations() == 0);
    CHECK(reply.response.type == "application/json");
    reply = send(HTTP_GET, "/api/config");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.body == next->canonical && serializations() == 0);
}

// Without a published configuration, GET /api/config needs recovery.
void config_unavailable() {
    System system;
    system.save(direct_configuration());
    CHECK(web_admin::load_configuration(system.control, true) == ESP_OK);
    const auto reply = send(HTTP_GET, "/api/config");
    CHECK(rejected(reply, "configuration_unavailable", "409 Conflict"));
}

// Adding an Amity found by a search

std::string operation_path(unsigned request) { return "/api/operations/" + id<m::MessageId>(request).str(); }

std::string addition(unsigned request, const std::string &name = "office", const std::string &extra = "") {
    return "{\"request_id\":\"" + id<m::MessageId>(request).str() + "\",\"scan_id\":\"" + id<m::MessageId>(2).str() +
           "\",\"candidate_token\":\"opaque-public-token\",\"name\":\"" + name + "\"" + extra + "}";
}

std::string print(const Json &root) {
    char *text = cJSON_PrintUnformatted(root.get());
    std::string out = text;
    cJSON_free(text);
    return out;
}

// The request with one field replaced, or removed when value is null.
std::string addition_with(const char *field, cJSON *value) {
    Json root(cJSON_Parse(addition(1).c_str()), cJSON_Delete);
    if (value) cJSON_ReplaceItemInObjectCaseSensitive(root.get(), field, value);
    else cJSON_DeleteItemFromObjectCaseSensitive(root.get(), field);
    return print(root);
}

// Reads an addition as the router does: bounded parsing, unique keys and the
// admission contract.
bool parse_addition(const std::string &raw, web_admin::AdmissionSelection &selection) {
    Json root(web_admin::parse_json_input(raw), cJSON_Delete);
    std::string message = "unset";
    if (!root || !web_admin::unique_keys(root.get())) return false;
    const bool accepted = web_admin::admission_selection(root.get(), selection, message);
    CHECK(accepted ? message.empty() : !message.empty() && message != "unset");
    return accepted;
}
bool addition_accepted(const std::string &raw) {
    web_admin::AdmissionSelection selection;
    return parse_addition(raw, selection);
}

c::RuntimePeerStatus member(unsigned owner, unsigned boot, const std::string &hostname = {}) {
    c::RuntimePeerStatus value;
    value.id = id<m::OctopusId>(owner);
    value.boot_id = id<m::BootId>(boot);
    value.supported_tentacles = 1;
    value.configured_tentacles_mask = 1;
    value.ready = value.enrolled = true;
    value.hostname = hostname;
    return value;
}

// The original main with a search in progress, recording each addition it starts.
struct Search : System {
    std::vector<std::tuple<m::MessageId, std::string, std::string>> started;
    esp_err_t start_result = ESP_OK;
    Search() {
        save(direct_configuration());
        boot();
        admission.scan_id = id<m::MessageId>(2);
        admission.phase = "found";
        install<&c::runtime_add_octopus>([this](m::MessageId scan, const std::string &token, const std::string &name) {
            started.emplace_back(scan, token, name);
            return start_result;
        });
    }
    Reply add(unsigned request, const std::string &name = "office") {
        return send(HTTP_POST, "/api/membership/add", addition(request, name));
    }
    Reply progress(unsigned request) { return send(HTTP_GET, operation_path(request)); }
    web_admin::Tracked &operation(unsigned request) {
        for (auto &operation : web_admin::operations)
            if (operation.id == id<m::MessageId>(request).str()) return operation;
        amity_test::fail(__FILE__, __LINE__, "no such operation");
    }
};

// The request names exactly the current search, a public candidate token of
// 1-128 printable bytes and a hostname, trimmed and normalized.
void admission_contract() {
    System system;
    web_admin::AdmissionSelection selection;
    CHECK(parse_addition(addition(1, "  AMITY-OFFICE  "), selection) && selection.name == "amity-office");
    CHECK(selection.candidate_token == "opaque-public-token" && selection.scan_id == id<m::MessageId>(2));
    CHECK(parse_addition(addition(1, "a"), selection) && selection.name == "a");
    CHECK(addition_accepted(addition(1, std::string(32, 'n'))));
    for (const auto *name : {"", "bad name", "-bad", "bad-", "amity.room", "caf\xc3\xa9"}) CHECK(!addition_accepted(addition(1, name)));
    CHECK(!addition_accepted(addition(1, std::string(33, 'n'))) && !addition_accepted(addition(1, std::string(80, 'n'))));
    CHECK(!addition_accepted(addition(1, "office", ",\"extra\":true")));
    CHECK(!addition_accepted(addition(1, "office", ",\"name\":\"other\"")));
    CHECK(!addition_accepted(addition(1) + " trailing"));
    CHECK(!addition_accepted(addition(1, "office", ",\"setup_code\":\"12345678\"")));
    for (const auto *field : {"scan_id", "candidate_token", "name"}) {
        CHECK(!addition_accepted(addition_with(field, cJSON_CreateNumber(1))));
        CHECK(!addition_accepted(addition_with(field, nullptr)));
    }
    for (const auto &token : {std::string{}, std::string(129, 'x'), std::string("bad") + char(1) + "token",
                              std::string("bad") + char(127) + "token"})
        CHECK(!addition_accepted(addition_with("candidate_token", cJSON_CreateString(token.c_str()))));
    CHECK(addition_accepted(addition_with("candidate_token", cJSON_CreateString(std::string(128, 'x').c_str()))));
    CHECK(!addition_accepted(addition_with("scan_id", cJSON_CreateString(std::string(32, '0').c_str()))));
}

// An addition starts only after the router and the contract accept it, for
// the current search and an unused name.
void admission_start() {
    Search search;
    auto reply = search.add(10, "bad name");
    CHECK(rejected(reply, "invalid_addition", "400 Bad Request") && reply.text("message").find("1–32") != std::string::npos);
    CHECK(rejected(send(HTTP_POST, "/api/membership/add", addition(10, "office", ",\"extra\":true")), "invalid_json",
                   "400 Bad Request"));
    CHECK(rejected(send(HTTP_POST, "/api/membership/add", addition(10, "office", ",\"name\":\"other\"")), "invalid_json",
                   "400 Bad Request"));
    CHECK(rejected(send(HTTP_POST, "/api/membership/add", addition(10) + " trailing"), "invalid_json", "400 Bad Request"));
    search.admission.scan_id = id<m::MessageId>(3);
    CHECK(rejected(search.add(10), "stale_scan", "409 Conflict"));
    search.admission.scan_id = id<m::MessageId>(2);
    CHECK(rejected(search.add(10, "AMITY"), "name_in_use", "409 Conflict"));
    search.peers = {member(8, 18, "office")};
    CHECK(rejected(search.add(10), "name_in_use", "409 Conflict"));
    search.peers[0].enrolled = false;
    search.peers[0].provisional = true;
    CHECK(rejected(search.add(10), "name_in_use", "409 Conflict"));
    CHECK(search.started.empty() && web_admin::operations.empty());
    // A peer that is neither enrolled nor provisional does not hold its name.
    search.peers[0].provisional = false;
    reply = search.add(10);
    CHECK(tracked(reply, id<m::MessageId>(10).str(), "admitted") && reply.offset == reply.content_len);
    CHECK(search.started.size() == 1 && std::get<0>(search.started[0]) == id<m::MessageId>(2));
    CHECK(std::get<1>(search.started[0]) == "opaque-public-token" && std::get<2>(search.started[0]) == "office");
    const auto &operation = search.operation(10);
    CHECK(operation.scan_id == id<m::MessageId>(2) && operation.name == "office" && !operation.candidate);
    // An addition that cannot start is not tracked, and an ID is used once.
    search.start_result = ESP_FAIL;
    CHECK(rejected(search.add(11, "den"), "enrollment_unavailable", "409 Conflict", true));
    CHECK(rejected(search.add(10, "den"), "request_conflict", "409 Conflict"));
    CHECK(search.started.size() == 2 && web_admin::operations.size() == 1);
}

// Progress follows the search to its exact candidate, ready under the
// requested name; a finished addition keeps its result.
void admission_completes() {
    Search search;
    CHECK(tracked(search.add(10), id<m::MessageId>(10).str(), "admitted"));
    search.admission.phase = "connecting";
    CHECK(tracked(search.progress(10), id<m::MessageId>(10).str(), "admitted") && !search.operation(10).candidate);
    search.admission.candidate_id = id<m::OctopusId>(9);
    search.peers = {member(9, 19, "office")};
    search.admission.phase = "finishing";
    CHECK(tracked(search.progress(10), id<m::MessageId>(10).str(), "admitted"));
    CHECK(search.operation(10).candidate == id<m::OctopusId>(9));
    search.admission.phase = "complete";
    search.peers[0].ready = false;
    CHECK(tracked(search.progress(10), id<m::MessageId>(10).str(), "admitted"));
    search.peers[0].ready = true;
    search.peers[0].provisional = true;
    CHECK(tracked(search.progress(10), id<m::MessageId>(10).str(), "admitted"));
    search.peers[0].provisional = false;
    auto reply = search.progress(10);
    CHECK(tracked(reply, id<m::MessageId>(10).str(), "complete") && reply.text("message").empty());
    CHECK(!reply.item("reset"));
    search.admission.phase = "failed";
    search.admission.error = "Could not connect";
    CHECK(tracked(search.progress(10), id<m::MessageId>(10).str(), "complete"));
}

// A candidate that finishes under another name fails the addition.
void admission_wrong_name() {
    Search search;
    CHECK(tracked(search.add(10, "den"), id<m::MessageId>(10).str(), "admitted"));
    search.admission.candidate_id = id<m::OctopusId>(9);
    search.admission.phase = "complete";
    search.peers = {member(9, 19, "office")};
    const auto reply = search.progress(10);
    CHECK(tracked(reply, id<m::MessageId>(10).str(), "failed"));
    CHECK(reply.text("message") == "The added Amity did not confirm its requested name.");
}

// After the search changes, an addition completes only when its candidate is
// confirmed ready under its name, and is unknown otherwise.
void admission_scan_change() {
    Search search;
    CHECK(tracked(search.add(10), id<m::MessageId>(10).str(), "admitted"));
    CHECK(tracked(search.add(11), id<m::MessageId>(11).str(), "admitted"));
    CHECK(tracked(search.add(12, "den"), id<m::MessageId>(12).str(), "admitted"));
    // Additions 10 and 12 learn the candidate; 11 is not asked again.
    search.admission.candidate_id = id<m::OctopusId>(9);
    search.admission.phase = "connecting";
    for (unsigned request : {10u, 12u}) CHECK(tracked(search.progress(request), id<m::MessageId>(request).str(), "admitted"));
    CHECK(search.operation(10).candidate == id<m::OctopusId>(9) && !search.operation(11).candidate);
    CHECK(search.operation(12).candidate == id<m::OctopusId>(9));
    search.admission = {};
    search.admission.scan_id = id<m::MessageId>(3);
    search.peers = {member(9, 19, "office")};
    auto reply = search.progress(10);
    CHECK(tracked(reply, id<m::MessageId>(10).str(), "complete") && reply.text("message").empty());
    for (unsigned unconfirmed : {11u, 12u}) {
        reply = search.progress(unconfirmed);
        CHECK(tracked(reply, id<m::MessageId>(unconfirmed).str(), "unknown"));
        CHECK(reply.text("message").find("Check the Amity devices list") != std::string::npos);
    }
}

// A failed or cancelled search ends its additions with its own result.
void admission_failure() {
    Search search;
    CHECK(tracked(search.add(10), id<m::MessageId>(10).str(), "admitted"));
    CHECK(tracked(search.add(11), id<m::MessageId>(11).str(), "admitted"));
    search.admission.phase = "failed";
    search.admission.error = "Could not connect";
    auto reply = search.progress(10);
    CHECK(tracked(reply, id<m::MessageId>(10).str(), "failed") && reply.text("message") == "Could not connect");
    search.admission.phase = "cancelled";
    search.admission.error.clear();
    reply = search.progress(11);
    CHECK(tracked(reply, id<m::MessageId>(11).str(), "cancelled") && reply.text("message").empty());
}

// GET /api/membership/admission reports the search with each candidate's
// public token and label, and nothing about setup codes.
void admission_status() {
    Search search;
    search.admission.candidates = {{"token", "Amity <nearby>"}};
    auto reply = send(HTTP_GET, "/api/membership/admission");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK");
    CHECK(reply.text("scan_id") == id<m::MessageId>(2).str() && reply.text("phase") == "found");
    CHECK(reply.text("error").empty() && cJSON_IsNull(reply.item("candidate_id")) && !reply.item("requires_setup_code"));
    const auto *candidates = reply.item("candidates");
    CHECK(cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) == 1);
    auto *candidate = cJSON_GetArrayItem(candidates, 0);
    CHECK(web_admin::string(candidate, "token") == "token" && web_admin::string(candidate, "label") == "Amity <nearby>");
    CHECK(cJSON_GetArraySize(candidate) == 2);
    search.admission = {};
    search.admission.phase = "failed";
    search.admission.error = "Could not connect";
    search.admission.candidate_id = id<m::OctopusId>(9);
    reply = send(HTTP_GET, "/api/membership/admission");
    CHECK(cJSON_IsNull(reply.item("scan_id")) && reply.text("phase") == "failed" && reply.text("error") == "Could not connect");
    CHECK(reply.text("candidate_id") == id<m::OctopusId>(9).str() && cJSON_GetArraySize(reply.item("candidates")) == 0);
}

// A search the runtime cannot start is refused. A full roster states how many
// Amity devices a system supports and is not worth retrying; any other refusal
// asks for a retry.
void scan_refusals() {
    System system;
    system.save(direct_configuration());
    system.boot();
    static esp_err_t refusal = ESP_OK;
    static unsigned request = 0;
    install<&c::runtime_scan_admission>([](m::MessageId scan) {
        CHECK(scan == id<m::MessageId>(request));
        return refusal;
    });
    const auto scan = [] {
        ++request;
        return send(HTTP_POST, "/api/membership/scan", "{\"request_id\":\"" + id<m::MessageId>(request).str() + "\"}");
    };
    refusal = ESP_ERR_INVALID_SIZE;
    auto reply = scan();
    CHECK(rejected(reply, "capacity_reached", "409 Conflict"));
    CHECK(reply.text("message") == "Up to 4 Amity devices are supported. Remove one before adding another.");
    for (const esp_err_t other : {ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_FAIL}) {
        refusal = other;
        reply = scan();
        CHECK(rejected(reply, "scan_unavailable", "409 Conflict", true));
        CHECK(reply.text("message") == "The search could not start. Close this window and try again.");
    }
    CHECK(amity_seam::count("amity::consortium::runtime_scan_admission") == 4 && web_admin::operations.empty());
}

// Removing and replacing an added Amity

const config_store::ConfiguredActivity &activity(const Configuration &config, const std::string &label) {
    for (const auto &value : config.activities)
        if (value.label == label) return value;
    amity_test::fail(__FILE__, __LINE__, "no such activity");
}

// The original main with an added member, 9, whose adapter holds the source
// of the Watch activity. Console uses only the main's equipment. The runtime
// revokes, reports and forgets the member as the scenario scripts.
struct Removal : System {
    std::string removal_stage = "admitted", reset = "pending";
    esp_err_t revocation = ESP_OK;
    bool forget_fails = false;
    explicit Removal(bool enrolled = true) {
        auto config = switch_configuration(direct_configuration());
        const auto owner = id<m::OctopusId>(9);
        config.octopuses.push_back({owner, 1});
        for (auto &component : config.components)
            if (component.record.component_id == activity(config, "Watch").source)
                component.record.qualified_name->port.octopus_id = owner;
        save(config);
        boot();
        if (enrolled) peers = {member(9, 19, "member")};
        names = {{owner, "member"}};
        synchronize();
        install<&c::runtime_remove_octopus>([this](m::OctopusId octopus, m::MessageId request, uint64_t expected,
                                                   std::optional<m::BootId> boot) {
            events.push_back("revoke");
            CHECK(octopus == id<m::OctopusId>(9) && request == id<m::MessageId>(200));
            if (revocation != ESP_OK) return revocation;
            if (expected != status.membership_revision) return ESP_ERR_INVALID_STATE;
            const auto peer = std::find_if(peers.begin(), peers.end(), [&](const auto &p) { return p.id == octopus; });
            if (peer == peers.end() || (boot && *boot != peer->boot_id)) return ESP_ERR_INVALID_STATE;
            ++status.membership_revision;
            peers.erase(peer);
            return ESP_OK;
        });
        install<&c::runtime_removal_operation>([this](m::OctopusId octopus, m::MessageId request) {
            CHECK(octopus == id<m::OctopusId>(9) && request == id<m::MessageId>(200));
            return "{\"stage\":\"" + removal_stage + "\",\"reset\":\"" + reset + "\"}";
        });
        install<&c::runtime_forget_removed_octopus>([this](m::OctopusId octopus) {
            events.push_back("forget");
            if (forget_fails) return ESP_FAIL;
            CHECK(std::none_of(peers.begin(), peers.end(), [&](const auto &p) { return p.id == octopus; }));
            names.erase(std::remove_if(names.begin(), names.end(), [&](const auto &item) { return item.first == octopus; }),
                        names.end());
            return ESP_OK;
        });
    }

    // The removal of member 9 the device list sends after review, listing
    // the activities it deletes.
    Json request(bool enrolled = true, std::initializer_list<const char *> affected_labels = {"Watch"}) const {
        const auto config = active();
        Json root(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(root.get(), "request_id", id<m::MessageId>(200).str().c_str());
        cJSON_AddStringToObject(root.get(), "octopus_id", id<m::OctopusId>(9).str().c_str());
        cJSON_AddBoolToObject(root.get(), "confirm", true);
        cJSON_AddStringToObject(root.get(), "expected_membership_revision", "7");
        cJSON_AddStringToObject(root.get(), "expected_config_revision", std::to_string(config->config_revision).c_str());
        cJSON_AddStringToObject(root.get(), "expected_presentation_revision",
                                std::to_string(config->presentation_revision).c_str());
        if (enrolled) cJSON_AddStringToObject(root.get(), "expected_target_boot_id", id<m::BootId>(19).str().c_str());
        else cJSON_AddNullToObject(root.get(), "expected_target_boot_id");
        auto *affected = cJSON_AddArrayToObject(root.get(), "affected_activity_ids");
        for (const auto *label : affected_labels)
            cJSON_AddItemToArray(affected, cJSON_CreateString(activity(*config, label).activity_id.str().c_str()));
        return root;
    }
    static cJSON *port(const m::CecPortRef &value) {
        auto *out = cJSON_Parse(m::name_json({value, "OSD"}).c_str());
        cJSON_DeleteItemFromObjectCaseSensitive(out, "osd_name");
        return out;
    }
    // Replacing member 9 by member 10, mapping each of 9's connectors and
    // pieces of equipment to the same place on 10.
    Json replacement() const {
        auto root = request();
        for (const auto *field : {"octopus_id", "confirm", "affected_activity_ids"})
            cJSON_DeleteItemFromObjectCaseSensitive(root.get(), field);
        cJSON_AddStringToObject(root.get(), "previous_octopus_id", id<m::OctopusId>(9).str().c_str());
        cJSON_AddStringToObject(root.get(), "replacement_octopus_id", id<m::OctopusId>(10).str().c_str());
        cJSON_AddBoolToObject(root.get(), "old_hardware_isolated", true);
        auto *ports = cJSON_AddArrayToObject(root.get(), "ports");
        auto *equipment = cJSON_AddArrayToObject(root.get(), "equipment");
        for (const auto &component : active()->components) {
            if (!component.record.qualified_name || component.record.qualified_name->port.octopus_id != id<m::OctopusId>(9))
                continue;
            const auto &previous = *component.record.qualified_name;
            auto next = previous;
            next.port.octopus_id = id<m::OctopusId>(10);
            auto *mapping = cJSON_CreateObject();
            cJSON_AddItemToObject(mapping, "previous", port(previous.port));
            cJSON_AddItemToObject(mapping, "replacement", port(next.port));
            cJSON_AddItemToArray(ports, mapping);
            auto *selection = cJSON_CreateObject();
            cJSON_AddStringToObject(selection, "component_id", component.record.component_id.str().c_str());
            cJSON_AddItemToObject(selection, "previous", cJSON_Parse(m::name_json(previous).c_str()));
            cJSON_AddItemToObject(selection, "replacement", cJSON_Parse(m::name_json(next).c_str()));
            cJSON_AddBoolToObject(selection, "same_equipment_confirmed", true);
            cJSON_AddItemToArray(equipment, selection);
        }
        return root;
    }
    Reply remove(const Json &root) { return send(HTTP_POST, "/api/membership/remove", print(root)); }
    Reply replace(const Json &root) { return send(HTTP_POST, "/api/replacements/apply", print(root)); }
    Reply progress() { return send(HTTP_GET, operation_path(200)); }
    bool owner_gone() const {
        return config_store::serialize_configuration(*active()).find(id<m::OctopusId>(9).str()) == std::string::npos;
    }
};

// A removal is refused before anything changes unless its review is
// current: revisions, boot, the exact set of affected activities and the
// confirmation.
void removal_review() {
    Removal removal;
    const auto before = removal.active();
    for (const auto *field : {"expected_config_revision", "expected_presentation_revision", "expected_membership_revision"}) {
        auto root = removal.request();
        cJSON_ReplaceItemInObjectCaseSensitive(root.get(), field, cJSON_CreateString("99"));
        CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    }
    auto root = removal.request();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "affected_activity_ids", cJSON_CreateArray());
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    root = removal.request();
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root.get(), "affected_activity_ids"),
                         cJSON_CreateString(activity(*before, "Watch").activity_id.str().c_str()));
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    root = removal.request();
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root.get(), "affected_activity_ids"),
                         cJSON_CreateString(activity(*before, "Console").activity_id.str().c_str()));
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    root = removal.request();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "expected_target_boot_id",
                                           cJSON_CreateString(id<m::BootId>(99).str().c_str()));
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    root = removal.request();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "expected_target_boot_id", cJSON_CreateNull());
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    root = removal.request();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "confirm", cJSON_CreateBool(false));
    CHECK(rejected(removal.remove(root), "invalid_removal", "400 Bad Request"));
    root = removal.request();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "octopus_id", cJSON_CreateString(id<m::OctopusId>(2).str().c_str()));
    CHECK(rejected(removal.remove(root), "invalid_removal", "409 Conflict"));
    // A member still being added is reviewed by its boot like an enrolled one.
    removal.peers[0].enrolled = false;
    removal.peers[0].provisional = true;
    CHECK(rejected(removal.remove(removal.request(false)), "stale_removal", "409 Conflict"));
    removal.peers[0] = member(9, 19, "member");
    // A device that is neither enrolled, named nor saved is not removable.
    root = removal.request(false, {});
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "octopus_id", cJSON_CreateString(id<m::OctopusId>(11).str().c_str()));
    CHECK(rejected(removal.remove(root), "stale_removal", "409 Conflict"));
    CHECK(calls("amity::consortium::runtime_remove_octopus") == 0 && calls("amity::consortium::runtime_forget_removed_octopus") == 0);
    CHECK(removal.events.empty() && web_admin::operations.empty() && removal.active() == before);
    CHECK(rejected(removal.progress(), "unknown_operation", "404 Not Found"));
}

// When the owner cannot reserve the save, nothing is revoked or saved.
void removal_reservation_failure() {
    Removal removal;
    const auto before = removal.active();
    removal.reservation = ESP_ERR_NO_MEM;
    CHECK(rejected(removal.remove(removal.request()), "removal_unavailable", "409 Conflict"));
    CHECK(removal.events == std::vector<std::string>({"reserve"}) && removal.active() == before);
    CHECK(calls("config_store::cancel_staged_configuration") == 1 && !removal.journal->staged());
    CHECK(web_admin::operations.empty() && removal.names.size() == 1 && removal.peers.size() == 1);
}

// A member whose firmware cannot be reset by removal must be updated first.
void removal_update_required() {
    Removal removal;
    const auto before = removal.active();
    removal.revocation = ESP_ERR_NOT_SUPPORTED;
    const auto reply = removal.remove(removal.request());
    CHECK(rejected(reply, "update_required", "409 Conflict") && reply.text("message").find("Update this Amity") == 0);
    CHECK(removal.events == std::vector<std::string>({"reserve", "revoke"}) && removal.active() == before);
    CHECK(web_admin::operations.empty() && removal.names.size() == 1);
}

// A revoked membership whose saved cleanup fails is reported failed, never
// complete; the member's reset is followed separately.
void removal_marker_failure() {
    Removal removal;
    const auto before = removal.active();
    removal.memory.fail_marker = true;
    auto reply = removal.remove(removal.request());
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "failed") && reply.text("reset") == "pending");
    CHECK(reply.text("message").find("Membership was removed") == 0);
    CHECK(removal.events == std::vector<std::string>({"reserve", "revoke", "commit"}) && removal.active() == before);
    CHECK(removal.peers.empty() && removal.names.size() == 1);
    removal.removal_stage = "complete";
    removal.reset = "scheduled";
    reply = removal.progress();
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "failed") && reply.text("reset") == "scheduled");
}

// A removal revokes the member after the owner reserves the save and before
// it commits, then forgets its name. It deletes the activities that used the
// member's equipment, and completes once the owner applies the saved
// configuration and the member's reset is under way.
void removal_success() {
    Removal removal;
    const auto keep = activity(*removal.active(), "Console").activity_id;
    const auto applied = removal.hub;
    removal.apply = false;
    auto reply = removal.remove(removal.request());
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "configuring") && reply.text("reset") == "pending");
    CHECK(removal.events == std::vector<std::string>({"reserve", "revoke", "commit", "publish", "forget"}));
    CHECK(removal.peers.empty() && removal.names.empty() && removal.status.membership_revision == 8);
    const auto config = removal.active();
    CHECK(config->activities.size() == 1 && config->activities.front().activity_id == keep);
    CHECK(config->octopuses.size() == 1 && removal.owner_gone() && removal.control.configuration_snapshot() == removal.saved);
    removal.removal_stage = "complete";
    removal.reset = "scheduled";
    CHECK(tracked(removal.progress(), id<m::MessageId>(200).str(), "configuring"));
    removal.hub = applied;
    removal.hub.applied_config_revision = config->config_revision;
    removal.hub.applied_presentation_revision = config->presentation_revision;
    reply = removal.progress();
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "complete") && reply.text("reset") == "scheduled");
    CHECK(reply.text("message").empty());
}

// A removal that cannot erase the member's name or settings is incomplete.
void removal_forget_failure() {
    Removal removal;
    removal.forget_fails = true;
    const auto reply = removal.remove(removal.request());
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "failed") && reply.text("message").find("incomplete") != std::string::npos);
    CHECK(removal.events == std::vector<std::string>({"reserve", "revoke", "commit", "publish", "forget"}));
    CHECK(removal.owner_gone() && removal.names.size() == 1);
}

// A device that is only saved, not enrolled, is removed without a reset.
void removal_saved_only() {
    Removal removal(false);
    const auto reply = removal.remove(removal.request(false));
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "complete") && reply.text("reset") == "not_needed");
    CHECK(removal.events == std::vector<std::string>({"reserve", "commit", "publish", "forget"}));
    CHECK(removal.owner_gone() && removal.names.empty() && calls("amity::consortium::runtime_removal_operation") == 0);
}

// After the saved records were removed but the name was kept, removing the
// device again erases the name.
void removal_metadata_only() {
    Removal removal(false);
    Configuration clean;
    std::vector<m::ActivityId> affected;
    std::string error;
    CHECK(config_store::propose_octopus_removal(*removal.active(), id<m::OctopusId>(9), clean, affected, error));
    CHECK(amity::activate_distributed_configuration(clean, removal.active()->revisions(), id<m::MessageId>(201)) == ESP_OK);
    CHECK(removal.owner_gone() && removal.names.size() == 1);
    const auto reply = removal.remove(removal.request(false, {}));
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "complete") && removal.names.empty());
    CHECK(calls("amity::consortium::runtime_remove_octopus") == 0 && calls("amity::consortium::runtime_forget_removed_octopus") == 1);
}

// Replacement revokes the old member, moves its equipment to the new one
// and keeps every activity and its order.
void replacement() {
    Removal removal;
    auto expanded = *removal.active();
    expanded.octopuses.push_back({id<m::OctopusId>(10), 1});
    CHECK(amity::activate_distributed_configuration(expanded, removal.active()->revisions(), id<m::MessageId>(202)) == ESP_OK);
    removal.peers.push_back(member(10, 20, "spare"));
    const auto order = removal.active()->activity_order;
    removal.events.clear();
    auto reply = removal.replace(removal.replacement());
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "configuring") && reply.text("reset") == "pending");
    CHECK(removal.events == std::vector<std::string>({"reserve", "revoke", "commit", "publish", "forget"}));
    const auto config = removal.active();
    CHECK(config->activity_order == order && config->activities.size() == 2 && removal.owner_gone());
    CHECK(removal.names.empty() && removal.peers.size() == 1 && removal.peers[0].id == id<m::OctopusId>(10));
    removal.removal_stage = "complete";
    removal.reset = "offline";
    reply = removal.progress();
    CHECK(tracked(reply, id<m::MessageId>(200).str(), "applied") && reply.text("reset") == "offline");
}

// A replacement is refused before anything changes for the main Amity, a
// stale review or an incomplete mapping.
void replacement_review() {
    Removal removal;
    const auto before = removal.active();
    auto root = removal.replacement();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "previous_octopus_id",
                                           cJSON_CreateString(id<m::OctopusId>(2).str().c_str()));
    CHECK(rejected(removal.replace(root), "coordinator_replacement", "409 Conflict"));
    root = removal.replacement();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "expected_config_revision", cJSON_CreateString("99"));
    CHECK(rejected(removal.replace(root), "stale_revision", "409 Conflict"));
    root = removal.replacement();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "expected_target_boot_id",
                                           cJSON_CreateString(id<m::BootId>(99).str().c_str()));
    CHECK(rejected(removal.replace(root), "stale_revision", "409 Conflict"));
    root = removal.replacement();
    cJSON_ReplaceItemInObjectCaseSensitive(root.get(), "ports", cJSON_CreateArray());
    CHECK(rejected(removal.replace(root), "invalid_mapping", "400 Bad Request"));
    CHECK(removal.events.empty() && removal.active() == before && web_admin::operations.empty());
}

// The administration API surface

using Fields = std::vector<std::string>;
const std::string settings_path = "/api/devices/0123456789abcdef0123456789abcdef/settings";

// Each change names a closed set of fields, request_id among them once.
void request_fields() {
    System system;
    const std::map<std::string, Fields> routes = {
        {"/api/membership/scan", {"request_id"}},
        {"/api/membership/cancel", {"request_id", "scan_id"}},
        {"/api/membership/add", {"request_id", "scan_id", "candidate_token", "name"}},
        {"/api/membership/remove",
         {"request_id", "octopus_id", "confirm", "expected_membership_revision", "expected_target_boot_id",
          "expected_config_revision", "expected_presentation_revision", "affected_activity_ids"}},
        {"/api/replacements/apply",
         {"request_id", "expected_config_revision", "expected_presentation_revision", "expected_membership_revision",
          "expected_target_boot_id", "previous_octopus_id", "replacement_octopus_id", "old_hardware_isolated", "ports",
          "equipment"}},
        {settings_path,
         {"request_id", "expected_boot_id", "expected_settings_revision", "expected_hostname_revision", "hostname",
          "changes"}},
    };
    for (const auto &[path, expected] : routes) {
        const auto fields = web_admin::request_fields(path);
        CHECK(fields && *fields == expected && std::count(fields->begin(), fields->end(), "request_id") == 1);
    }
    CHECK(web_admin::device_settings_target(settings_path) == std::optional<std::string>("0123456789abcdef0123456789abcdef"));
    for (const auto *path : {"/api/devices/00000000000000000000000000000000/settings",
                             "/api/devices/0123456789ABCDEF0123456789abcdef/settings",
                             "/api/devices/0123456789abcdef0123456789abcdef/settings/extra",
                             "/api/devices/0123456789abcdef0123456789abcdef/settingz",
                             "/api/devicez/0123456789abcdef0123456789abcdef/settings",
                             "/api/devices/0123456789abcdef0123456789abcdeg/settings",
                             "/api/devices/0123456789abcdef/settings", "/api/unknown", "/api/system"}) {
        CHECK(!web_admin::request_fields(path) && !web_admin::device_settings_target(path));
    }
}

// The router owns exactly the administration paths; any other path, including
// one that holds an administration prefix after its start, is left to the
// caller untouched.
void administrative_paths() {
    System system;
    for (const auto *path : {"/api/setup", "/api/login", "/api/logout", "/api/password", "/api/reboot",
                             "/api/factory-reset", "/api/firmware", "/api/remotes", "/api/remotes/pair", "/api/matter",
                             "/api/matter/open", "/api/homekit", "/api/logs", "/api/activities", "/api/systems",
                             "/api/configuration", "/api/update", "/api/membership", "/api/remotes/api/membership/scan"}) {
        CHECK(!web_admin::administrative_path(path));
        const auto reply = send(HTTP_POST, path, "{}");
        CHECK(!reply.handled && !reply.response.head_sent && reply.unread());
    }
    for (const auto *path : {"/api/system", "/api/config", "/api/updates/cohort", "/api/membership/admission",
                             "/api/operations/0123456789abcdef0123456789abcdef", "/api/replacements/apply",
                             "/api/devices/0123456789abcdef0123456789abcdef/settings"})
        CHECK(web_admin::administrative_path(path));
}

// An unknown change, or another method, is refused before its body is read.
void unknown_change() {
    System system;
    system.save(direct_configuration());
    system.boot();
    for (const auto *path : {"/api/membership/unknown", "/api/system", "/api/devices/0123456789abcdef/settings"}) {
        const auto reply = send(HTTP_POST, path, "{}", 16000000);
        CHECK(rejected(reply, "not_found", "404 Not Found") && reply.unread());
    }
    auto reply = send(HTTP_PUT, "/api/membership/scan", "{}", 16000000);
    CHECK(rejected(reply, "method_not_allowed", "405 Method Not Allowed") && reply.unread());
    reply = send(HTTP_GET, "/api/membership/unknown");
    CHECK(rejected(reply, "not_found", "404 Not Found"));
}

// Update status is read with GET only.
void update_cohort() {
    System system;
    system.save(direct_configuration());
    system.boot();
    install<&system_services::updates::cohort_status_json>([] { return std::string("{\"phase\":\"idle\"}"); });
    auto reply = send(HTTP_GET, "/api/updates/cohort");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK");
    CHECK(reply.response.body == "{\"phase\":\"idle\"}" && reply.response.type == "application/json");
    reply = send(HTTP_POST, "/api/updates/cohort", "{}", 16000000);
    CHECK(rejected(reply, "not_found", "404 Not Found") && reply.unread());
    CHECK(calls("system_services::updates::cohort_status_json") == 1);
}

// GET /api/system describes this Amity, including how many Amity devices a
// system supports: the main and its roster.
void system_snapshot() {
    System system;
    system.save(direct_configuration());
    system.boot();
    const auto reply = send(HTTP_GET, "/api/system");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK");
    const auto *capabilities = reply.item("capabilities");
    const auto *supported = cJSON_GetObjectItemCaseSensitive(capabilities, "supported_octopuses");
    CHECK(cJSON_IsNumber(supported) && supported->valuedouble == 4);
    CHECK(supported->valuedouble == amity::consortium::maximum_roster_peers + 1);
    CHECK(reply.text("mode") == "coordinator" && reply.text("octopus_id") == id<m::OctopusId>(2).str());
    CHECK(reply.text("consortium_id") == id<m::ConsortiumId>(1).str() && reply.text("boot_id") == boot_identity);
    CHECK(reply.text("hostname") == "amity");
}

// GET /api/system lists every runtime peer with what the UI reads: its ID,
// readiness, boot (null until known), whether it is enrolled or still
// provisional, its tentacles, firmware and hostname, and its Wi-Fi signal
// once the peer has reported one. A candidate whose join is stored but not
// yet committed is provisional, never enrolled.
void system_peers() {
    System system;
    system.save(direct_configuration());
    system.boot();
    // Its link is up, but its management session is not ready yet.
    auto enrolled = member(3, 13, "den");
    enrolled.supported_tentacles = 4;
    enrolled.configured_tentacles_mask = 5;
    enrolled.firmware_identity = "0.1.179 c0ffee";
    enrolled.wifi_known = true;
    enrolled.wifi_rssi = -61;
    enrolled.wifi_channel = 11;
    c::RuntimePeerStatus joining;
    joining.id = id<m::OctopusId>(9);
    joining.provisional = true;
    system.peers = {enrolled, joining};
    const auto reply = send(HTTP_GET, "/api/system");
    CHECK(reply.handled && reply.result == ESP_OK && reply.response.status == "200 OK");
    const auto *peers = cJSON_GetObjectItemCaseSensitive(reply.item("membership"), "peers");
    CHECK(cJSON_IsArray(peers) && cJSON_GetArraySize(peers) == 2);
    const auto field = [](const cJSON *record, const char *key) { return cJSON_GetObjectItemCaseSensitive(record, key); };
    const auto text = [&field](const cJSON *record, const char *key) {
        const auto *value = field(record, key);
        return cJSON_IsString(value) ? std::string(value->valuestring) : "<not a string>";
    };
    const auto number = [&field](const cJSON *record, const char *key) {
        const auto *value = field(record, key);
        return cJSON_IsNumber(value) ? value->valuedouble : -1000;
    };
    const auto *first = cJSON_GetArrayItem(peers, 0);
    CHECK(text(first, "octopus_id") == id<m::OctopusId>(3).str() && text(first, "boot_id") == id<m::BootId>(13).str());
    CHECK(cJSON_IsTrue(field(first, "ready")) && cJSON_IsFalse(field(first, "management_ready")));
    CHECK(cJSON_IsTrue(field(first, "enrolled")) && cJSON_IsFalse(field(first, "provisional")));
    CHECK(number(first, "supported_tentacles") == 4 && number(first, "configured_tentacles_mask") == 5);
    CHECK(text(first, "firmware_identity") == "0.1.179 c0ffee" && text(first, "hostname") == "den");
    CHECK(number(first, "wifi_rssi") == -61 && number(first, "wifi_channel") == 11 && cJSON_GetArraySize(first) == 12);
    const auto *second = cJSON_GetArrayItem(peers, 1);
    CHECK(text(second, "octopus_id") == id<m::OctopusId>(9).str() && cJSON_IsNull(field(second, "boot_id")));
    CHECK(cJSON_IsFalse(field(second, "ready")) && cJSON_IsFalse(field(second, "management_ready")));
    CHECK(cJSON_IsFalse(field(second, "enrolled")) && cJSON_IsTrue(field(second, "provisional")));
    CHECK(number(second, "supported_tentacles") == 1 && number(second, "configured_tentacles_mask") == 1);
    CHECK(text(second, "firmware_identity").empty() && text(second, "hostname").empty());
    CHECK(!field(second, "wifi_rssi") && !field(second, "wifi_channel") && cJSON_GetArraySize(second) == 10);
}

// The harness consumes what it scripts: a request body read to its end
// under its deadline, the saved record loaded and published, the
// synchronization task created and one response sent.
void sanity() {
    System system;
    system.save(direct_configuration());
    CHECK(web_admin::load_configuration(system.control) == ESP_OK);
    CHECK(amity_seam::count("config_store::load_system_configuration") == 1);
    CHECK(system.events == std::vector<std::string>({"reserve", "publish"}));
    CHECK(system.control.configuration_snapshot() == system.saved);
    system.synchronize();
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1 && tasks[0].name == "system_sync" && tasks[0].state == amity_fake::Task::State::created);
    install<&c::runtime_scan_admission>([](m::MessageId scan) {
        CHECK(scan == id<m::MessageId>(1));
        return ESP_OK;
    });
    const std::string body = "{\"request_id\":\"" + id<m::MessageId>(1).str() + "\"}";
    const auto reply = send(HTTP_POST, "/api/membership/scan", body);
    CHECK(tracked(reply, id<m::MessageId>(1).str(), "complete"));
    CHECK(reply.offset == body.size() && reply.receive_calls >= 1 && reply.response.sends == 1);
    const auto timers = amity_fake::timers();
    CHECK(timers.size() == 1 && timers[0].name == "http_body" && timers[0].starts == 1 && timers[0].deleted);
    CHECK(amity_seam::count("amity::consortium::runtime_scan_admission") == 1);
    CHECK(amity_seam::count("web::stall::Response::Response") == 1);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {
                               {"error-response", error_response},
                               {"rejection-before-body", rejection_before_body},
                               {"retained", retained},
                               {"retained-pending-image", retained_pending_image},
                               {"hardware-change", hardware_change},
                               {"initial-configuration", initial_configuration},
                               {"pending-image", pending_image},
                               {"pending-image-healthy", pending_image_healthy},
                               {"pending-initial-healthy", pending_initial_healthy},
                               {"reservation-failure", reservation_failure},
                               {"commit-failure", commit_failure},
                               {"read-failure", read_failure},
                               {"foreign-controller", foreign_controller},
                               {"deferred-load", deferred_load},
                               {"passive-states", passive_states},
                               {"config-read", config_read},
                               {"config-unavailable", config_unavailable},
                               {"admission-contract", admission_contract},
                               {"admission-start", admission_start},
                               {"admission-completes", admission_completes},
                               {"admission-wrong-name", admission_wrong_name},
                               {"admission-scan-change", admission_scan_change},
                               {"admission-failure", admission_failure},
                               {"admission-status", admission_status},
                               {"scan-refusals", scan_refusals},
                               {"removal-review", removal_review},
                               {"removal-reservation-failure", removal_reservation_failure},
                               {"removal-update-required", removal_update_required},
                               {"removal-marker-failure", removal_marker_failure},
                               {"removal-success", removal_success},
                               {"removal-forget-failure", removal_forget_failure},
                               {"removal-saved-only", removal_saved_only},
                               {"removal-metadata-only", removal_metadata_only},
                               {"replacement", replacement},
                               {"replacement-review", replacement_review},
                               {"request-fields", request_fields},
                               {"administrative-paths", administrative_paths},
                               {"unknown-change", unknown_change},
                               {"update-cohort", update_cohort},
                               {"system-snapshot", system_snapshot},
                               {"system-peers", system_peers},
                               {"sanity", sanity},
                           });
}
