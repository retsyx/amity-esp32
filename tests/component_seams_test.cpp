/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// Every component seam links, records its calls and fails without a behaviour.
#include "activity_editor.hpp"
#include "adapter_settings.hpp"
#include "amity/consortium/runtime.hpp"
#include "amity/system_control.hpp"
#include "amity_matter.h"
#include "ble_remote.h"
#include "config_store.hpp"
#include "crypto_fakes.hpp"
#include "device_settings.hpp"
#include "group_firmware_update.hpp"
#include "homekit.hpp"
#include "httpd_fake.hpp"
#include "member_removal.hpp"
#include "network_diagnostics.hpp"
#include "platform.hpp"
#include "radio_control.hpp"
#include "seams.hpp"
#include "system_admin.hpp"
#include "system_identity.hpp"
#include "system_services.hpp"
#include "web.hpp"
#include "web_admin.hpp"
#include "wifi_signal.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace consortium = amity::consortium;
namespace platform = amity::consortium::platform;
namespace updates = system_services::updates;
namespace model = amity::system;
using amity::HubService;
using amity::SystemControl;

template <typename T> T id(unsigned value) {
    char text[33];
    std::snprintf(text, sizeof(text), "%032x", value);
    return *T::parse(text);
}

template <typename... F> constexpr std::size_t seams(F...) { return sizeof...(F); }

