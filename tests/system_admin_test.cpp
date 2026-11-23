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
// healthy, activation changes nothing. Activation once healthy stays in
// configuration_snapshot_web_test.py: the hub refuses that save, because
// nothing has applied the passive metadata the facade compares it with.
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

// Only the original main loads the active configuration: another controller's
// record is refused and nothing is published.
void foreign_controller() {
    System system;
    system.save(direct_configuration());
    system.control.set_identity(id<m::ConsortiumId>(1), id<m::OctopusId>(3));
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
                               {"reservation-failure", reservation_failure},
                               {"commit-failure", commit_failure},
                               {"read-failure", read_failure},
                               {"foreign-controller", foreign_controller},
                               {"deferred-load", deferred_load},
                               {"passive-states", passive_states},
                               {"config-read", config_read},
                               {"config-unavailable", config_unavailable},
                               {"sanity", sanity},
                           });
}
