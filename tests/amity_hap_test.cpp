/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The Amity side of the HomeKit SDK fork, built from the whole of
// third_party/esp-homekit-sdk/components/esp_hap_core/src/amity_hap.c as C11
// against the core fakes, with strict seams for the fork functions it calls.
#include "amity_hap.h"
extern "C" {
#include "esp_hap_database.h"
#include "esp_hap_ip_services.h"
#include "esp_hap_keystore.h"
#include "esp_hap_pair_verify.h"
#include "hap_platform_httpd.h"
}
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

using amity_seam::calls;
using amity_seam::count;
using amity_seam::install;
using Calls = std::vector<std::string>;

// The HAP lock amity_hap.c creates: its only recursive mutex.
QueueHandle_t hap_lock() {
    QueueHandle_t found = nullptr;
    for (const auto &queue : amity_fake::queues()) {
        if (queue.kind != amity_fake::Queue::Kind::recursive_mutex) continue;
        CHECK(!found);
        found = queue.handle;
    }
    CHECK(found != nullptr);
    return found;
}
bool held() { return xSemaphoreGetMutexHolder(hap_lock()) == xTaskGetCurrentTaskHandle(); }
bool released() { return xSemaphoreGetMutexHolder(hap_lock()) == nullptr; }

// What the fork functions report, and the hostname each discovery call saw.
struct Sdk {
    std::deque<int> announce;
    int withdraw = HAP_SUCCESS, deannounce = HAP_SUCCESS, ip_stop = HAP_SUCCESS, keystore = HAP_SUCCESS;
    int httpd_stop = ESP_OK;
    std::vector<std::string> announced, withdrawn;
};
Sdk &sdk() {
    static Sdk instance;
    return instance;
}

// Every fork function runs under the HAP lock, except the HTTP server stop,
// which must run without it.
void install_sdk() {
    install<&hap_mdns_announce>([](bool first) {
        CHECK(held() && !first);
        sdk().announced.push_back(hap_amity_hostname());
        if (sdk().announce.empty()) return HAP_SUCCESS;
        const int result = sdk().announce.front();
        sdk().announce.pop_front();
        return result;
    });
    install<&hap_mdns_deannounce_for_rename>([] {
        CHECK(held());
        sdk().withdrawn.push_back(hap_amity_hostname());
        return sdk().withdraw;
    });
    install<&hap_mdns_deannounce>([] {
        CHECK(held());
        return sdk().deannounce;
    });
    install<&hap_close_all_sessions>([] { CHECK(held()); });
    install<&hap_ip_services_stop>([] {
        CHECK(held());
        return sdk().ip_stop;
    });
    install<&hap_keystore_delete_namespace>([](const char *name) {
        CHECK(held() && std::strcmp(name, "hap_ctrl") == 0);
        return sdk().keystore;
    });
    install<&hap_platform_httpd_stop>([](httpd_handle_t *handle) {
        CHECK(released() && handle == &hap_priv.server && *handle);
        return sdk().httpd_stop;
    });
}

bool controllers_cleared() {
    for (const auto &controller : hap_priv.controllers)
        if (controller.valid || controller.index || controller.info.id[0]) return false;
    return true;
}
void pair_controllers() {
    hap_priv.controllers[0].valid = true;
    std::strcpy(hap_priv.controllers[0].info.id, "controller-1");
    hap_priv.controllers[5].valid = true;
    hap_priv.controllers[5].index = 5;
}

// Initialization keeps a hostname of up to 32 bytes and creates the
// recursive HAP lock once.
void init() {
    CHECK(hap_amity_init(nullptr) == HAP_FAIL && hap_amity_init(std::string(33, 'h').c_str()) == HAP_FAIL);
    CHECK(amity_fake::queues().empty() && std::string(hap_amity_hostname()).empty());
    const std::string longest(32, 'h');
    CHECK(hap_amity_init(longest.c_str()) == HAP_SUCCESS && hap_amity_hostname() == longest);
    CHECK(hap_amity_init("living-room") == HAP_SUCCESS && std::string(hap_amity_hostname()) == "living-room");
    CHECK(amity_fake::queues().size() == 1 && released() && hap_amity_enabled());
}

// Without its lock the service cannot start: locking does nothing and stop()
// only disables the service.
void init_without_lock() {
    amity_fake::fail_queue_creations(1);
    CHECK(hap_amity_init("amity") == HAP_FAIL && std::string(hap_amity_hostname()).empty());
    hap_amity_lock();
    hap_amity_unlock();
    CHECK(amity_fake::queues().empty());
    CHECK(hap_amity_stop() == HAP_SUCCESS && calls().empty());
    CHECK(!hap_amity_enabled() && hap_amity_init("amity") == HAP_FAIL);
}