// Each function the seamed headers declared when the seams were written, so
// each one has a definition: a missing seam would fail to link here.
void complete() {
    CHECK(seams(&consortium::runtime_read_startup, &consortium::runtime_set_hardware_profile, &consortium::runtime_start,
                &consortium::runtime_status, &consortium::runtime_configuration_changed,
                &consortium::runtime_hostname_changed, &consortium::runtime_claim_original_controller,
                &consortium::runtime_original_setup_eligible, &consortium::runtime_open_enrollment,
                &consortium::runtime_close_enrollment, &consortium::runtime_cancel_radio_workflows,
                &consortium::runtime_register_gatt, &consortium::runtime_admission_status,
                &consortium::runtime_scan_admission, &consortium::runtime_cancel_admission,
                &consortium::runtime_add_octopus, &consortium::runtime_remove_octopus,
                &consortium::runtime_removal_operation, &consortium::runtime_forget_removed_octopus,
                &consortium::runtime_radio_hooks, &consortium::runtime_network_hooks, &consortium::runtime_control_hooks,
                &consortium::runtime_lifecycle_hooks, &consortium::runtime_settings_hooks, &consortium::runtime_log_hooks,
                &consortium::runtime_pull_logs, &consortium::runtime_device_settings,
                &consortium::runtime_write_device_settings, &consortium::runtime_device_settings_operation,
                &consortium::runtime_device_settings_authorized, &consortium::runtime_update_hooks,
                &consortium::runtime_send_update, &consortium::runtime_stage_update, &consortium::runtime_peers,
                &consortium::runtime_known_hostnames, &consortium::runtime_send_owned,
                &consortium::runtime_send_owned_result, &consortium::runtime_fence_owned_link,
                &consortium::runtime_owned_authorized) == 39);
    CHECK(seams(&platform::sha256, &platform::random_hex, &platform::now_ms, &platform::read_record, &platform::write_record,
                &platform::load_identity, &platform::certificate, &platform::certificate_binding, &platform::load_group,
                &platform::save_group) == 10);
    CHECK(seams(&system_services::start_log_capture, &system_services::log_memory_if_due, &system_services::logs,
                &system_services::log_size, &system_services::log_snapshot, &system_services::upload_firmware,
                static_cast<void (*)()>(&system_services::reboot_later),
                static_cast<void (*)(uint32_t)>(&system_services::reboot_later), &system_services::confirm_firmware,
                &system_services::boot_id, &system_services::firmware_ready, &system_services::download_logs,
                &system_services::stop_log_downloads) == 13);
    CHECK(seams(&updates::maintenance_hooks, &updates::validate_release, &updates::enter_maintenance,
                &updates::leave_maintenance, &updates::peer_begin, &updates::peer_chunk, &updates::peer_stage,
                &updates::peer_install, &updates::partition_release, &updates::read_running_image,
                &updates::read_staged_image, &updates::peer_abort, &updates::poll_peer_timeout,
                &updates::start_group_updates, &updates::begin_system_upload, &updates::system_upload_progress,
                &updates::finish_system_upload, &updates::fail_system_upload, &updates::cohort_status_json,
                &updates::poll_group_update, &updates::wait_group_update) == 21);
    CHECK(seams(&system_services::adapter_settings::install, &system_services::adapter_settings::snapshot,
                &system_services::adapter_settings::apply, &system_services::prepare_removed_device_reset,
                &system_services::removed_device_reset_response_attempted) == 5);
    CHECK(seams(&device_settings::finish_factory_reset, &device_settings::schedule_factory_reset,
                &device_settings::reset_pending, &device_settings::start_reset_button,
                &device_settings::set_setup_button_handler, &device_settings::init, &device_settings::factory,
                &device_settings::hostname, &device_settings::hostname_settings, &device_settings::save_hostname,
                &device_settings::initial_admin_auth, &device_settings::front_physical_address,
                &device_settings::adapter_defaults, &device_settings::save_adapter_defaults,
                &device_settings::set_hostname_handler, &device_settings::normalize_hostname,
                &device_settings::apply_enrollment_hostname) == 17);
    CHECK(seams(&amity::network_diagnostics::start, &amity::network_diagnostics::snapshot,
                &amity::network_diagnostics::record_console_call, &amity::network_diagnostics::poll,
                &amity::network_diagnostics::wifi_signal, &amity::network_diagnostics::poll_wifi_signal) == 6);
    CHECK(seams(&web::start, &web::stop, &web::route, &web::start_redirect, &web::stop_redirect, &web::redirect_location,
                &web::BodyReader::receive, &web::body, &web::response, &web::json, &web::error, &web::credentials_init,
                &web::setup_required, &web::setup_password, &web::authenticated, &web::login, &web::login_retry_after,
                &web::change_password, &web::logout) == 19);
    CHECK(seams(&config_store::with_internal_flash_stack, &config_store::load_system_configuration,
                &config_store::stage_system_configuration, &config_store::activate_system_configuration,
                &config_store::staged_system_configuration, &config_store::cancel_staged_configuration,
                &config_store::load_system_identity) == 7);
    CHECK(seams(&SystemControl::transition_coordinator_authority, &SystemControl::accepts_local_input,
                static_cast<esp_err_t (SystemControl::*)(int)>(&SystemControl::request_activity), &SystemControl::press_key,
                static_cast<esp_err_t (SystemControl::*)(unsigned)>(&SystemControl::request_remote_activity),
                static_cast<esp_err_t (SystemControl::*)(unsigned, uint32_t)>(&SystemControl::request_remote_activity),
                &SystemControl::repair_remote_activity,
                static_cast<esp_err_t (SystemControl::*)(uint32_t)>(&SystemControl::stop_remote_zone),
                &SystemControl::configure, &SystemControl::configure_passive_metadata,
                static_cast<esp_err_t (SystemControl::*)(model::ActivityId, model::MessageId, std::optional<uint64_t>)>(
                    &SystemControl::request_activity),
                &SystemControl::execution_snapshot, &SystemControl::applied_configuration,
                &SystemControl::native_observations, &SystemControl::external_activity,
                static_cast<std::optional<unsigned> (SystemControl::*)(const amity::Snapshot &) const>(
                    &SystemControl::external_activity_index),
                static_cast<std::optional<unsigned> (SystemControl::*)(const amity::PlaybackState &) const>(
                    &SystemControl::external_activity_index)) == 17);
    CHECK(seams(&HubService::start, &HubService::adapter_runtime_snapshot, &HubService::commit_adapter_settings,
                static_cast<esp_err_t (HubService::*)(int, uint32_t *, std::optional<model::ActivityId>)>(
                    &HubService::request_activity),
                &HubService::repair_activity, &HubService::activity_selection, &HubService::activity_target,
                &HubService::press_key, &HubService::press_key_for_activity, &HubService::input_epoch,
                &HubService::activity_generation, &HubService::release_key, &HubService::standby, &HubService::discover,
                &HubService::activity_state, &HubService::snapshot, &HubService::on_change, &HubService::playback_state,
                &HubService::on_configuration_applied, &HubService::activity_setup_in_progress,
                &HubService::set_system_identity, &HubService::set_boot_identity, &HubService::set_coordinator_authority,
                &HubService::transition_coordinator_authority, &HubService::retire_membership,
                &HubService::begin_maintenance, &HubService::end_maintenance, &HubService::maintenance_ready,
                &HubService::configure_scoped, &HubService::submit_owned, &HubService::submit_key_edges,
                &HubService::bind_owned_controller, &HubService::set_owned_authorizer, &HubService::set_owned_observer,
                &HubService::update_owned_snapshot, &HubService::forget_owned_snapshots, &HubService::refresh_owned_context,
                &HubService::owned_snapshots, &HubService::fence_owned, &HubService::discover_owned,
                &HubService::executing_configuration_revision) == 41);
    CHECK(seams(&web_admin::load_configuration, &web_admin::configuration_is_passive,
                &web_admin::finish_configuration_activation, &web_admin::admin_request,
                &web_admin::activity_editor_request) == 5);
    CHECK(seams(&amity_matter_set_controller_enabled, &amity_matter_start, &amity_matter_controller_enrollment_complete,
                &amity_matter_update_state, &amity_matter_flush_state, &amity_matter_update_remote,
                &amity_matter_get_status, &amity_matter_open_commissioning, &amity_matter_close_commissioning,
                &amity_matter_remove_fabric) == 10);
    CHECK(seams(&amity_homekit::start, &amity_homekit::stop, &amity_homekit::rename_hostname, &amity_homekit::status,
                &amity_homekit::open_pairing, &amity_homekit::close_pairing, &amity_homekit::remove_pairings) == 7);
    CHECK(seams(&ble_remote_start, &ble_remote_set_disconnect_callback, &ble_remote_set_siri_callback,
                &ble_remote_start_pairing, &ble_remote_unpair, &ble_remote_get_status, &ble_remote_set_radio_hooks,
                &ble_remote_pause_background, &ble_remote_cancel_setup, &ble_remote_stop_pairing) == 10);
    CHECK(seams(&amity::radio::start, &amity::radio::matter_will_advertise, &amity::radio::reserve_consortium,
                &amity::radio::release) == 4);
}

void strict() { (void)consortium::runtime_status(); }

void runtime() {
    amity_seam::install<&consortium::runtime_start>(
        [](model::ConsortiumId, model::OctopusId octopus, model::BootId, uint64_t revision) {
            return octopus == id<model::OctopusId>(7) && revision == 3 ? ESP_OK : ESP_FAIL;
        });
    amity_seam::install<&consortium::runtime_status>([] {
        consortium::RuntimeStatus status;
        status.coordinator = true;
        status.phase = "ready";
        return status;
    });
    std::string sent;
    uint32_t ttl = 0;
    amity_seam::install<&consortium::runtime_send_update>(
        [&](model::OctopusId, consortium::Kind, std::string payload, uint32_t ttl_ms) {
            sent = std::move(payload);
            ttl = ttl_ms;
            return ESP_OK;
        });
    consortium::RuntimeControlHooks kept;
    amity_seam::install<&consortium::runtime_control_hooks>([&](consortium::RuntimeControlHooks hooks) { kept = hooks; });
    CHECK(consortium::runtime_start(id<model::ConsortiumId>(1), id<model::OctopusId>(7), id<model::BootId>(2), 3) == ESP_OK);
    CHECK(consortium::runtime_status().coordinator && consortium::runtime_status().phase == "ready");
    CHECK(consortium::runtime_send_update(id<model::OctopusId>(9), consortium::Kind{}, "chunk") == ESP_OK);
    CHECK(sent == "chunk" && ttl == 5000);
    consortium::RuntimeControlHooks hooks;
    hooks.disconnected = [](model::OctopusId, model::BootId, model::LinkId) { return true; };
    consortium::runtime_control_hooks(hooks);
    CHECK(kept.disconnected && !kept.synchronize);
    CHECK((amity_seam::calls() ==
           std::vector<std::string>{"amity::consortium::runtime_start", "amity::consortium::runtime_status",
                                    "amity::consortium::runtime_status", "amity::consortium::runtime_send_update",
                                    "amity::consortium::runtime_control_hooks"}));
}