// The HAP lock is recursive and belongs to the task that holds it.
void lock() {
    CHECK(hap_amity_init("amity") == HAP_SUCCESS && released());
    hap_amity_lock();
    CHECK(held());
    hap_amity_lock();
    hap_amity_unlock();
    CHECK(held());
    hap_amity_unlock();
    CHECK(released());
}

// A pairing window is open while the service is enabled and its deadline is
// ahead; every new deadline is a new window.
void pairing_window() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    CHECK(!hap_amity_pairing_allowed() && hap_amity_pairing_window() == 0);
    amity_fake::advance_us(1000);
    hap_amity_set_pairing_deadline(5000);
    CHECK(hap_amity_pairing_allowed() && hap_amity_pairing_window() == 1);
    hap_amity_set_pairing_deadline(5000);
    CHECK(hap_amity_pairing_window() == 1);
    amity_fake::advance_us(3999);
    CHECK(hap_amity_pairing_allowed());
    amity_fake::advance_us(1);
    CHECK(!hap_amity_pairing_allowed());
    hap_amity_set_pairing_deadline(9000);
    CHECK(hap_amity_pairing_allowed() && hap_amity_pairing_window() == 2);
    CHECK(hap_amity_stop() == HAP_SUCCESS && !hap_amity_pairing_allowed() && hap_amity_pairing_window() == 3);
    // A disabled service opens no window.
    hap_amity_set_pairing_deadline(20000);
    CHECK(!hap_amity_pairing_allowed() && hap_amity_pairing_window() == 4);
}

// Refreshing discovery announces the current hostname again.
void refresh_mdns() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    sdk().announce = {HAP_SUCCESS, HAP_FAIL};
    hap_amity_lock();
    CHECK(hap_amity_refresh_mdns() == HAP_SUCCESS && hap_amity_refresh_mdns() == HAP_FAIL);
    hap_amity_unlock();
    CHECK((sdk().announced == std::vector<std::string>{"amity", "amity"}) && released());
}

// A rename withdraws the old name and announces the new one under the HAP
// lock, leaving sessions and pairings alone. An unchanged name changes
// nothing; an empty or too long name is refused.
void rename_hostname() {
    install_sdk();
    CHECK(hap_amity_init("old") == HAP_SUCCESS);
    CHECK(hap_amity_rename_hostname("new") == HAP_SUCCESS);
    CHECK((calls() == Calls{"hap_mdns_deannounce_for_rename", "hap_mdns_announce"}));
    CHECK((sdk().withdrawn == std::vector<std::string>{"old"} && sdk().announced == std::vector<std::string>{"new"}));
    CHECK(std::string(hap_amity_hostname()) == "new" && released());
    amity_seam::clear_calls();
    CHECK(hap_amity_rename_hostname("new") == HAP_SUCCESS && calls().empty() && released());
    CHECK(hap_amity_rename_hostname("") == HAP_FAIL && hap_amity_rename_hostname(nullptr) == HAP_FAIL);
    CHECK(hap_amity_rename_hostname(std::string(33, 'n').c_str()) == HAP_FAIL && calls().empty());
    const std::string longest(32, 'n');
    CHECK(hap_amity_rename_hostname(longest.c_str()) == HAP_SUCCESS && hap_amity_hostname() == longest);
    // Renaming back restores the first name.
    CHECK(hap_amity_rename_hostname("old") == HAP_SUCCESS && std::string(hap_amity_hostname()) == "old");
    CHECK(sdk().announced.back() == "old" && released());
}

// A failed withdrawal keeps the old name and announces nothing.
void rename_withdrawal_failure() {
    install_sdk();
    CHECK(hap_amity_init("old") == HAP_SUCCESS);
    sdk().withdraw = HAP_FAIL;
    CHECK(hap_amity_rename_hostname("new") == HAP_FAIL);
    CHECK(std::string(hap_amity_hostname()) == "old" && sdk().announced.empty() && released());
}

// A failed announcement restores the old name and announces it again; a
// failed restore is logged.
void rename_announcement_failure() {
    install_sdk();
    CHECK(hap_amity_init("old") == HAP_SUCCESS);
    sdk().announce = {HAP_FAIL};
    CHECK(hap_amity_rename_hostname("new") == HAP_FAIL);
    CHECK((sdk().announced == std::vector<std::string>{"new", "old"}) && std::string(hap_amity_hostname()) == "old");
    CHECK(!amity_fake::logged("Could not restore") && released());
    sdk().announce = {HAP_FAIL, HAP_FAIL};
    CHECK(hap_amity_rename_hostname("new") == HAP_FAIL && std::string(hap_amity_hostname()) == "old");
    const auto lines = amity_fake::log_lines();
    CHECK(lines.size() == 1 && lines[0].level == ESP_LOG_ERROR && lines[0].tag == "amity_hap");
    CHECK(lines[0].message == "Could not restore HomeKit discovery after hostname change" && released());
}