void credentials() {
    amity_seam::install<&platform::now_ms>([] { return uint64_t(1234); });
    amity_seam::install<&platform::load_group>([](platform::Group &group) {
        group.role = platform::Group::coordinator;
        return ESP_OK;
    });
    amity_seam::install<&platform::read_record>([](const char *space, const char *key, std::string &out, size_t cap) {
        out = std::string(space) + "/" + key;
        return out.size() <= cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
    });
    platform::Group group;
    std::string record;
    CHECK(platform::now_ms() == 1234 && platform::load_group(group) == ESP_OK && group.role == platform::Group::coordinator);
    CHECK(platform::read_record("amity_group", "identity", record, 64) == ESP_OK && record == "amity_group/identity");
    amity_seam::install<&mbedtls_ssl_check_pending>([](const mbedtls_ssl_context *) { return 1; });
    mbedtls_ssl_context ssl{};
    CHECK(mbedtls_ssl_check_pending(&ssl) == 1 && amity_seam::count("mbedtls_ssl_check_pending") == 1);
}

void services() {
    std::vector<uint32_t> reboots;
    amity_seam::install<static_cast<void (*)()>(&system_services::reboot_later)>([&] { reboots.push_back(0); });
    amity_seam::install<static_cast<void (*)(uint32_t)>(&system_services::reboot_later)>(
        [&](uint32_t delay) { reboots.push_back(delay); });
    system_services::reboot_later();
    system_services::reboot_later(1500);
    CHECK((reboots == std::vector<uint32_t>{0, 1500}) && amity_seam::count("system_services::reboot_later") == 2);
    amity_fake::HttpRequest request(HTTP_GET, "/api/logs");
    amity_seam::install<&system_services::download_logs>([](httpd_req_t *req, model::OctopusId) {
        return httpd_resp_sendstr(req, "log");
    });
    CHECK(system_services::download_logs(&request, id<model::OctopusId>(1)) == ESP_OK && request.response->body == "log");
    amity_seam::install<&updates::peer_chunk>([](std::string_view, uint64_t offset, std::string_view bytes, std::string &error) {
        error = offset ? "" : "first";
        return bytes.size() == 2048 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    });
    std::string error;
    CHECK(updates::peer_chunk("release", 0, std::string(2048, 'x'), error) == ESP_OK && error == "first");
    SystemControl control;
    amity_seam::install<&system_services::prepare_removed_device_reset>(
        [&](SystemControl &target, model::OctopusId) { return &target == &control ? ESP_OK : ESP_FAIL; });
    CHECK(system_services::prepare_removed_device_reset(control, id<model::OctopusId>(2)) == ESP_OK);
    amity_seam::install<&system_services::adapter_settings::apply>(
        [](model::MessageId, const consortium::DeviceSettingsWrite &write, uint64_t, std::string &snapshot) {
            snapshot = write.hostname.value_or("");
            return consortium::Error::none;
        });
    consortium::DeviceSettingsWrite write;
    write.hostname = "den";
    std::string snapshot;
    CHECK(system_services::adapter_settings::apply(id<model::MessageId>(3), write, 0, snapshot) == consortium::Error::none);
    CHECK(snapshot == "den");
}

void settings() {
    amity_seam::install<&device_settings::hostname>([] { return "living-room"; });
    std::string saved;
    amity_seam::install<&device_settings::save_hostname>([&](uint64_t revision, std::string hostname) {
        saved = hostname;
        return revision == 4 ? ESP_OK : ESP_ERR_INVALID_STATE;
    });
    amity_seam::install<&device_settings::schedule_factory_reset>([] { return ESP_OK; });
    amity_seam::install<&device_settings::start_reset_button>([](int gpio) { return gpio == 4 ? ESP_OK : ESP_FAIL; });
    CHECK(std::strcmp(device_settings::hostname(), "living-room") == 0);
    CHECK(device_settings::save_hostname(4, "den") == ESP_OK && saved == "den");
    CHECK(device_settings::schedule_factory_reset() == ESP_OK && device_settings::start_reset_button(4) == ESP_OK);
    amity_seam::install<&amity::network_diagnostics::snapshot>([] {
        amity::network_diagnostics::Snapshot value;
        value.now_ms = 77;
        return value;
    });
    amity_seam::install<&amity::network_diagnostics::wifi_signal>([] {
        return amity::network_diagnostics::WifiSignal{true, -61, 6};
    });
    CHECK(amity::network_diagnostics::snapshot().now_ms == 77 && amity::network_diagnostics::wifi_signal().rssi == -61);
    amity_seam::install<&config_store::with_internal_flash_stack>([](std::function<void()> function) {
        function();
        return true;
    });
    bool ran = false;
    CHECK(config_store::with_internal_flash_stack([&] { ran = true; }) && ran);
    amity_seam::install<&config_store::load_system_identity>([](model::OctopusId &octopus, model::ConsortiumId &group) {
        octopus = id<model::OctopusId>(5);
        group = id<model::ConsortiumId>(6);
        return ESP_OK;
    });
    model::OctopusId octopus;
    model::ConsortiumId group;
    CHECK(config_store::load_system_identity(octopus, group) == ESP_OK && octopus == id<model::OctopusId>(5));
}

void web_server() {
    amity_fake::HttpRequest request(HTTP_POST, "/api/login", "secret");
    amity_seam::install<&amity_seam::constructor<bool, web::BodyReader, httpd_req_t *, uint64_t>>(
        [](web::BodyReader &, httpd_req_t *, uint64_t timeout_us) { return timeout_us == 15000000; });
    amity_seam::install<&web::BodyReader::receive>([](web::BodyReader &reader, char *buffer, size_t) {
        CHECK(reader.valid());
        buffer[0] = 's';
        return 1;
    });
    {
        web::BodyReader reader(&request);
        char byte = 0;
        CHECK(reader.valid() && reader.receive(&byte, 1) == 1 && byte == 's');
    }
    {
        web::BodyReader reader(&request, 5);
        CHECK(!reader.valid());
    }
    CHECK((amity_seam::calls() ==
           std::vector<std::string>{"web::BodyReader::BodyReader", "web::BodyReader::receive", "web::BodyReader::~BodyReader",
                                    "web::BodyReader::BodyReader", "web::BodyReader::~BodyReader"}));
    amity_seam::install<&web::login>([](const std::string &password, std::string &token) {
        token = "token";
        return password == "secret";
    });
    std::string token;
    CHECK(web::login("secret", token) && token == "token" && !web::login("wrong", token));
    amity_seam::install<&web::route>([](httpd_handle_t, const char *path, httpd_method_t method, web::Handler, void *) {
        return std::strcmp(path, "/api/config") == 0 && method == HTTP_GET ? ESP_OK : ESP_FAIL;
    });
    CHECK(web::route(&request, "/api/config", HTTP_GET, nullptr) == ESP_OK);
}

void system_control() {
    amity_seam::install<&HubService::activity_target>([](const HubService &) { return 3; });
    amity_seam::install<static_cast<esp_err_t (SystemControl::*)(int)>(&SystemControl::request_activity)>(
        [](SystemControl &, int index) { return index == 2 ? ESP_OK : ESP_ERR_INVALID_ARG; });
    amity_seam::install<&SystemControl::accepts_local_input>([](const SystemControl &) { return true; });
    {
        SystemControl control;
        CHECK(control.activity_target() == 3 && control.request_activity(2) == ESP_OK && control.accepts_local_input());
    }
    CHECK((amity_seam::calls() ==
           std::vector<std::string>{"amity::HubService::HubService", "amity::HubService::activity_target",
                                    "amity::SystemControl::request_activity", "amity::SystemControl::accepts_local_input",
                                    "amity::HubService::~HubService"}));
}

void radios() {
    amity_seam::install<&amity_matter_update_remote>([](bool paired, bool connected, int battery, int charging) {
        return paired && !connected && battery == -1 && charging == -1 ? ESP_OK : ESP_FAIL;
    });
    CHECK(amity_matter_update_remote(true, false, -1, -1) == ESP_OK);
    amity_seam::install<&amity_homekit::status>([] {
        amity_homekit::Status status;
        status.paired = true;
        return status;
    });
    CHECK(amity_homekit::status().paired);
    amity_seam::install<&ble_remote_get_status>([](ble_remote_status_t *status) { status->battery_percent = 80; });
    ble_remote_status_t status{};
    ble_remote_get_status(&status);
    CHECK(status.battery_percent == 80);
    amity_seam::install<&amity::radio::reserve_consortium>([](bool inviter, uint64_t &generation) {
        generation = inviter ? 11 : 12;
        return ESP_OK;
    });
    uint64_t generation = 0;
    CHECK(amity::radio::reserve_consortium(true, generation) == ESP_OK && generation == 11);
    CHECK(amity_seam::calls().size() == 4);
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"complete", complete},
                            {"strict", strict},
                            {"runtime", runtime},
                            {"credentials", credentials},
                            {"services", services},
                            {"settings", settings},
                            {"web", web_server},
                            {"system-control", system_control},
                            {"radios", radios}});
}