// Removing pairings, under the caller's HAP lock, closes every session,
// erases the controllers from the keystore and the SDK database, closes the
// pairing window and withdraws discovery.
void remove_pairings() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    pair_controllers();
    hap_amity_set_pairing_deadline(5000);
    hap_amity_lock();
    CHECK(hap_amity_remove_pairings() == HAP_SUCCESS);
    hap_amity_unlock();
    CHECK((calls() == Calls{"hap_close_all_sessions", "hap_keystore_delete_namespace", "hap_mdns_deannounce"}));
    CHECK(controllers_cleared() && !hap_amity_pairing_allowed() && hap_amity_pairing_window() == 1);
    sdk().deannounce = HAP_FAIL;
    hap_amity_lock();
    CHECK(hap_amity_remove_pairings() == HAP_FAIL);
    hap_amity_unlock();
}

// A keystore that cannot erase the controllers keeps them, the window and
// discovery.
void remove_pairings_failure() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    pair_controllers();
    hap_amity_set_pairing_deadline(5000);
    sdk().keystore = HAP_FAIL;
    hap_amity_lock();
    CHECK(hap_amity_remove_pairings() == HAP_FAIL);
    hap_amity_unlock();
    CHECK((calls() == Calls{"hap_close_all_sessions", "hap_keystore_delete_namespace"}));
    CHECK(!controllers_cleared() && hap_amity_pairing_allowed());
}

// stop() disables the service for good. Under the HAP lock it closes the
// pairing window, every session, discovery and the IP services; it stops the
// HTTP server only after releasing the lock, since a handler can be waiting
// for the lock while the server shuts down. Pairings are kept.
void stop() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    int server = 0;
    hap_priv.server = &server;
    pair_controllers();
    hap_amity_set_pairing_deadline(5000);
    CHECK(hap_amity_stop() == HAP_SUCCESS);
    CHECK((calls() ==
           Calls{"hap_close_all_sessions", "hap_mdns_deannounce", "hap_ip_services_stop", "hap_platform_httpd_stop"}));
    CHECK(hap_priv.server == nullptr && !controllers_cleared() && released());
    CHECK(!hap_amity_enabled() && !hap_amity_pairing_allowed() && hap_amity_pairing_window() == 2);
    CHECK(hap_amity_init("amity") == HAP_FAIL);
    amity_seam::clear_calls();
    CHECK(hap_amity_stop() == HAP_SUCCESS);
    CHECK((calls() == Calls{"hap_close_all_sessions", "hap_mdns_deannounce", "hap_ip_services_stop"}));
}

// A failed HTTP server stop keeps the server for a later stop to finish.
void stop_server_failure() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    int server = 0;
    hap_priv.server = &server;
    sdk().httpd_stop = ESP_FAIL;
    CHECK(hap_amity_stop() == HAP_FAIL && hap_priv.server == &server && !hap_amity_enabled() && released());
    sdk().httpd_stop = ESP_OK;
    CHECK(hap_amity_stop() == HAP_SUCCESS && hap_priv.server == nullptr);
    CHECK(count("hap_platform_httpd_stop") == 2 && hap_amity_stop() == HAP_SUCCESS);
    CHECK(count("hap_platform_httpd_stop") == 2);
}

// A failed withdrawal or IP services stop is reported, and the stop still
// finishes.
void stop_failures() {
    install_sdk();
    CHECK(hap_amity_init("amity") == HAP_SUCCESS);
    int server = 0;
    hap_priv.server = &server;
    sdk().deannounce = HAP_FAIL;
    CHECK(hap_amity_stop() == HAP_FAIL && hap_priv.server == nullptr && count("hap_ip_services_stop") == 1);
    sdk().deannounce = HAP_SUCCESS;
    sdk().ip_stop = HAP_FAIL;
    hap_priv.server = &server;
    CHECK(hap_amity_stop() == HAP_FAIL && hap_priv.server == nullptr && released());
}

}  // namespace

int main(int argc, char **argv) {
    return amity_test::run(argc, argv,
                           {{"init", init},
                            {"init-without-lock", init_without_lock},
                            {"lock", lock},
                            {"pairing-window", pairing_window},
                            {"refresh-mdns", refresh_mdns},
                            {"rename-hostname", rename_hostname},
                            {"rename-withdrawal-failure", rename_withdrawal_failure},
                            {"rename-announcement-failure", rename_announcement_failure},
                            {"remove-pairings", remove_pairings},
                            {"remove-pairings-failure", remove_pairings_failure},
                            {"stop", stop},
                            {"stop-server-failure", stop_server_failure},
                            {"stop-failures", stop_failures}});
}
