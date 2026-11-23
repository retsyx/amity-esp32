/*
 * Copyright 2026.
 * This file is part of Amity-esp32.
 * Amity-esp32 is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 */

// The radio owner of components/radio_control/radio_control.cpp, built whole
// with the real RadioArbiter, against the CHIP and core fakes. The test stands
// in for NimBLE, the CHIP BLE manager and the BLE remote. macOS has no linker
// wrapping, so the test defines the __real_ functions the wrappers call and
// calls the __wrap_ functions where the SDK would.
#include "../components/radio_control/radio_control.cpp"
#include "amity_matter_fakes.hpp"
#include "core_fakes.hpp"
#include "scenario_main.hpp"
#include "seams.hpp"
#include <array>
#include <functional>
#include <map>
#include <vector>

namespace amity::radio {
namespace test {

using namespace amity::consortium;

// IP-only Matter state. No accessor reaches radio_control.cpp, so none of it
// can hold BLE.
bool fail_safe = false, fail_safe_busy = false, pase_session = false, window_open = false;

// NimBLE's advertiser, the advertising state the CHIP BLE manager wants, and
// whether a call is inside the SDK's advertising switch.
bool advertising = false, desired_advertising = false, sdk_advertising_call = false;
CHIP_ERROR advertising_error = CHIP_NO_ERROR;
// When the advertiser stops after a disable, and when the remote finishes its
// cleanup, in milliseconds; 0 means at once.
uint64_t advert_stop_at = 0, cleanup_ready_at = 0;
bool background_paused = false;
unsigned disables = 0, pauses = 0, resumes = 0, delays = 0, dispatches = 0;
// The roles of the GAP links the controller reported.
std::map<uint16_t, uint8_t> gap_roles;
std::function<void(uint8_t *, void *)> on_dispatch;
uint8_t *expected_event = nullptr;
void *const expected_context = reinterpret_cast<void *>(0x1234);
std::function<void()> on_disable, on_disabled;
// Runs at each delay with the time the delay ends.
std::function<void(uint64_t)> on_delay;

constexpr uint8_t gap_role_master = 0, gap_role_slave = 1;

uint64_t clock_ms() { return uint64_t(amity_fake::now_us() / 1000); }
bool radio_held() { return mutex && xSemaphoreGetMutexHolder(mutex) == xTaskGetCurrentTaskHandle(); }
bool in_critical_section() { return peripheral_mux.count != 0; }

// What the scenario captured from start().
ble_remote_radio_admit_t admit_hook = nullptr;
ble_remote_radio_done_t done_hook = nullptr;
void *remote_context = reinterpret_cast<void *>(1);
RuntimeRadioHooks radio_hooks;

// The SDK path from ConnectivityMgr().SetBLEAdvertisingEnabled into the
// wrapped BLE manager method; the hooks stand in for other tasks. No call into
// another component may hold the radio mutex, and the CHIP lock is taken only
// without it and outside the critical section.
void script_sdk() {
    using chip::DeviceLayer::ConnectivityManager;
    amity_seam::install<&ConnectivityManager::SetBLEAdvertisingEnabled>([](ConnectivityManager &, bool enabled) {
        const bool previous = sdk_advertising_call;
        sdk_advertising_call = true;
        const auto result = __wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(nullptr, enabled);
        sdk_advertising_call = previous;
        const auto hook = on_disabled;
        if (!enabled && hook) hook();
        return result;
    });
    amity_seam::install<&ble_remote_pause_background>([](bool paused) {
        CHECK((!amity_fake::chip_stack_locked() || sdk_advertising_call) && !radio_held());
        background_paused = paused;
        if (!paused) {
            ++resumes;
            return ESP_OK;
        }
        ++pauses;
        return clock_ms() < cleanup_ready_at ? ESP_ERR_TIMEOUT : ESP_OK;
    });
    amity_seam::install<&ble_remote_cancel_setup>([] {
        CHECK(!radio_held());
        return ESP_OK;
    });
    amity_seam::install<&runtime_cancel_radio_workflows>([] {
        CHECK(!radio_held());
        return true;
    });
    amity_seam::install<&ble_remote_set_radio_hooks>([](ble_remote_radio_admit_t admit, ble_remote_radio_done_t done, void *context) {
        admit_hook = admit;
        done_hook = done;
        remote_context = context;
    });
    amity_seam::install<&runtime_radio_hooks>([](RuntimeRadioHooks hooks) { radio_hooks = std::move(hooks); });
    amity_fake::on_delay([](TickType_t ticks) {
        CHECK(!amity_fake::chip_stack_locked() && !radio_held());
        const uint64_t after = clock_ms() + pdTICKS_TO_MS(ticks);
        ++delays;
        if (advert_stop_at && after >= advert_stop_at) advertising = false;
        const auto hook = on_delay;
        if (hook) hook(after);
    });
    amity_fake::on_chip_stack_lock([] { CHECK(!radio_held() && !in_critical_section()); });
}

// Starts the radio owner, as the firmware does before anything else uses it.
void begin() {
    script_sdk();
    CHECK(start() == ESP_OK);
}

void hci_event(uint8_t code, std::vector<uint8_t> payload) {
    std::vector<uint8_t> packet{code, static_cast<uint8_t>(payload.size())};
    packet.insert(packet.end(), payload.begin(), payload.end());
    const auto original = packet;
    const unsigned previous = dispatches;
    expected_event = packet.data();
    CHECK(__wrap_ble_hs_hci_rx_evt(packet.data(), expected_context) == 73);
    CHECK(packet == original && dispatches == previous + 1);
    CHECK(!in_critical_section());
}

void gap_connect(uint16_t handle, uint8_t role = gap_role_slave, uint8_t status = 0,
                 uint8_t subevent = BLE_HCI_LE_SUBEV_CONN_COMPLETE) {
    if (!status) gap_roles[handle] = role;
    std::vector<uint8_t> payload(subevent == BLE_HCI_LE_SUBEV_CONN_COMPLETE ? 19
                                 : subevent == BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE ? 31 : 34);
    payload[0] = subevent;
    payload[1] = status;
    payload[2] = handle & 0xff;
    payload[3] = handle >> 8;
    payload[4] = role;
    hci_event(BLE_HCI_EVCODE_LE_META, payload);
}

void gap_disconnect(uint16_t handle, uint8_t status = 0) {
    if (!status) gap_roles.erase(handle);
    hci_event(BLE_HCI_EVCODE_DISCONN_CMP, {status, static_cast<uint8_t>(handle), static_cast<uint8_t>(handle >> 8), 0x13});
}

void controller_reset(uint8_t status = 0) {
    hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE, {1, 3, 0x0c, status});
    if (!status) gap_roles.clear();
}

void disable_matter() {
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    CHECK(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false) == CHIP_NO_ERROR);
}

// Runs the worker until its first delay.
void worker_once() {
    on_delay = [](uint64_t) { throw amity_fake::LeaveTask{}; };
    try {
        worker(nullptr);
    } catch (const amity_fake::LeaveTask &) {
    }
    on_delay = {};
}

// Opens idle Matter BLE as the SDK's commissioning window would.
void idle_matter() {
    Lock lock;
    CHECK(arbiter.external_matter_will_open(now(), 300000, false) == RadioResult::granted);
    window_open = advertising = desired_advertising = true;
}

// Another task replaces the radio owner.
void replace_owner() {
    Lock lock;
    auto current = arbiter.reservation();
    arbiter.release(current.generation, now());
    current = arbiter.reservation();
    arbiter.cleanup_complete(current.generation, now());
    CHECK(arbiter.request(RadioMode::remote_pairing, now(), 30000, false) == RadioResult::granted);
}

RadioReservation reservation() {
    Lock lock;
    return arbiter.reservation();
}

// Idle Matter BLE hands off to a candidate or an inviter: advertising is
// disabled, independently of the IP window, and the handoff waits for the
// asynchronous stop. A queued old disable leaves the new owner alone.
template <bool inviter> void idle_handoff() {
    begin();
    idle_matter();
    const auto previous = reservation();
    advert_stop_at = 300;
    uint64_t admitted = 99;
    CHECK(reserve_consortium(inviter, admitted) == ESP_OK);
    const auto current = reservation();
    CHECK(disables == 1 && window_open && !advertising && !desired_advertising);
    CHECK(current.owner == (inviter ? RadioMode::consortium_inviter : RadioMode::consortium_candidate));
    CHECK(current.generation == admitted && admitted > previous.generation);
    CHECK(clock_ms() == 300 && delays == 3);
    disable_matter();
    CHECK(reservation().generation == admitted && reservation().owner == current.owner);
}

// A physical Matter peripheral denies takeover, with one to three links.
void active_denied() {
    begin();
    idle_matter();
    for (uint16_t handle = 1; handle <= 3; ++handle) {
        gap_connect(handle);
        const auto previous = reservation();
        uint64_t admitted = 99;
        CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE);
        CHECK(admitted == 0 && disables == 0 && window_open && advertising);
        CHECK(reservation().generation == previous.generation);
        CHECK(!remote_request(20, nullptr));
    }
}

// Another busy owner is neither preempted nor changed.
void other_busy() {
    begin();
    {
        Lock lock;
        CHECK(arbiter.request(RadioMode::remote_pairing, now(), 30000, false) == RadioResult::granted);
    }
    const auto previous = reservation();
    uint64_t admitted = 99;
    CHECK(reserve_consortium(true, admitted) == ESP_ERR_INVALID_STATE && admitted == 0 && disables == 0);
    CHECK(reservation().generation == previous.generation && reservation().owner == RadioMode::remote_pairing);
}

// A disable whose generation was replaced meanwhile admits nothing and keeps
// the replacement.
void stale_disable() {
    begin();
    idle_matter();
    on_disable = replace_owner;
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE && admitted == 0);
    CHECK(reservation().owner == RadioMode::remote_pairing);
}

// A cleanup whose generation was replaced meanwhile admits nothing and keeps
// the replacement.
void stale_cleanup() {
    begin();
    idle_matter();
    advert_stop_at = 300;
    on_delay = [](uint64_t) {
        on_delay = {};
        replace_owner();
    };
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE && admitted == 0);
    CHECK(reservation().owner == RadioMode::remote_pairing && delays == 1);
}

// A stuck advertiser fails the handoff after the shared 50 checks, 5 seconds,
// and the failed handoff gives its generation back.
void bounded_timeout_steps() {
    idle_matter();
    const auto matter = reservation();
    advert_stop_at = 6000;
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_TIMEOUT && admitted == 0);
    CHECK(delays == 50 && clock_ms() == 5000 && reservation().owner != RadioMode::consortium_candidate);
    Lock lock;
    CHECK(arbiter.cleanup_complete(matter.generation + 1, now()) == RadioResult::stale);
}

void bounded_timeout() {
    begin();
    bounded_timeout_steps();
}

// The idle Matter handoff and the requested workflow's cleanup share the 50
// checks: the handoff's four leave the workflow 46.
void shared_budget() {
    begin();
    idle_matter();
    advert_stop_at = 300;
    // After the handoff, the remote's background work takes longer to pause
    // than the budget allows.
    amity_seam::install<&ble_remote_pause_background>([](bool paused) {
        CHECK(paused && !radio_held());
        background_paused = paused;
        return ++pauses >= 2 ? ESP_ERR_TIMEOUT : ESP_OK;
    });
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_TIMEOUT && admitted == 0);
    CHECK(pauses == 47 && delays == 49 && clock_ms() == 4900);
    CHECK(reservation().owner == RadioMode::transition);
}

// A peripheral that connects while advertising is disabled denies the
// takeover at once.
void peripheral_during_disable() {
    begin();
    idle_matter();
    on_disable = [] { gap_connect(7); };
    const auto matter = reservation();
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE && admitted == 0 && delays == 0);
    CHECK(reservation().owner == RadioMode::matter_induction && reservation().generation == matter.generation);
}

// A disable the SDK refuses stops the handoff, even when an earlier disable
// of the same generation was committed and the worker has not yet run.
void disable_refused() {
    begin();
    idle_matter();
    disable_matter();
    const auto matter = reservation();
    advertising_error = CHIP_ERROR_INTERNAL;
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE && admitted == 0 && delays == 0);
    CHECK(reservation().owner == RadioMode::matter_induction && reservation().generation == matter.generation);
}

// Only the cleanup generation right after the disable belongs to it: once
// another owner came and went meanwhile, the handoff is refused.
void handoff_after_other_owner() {
    begin();
    idle_matter();
    const auto matter = reservation();
    on_disabled = [matter] {
        on_disabled = {};
        release_idle_matter(matter.generation);
        Lock lock;
        CHECK(arbiter.cleanup_complete(arbiter.reservation().generation, now()) == RadioResult::granted);
        CHECK(arbiter.request(RadioMode::remote_pairing, now(), 30000, false) == RadioResult::granted);
        CHECK(arbiter.release(arbiter.reservation().generation, now()) == RadioResult::cancelling);
    };
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_ERR_INVALID_STATE && admitted == 0);
    CHECK(reservation().owner == RadioMode::transition && reservation().generation == matter.generation + 3);
}

// A late confirmed cleanup releases the radio, resumes remote reconnect,
// rejects duplicate confirmations and leaves fresh setups alone; until then
// the radio stays in transition.
template <bool finishing_release> void late_cleanup() {
    begin();
    {
        Lock lock;
        if (finishing_release) {
            CHECK(arbiter.request(RadioMode::remote_pairing, now(), 30000, false) == RadioResult::granted);
            CHECK(arbiter.release(arbiter.reservation().generation, now()) == RadioResult::cancelling);
        } else {
            arbiter.remote_reconnect_running(true);
            CHECK(arbiter.request(RadioMode::consortium_candidate, now(), 30000, false) == RadioResult::cancelling);
        }
    }
    const auto cancelled = reservation();
    cleanup_ready_at = 6000;
    on_delay = [](uint64_t after) {
        if (after <= cleanup_ready_at) CHECK(reservation().owner == RadioMode::transition);
        if (after > cleanup_ready_at) throw amity_fake::LeaveTask{};
    };
    try {
        worker(nullptr);
    } catch (const amity_fake::LeaveTask &) {
    }
    on_delay = {};
    Lock lock;
    CHECK(arbiter.reservation().owner == RadioMode::operational && arbiter.reservation().deadline_ms == 0);
    CHECK(arbiter.remote_may_reconnect() && !background_paused && resumes == 1);
    CHECK(arbiter.cleanup_complete(cancelled.generation, now()) == RadioResult::stale);
    for (const auto requested : {RadioMode::remote_pairing, RadioMode::consortium_candidate, RadioMode::matter_induction}) {
        CHECK(arbiter.request(requested, now(), 30000, false) == RadioResult::granted);
        const auto fresh = arbiter.reservation();
        CHECK(fresh.generation > cancelled.generation);
        CHECK(arbiter.cleanup_complete(cancelled.generation, now()) == RadioResult::stale);
        CHECK(arbiter.reservation().generation == fresh.generation && arbiter.reservation().owner == requested);
        CHECK(arbiter.release(fresh.generation, now()) == RadioResult::cancelling);
        CHECK(arbiter.cleanup_complete(arbiter.reservation().generation, now()) == RadioResult::granted);
    }
}

// A cleanup confirmed after the handoff expired reports the timeout and still
// returns the radio.
void late_cleanup_result() {
    begin();
    Lock lock;
    arbiter.remote_reconnect_running(true);
    CHECK(arbiter.request(RadioMode::consortium_candidate, now(), 30000, false) == RadioResult::cancelling);
    const auto generation = arbiter.reservation().generation;
    amity_fake::advance_ms(RadioArbiter::cancel_ms);
    CHECK(arbiter.cleanup_complete(generation, now()) == RadioResult::timed_out);
    CHECK(arbiter.reservation().owner == RadioMode::operational);
}

// After a timed-out handoff, the worker waits for the advertiser to stop
// before releasing the radio, and a new admission then succeeds.
void late_advertising_cleanup() {
    begin();
    bounded_timeout_steps();
    const auto cancelled = reservation();
    CHECK(advertising && cancelled.owner == RadioMode::transition);
    on_delay = [](uint64_t after) {
        if (after <= advert_stop_at) CHECK(reservation().owner == RadioMode::transition && background_paused);
        if (after > advert_stop_at) throw amity_fake::LeaveTask{};
    };
    try {
        worker(nullptr);
    } catch (const amity_fake::LeaveTask &) {
    }
    on_delay = {};
    CHECK(!advertising && reservation().owner == RadioMode::operational && !background_paused);
    uint64_t fresh = 0;
    CHECK(reserve_consortium(false, fresh) == ESP_OK && fresh > cancelled.generation);
}

// The worker consuming the committed disable, or also finishing the cleanup,
// during the handoff still lets the handoff succeed.
template <bool completed> void worker_during_disable() {
    begin();
    idle_matter();
    const auto previous = reservation();
    on_disabled = [previous] {
        on_disabled = {};
        release_idle_matter(previous.generation);
        Lock lock;
        CHECK(arbiter.reservation().owner == RadioMode::transition);
        if (completed) CHECK(arbiter.cleanup_complete(arbiter.reservation().generation, now()) == RadioResult::granted);
    };
    uint64_t admitted = 0;
    CHECK(reserve_consortium(false, admitted) == ESP_OK && admitted > previous.generation);
    CHECK(reservation().owner == RadioMode::consortium_candidate && window_open);
}

// An admission with no Matter BLE involved disables nothing and waits for nothing.
void ordinary() {
    begin();
    uint64_t admitted = 99;
    CHECK(reserve_consortium(false, admitted) == ESP_OK && admitted && disables == 0 && delays == 0);
    CHECK(reservation().owner == RadioMode::consortium_candidate);
}

// Disabling BLE before the peripheral disconnects keeps Matter until the
// disconnect, which then releases the radio.
void disable_before_disconnect() {
    begin();
    idle_matter();
    gap_connect(1);
    window_open = advertising = false;
    disable_matter();
    CHECK(reservation().owner == RadioMode::matter_induction);
    amity_fake::advance_ms(60);
    gap_disconnect(1);
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
    Lock lock;
    CHECK(arbiter.request(RadioMode::remote_pairing, now(), 20000, false) == RadioResult::granted);
}

// A disconnect while advertising stays enabled keeps Matter; the disable then
// releases the radio.
void disconnect_before_disable() {
    begin();
    idle_matter();
    gap_connect(1);
    gap_disconnect(1);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    window_open = advertising = false;
    disable_matter();
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// Each physical peripheral holds Matter until its own disconnect; connection
// updates, failed connects, repeated connects and central-role remotes change
// nothing.
void unrelated_events() {
    begin();
    idle_matter();
    gap_connect(11);
    gap_connect(12);
    gap_connect(13, gap_role_master);
    window_open = advertising = false;
    disable_matter();
    for (unsigned count = 0; count < 3; ++count)
        hci_event(BLE_HCI_EVCODE_LE_META, {BLE_HCI_LE_SUBEV_CONN_UPD_COMPLETE, 0, 11, 0, 0, 0, 0, 0, 0, 0});
    gap_connect(12);
    gap_connect(14, gap_role_slave, 13);
    gap_disconnect(11);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    uint64_t admission = 99;
    CHECK(reserve_consortium(false, admission) == ESP_ERR_INVALID_STATE && admission == 0);
    gap_disconnect(12);
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
    CHECK(gap_roles.count(13) == 1);
    CHECK(remote_request(20, nullptr));
}

// PASE, fail-safe and window state hold no BLE: a committed disable releases
// the radio, and remote scans proceed.
void ip_state_releases() {
    begin();
    idle_matter();
    pase_session = fail_safe = fail_safe_busy = true;
    disable_matter();
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
    CHECK(remote_request(20, nullptr));
}

// PASE and fail-safe state do not stop a consortium setup.
void ip_state_admits() {
    begin();
    idle_matter();
    window_open = false;
    pase_session = fail_safe = fail_safe_busy = true;
    uint64_t admitted = 0;
    CHECK(reserve_consortium(false, admitted) == ESP_OK && admitted);
}

// The initial scheduled-advertisement gap and a failed disable keep Matter;
// a committed disable releases it; an SDK retry reacquires BLE without the old
// disable's authority, and its physical link is not preempted.
void committed_disable_retry() {
    begin();
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true) == CHIP_NO_ERROR);
    }
    CHECK(desired_advertising && !advertising);
    amity_fake::advance_ms(900001);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    advertising_error = CHIP_ERROR_INTERNAL;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false) == CHIP_ERROR_INTERNAL);
    }
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction && desired_advertising);
    advertising_error = CHIP_NO_ERROR;
    disable_matter();
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
    pase_session = fail_safe = true;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        CHECK(chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true) == CHIP_NO_ERROR);
    }
    CHECK(desired_advertising && matter_disabled_generation == 0 && reservation().owner == RadioMode::matter_induction);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    advertising = true;
    gap_connect(25);
    disable_matter();
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    gap_disconnect(25);
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// A committed disable releases only its own generation.
void disable_fences_generation() {
    begin();
    idle_matter();
    window_open = false;
    disable_matter();
    const auto previous = reservation();
    replace_owner();
    release_idle_matter(previous.generation);
    CHECK(reservation().owner == RadioMode::remote_pairing);
    disable_matter();
    CHECK(reservation().owner == RadioMode::remote_pairing);
    {
        Lock lock;
        const auto remote = arbiter.reservation();
        arbiter.release(remote.generation, now());
        arbiter.cleanup_complete(arbiter.reservation().generation, now());
        CHECK(arbiter.request(RadioMode::matter_induction, now(), 300000, false) == RadioResult::granted);
    }
    const auto fresh = reservation();
    release_idle_matter(previous.generation);
    worker_once();
    CHECK(reservation().generation == fresh.generation && reservation().owner == RadioMode::matter_induction);
}

// A peripheral that connects during the transition delays its cleanup until it
// disconnects.
void late_peripheral_in_transition() {
    begin();
    idle_matter();
    window_open = advertising = false;
    disable_matter();
    {
        Lock lock;
        CHECK(arbiter.release(arbiter.reservation().generation, now()) == RadioResult::cancelling);
    }
    gap_connect(31);
    worker_once();
    CHECK(reservation().owner == RadioMode::transition);
    gap_disconnect(31);
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// An invalid role makes tracking uncertain until a successful controller reset.
void invalid_role() {
    begin();
    idle_matter();
    window_open = advertising = false;
    disable_matter();
    gap_connect(51, 2);
    CHECK(peripheral_tracking_failed);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    controller_reset(1);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    controller_reset();
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// An expired consortium workflow is cancelled through its owner before its
// reservation expires.
void expired_workflow() {
    begin();
    uint64_t admitted = 0;
    CHECK(reserve_consortium(false, admitted) == ESP_OK);
    amity_fake::advance_ms(RadioArbiter::enrollment_ms);
    amity_seam::install<&runtime_cancel_radio_workflows>([] {
        CHECK(!radio_held());
        return false;
    });
    worker_once();
    CHECK(reservation().owner == RadioMode::consortium_candidate && reservation().generation == admitted);
    amity_seam::install<&runtime_cancel_radio_workflows>([] {
        CHECK(!radio_held());
        return true;
    });
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// A disable that arrives while Matter waits for another owner's cleanup is not
// Matter's: once Matter owns the radio, it keeps it.
void disable_before_matter_owns() {
    begin();
    {
        Lock lock;
        arbiter.remote_reconnect_running(true);
    }
    cleanup_ready_at = 1000;
    CHECK(!matter_will_advertise() && reservation().owner == RadioMode::transition);
    disable_matter();
    cleanup_ready_at = 0;
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
}

// More peripherals than the table holds make tracking uncertain for good.
void table_overflow() {
    begin();
    idle_matter();
    window_open = advertising = false;
    disable_matter();
    for (uint16_t handle = 1; handle <= 4; ++handle) gap_connect(handle);
    for (uint16_t handle = 1; handle <= 4; ++handle) gap_disconnect(handle);
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
}

// Legacy and enhanced connection events: central links and failed connects are
// ignored, handle 0, the highest handle and repeats are tracked, a failed
// disconnect removes nothing, and each disconnect removes only its link.
void hci_connection_forms() {
    begin();
    for (const uint8_t subevent : {BLE_HCI_LE_SUBEV_CONN_COMPLETE, BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE,
                                   BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2}) {
        gap_connect(0, gap_role_master, 0, subevent);
        CHECK(!peripheral_active());
        gap_connect(0, gap_role_slave, 13, subevent);
        CHECK(!peripheral_active());
        gap_connect(0, gap_role_slave, 0, subevent);
        gap_connect(0, gap_role_slave, 0, subevent);
        gap_connect(BLE_HCI_LE_CONN_HANDLE_MAX, gap_role_slave, 0, subevent);
        CHECK(peripheral_active() && !peripheral_tracking_failed);
        gap_disconnect(0, 1);
        CHECK(peripheral_active());
        gap_disconnect(BLE_HCI_LE_CONN_HANDLE_MAX);
        CHECK(peripheral_active());
        gap_disconnect(0);
        CHECK(!peripheral_active());
    }
}

// Truncated connection and disconnection events make tracking uncertain; a
// short failed connect, short or unknown LE events and short command
// completions change nothing; an out-of-range handle is uncertain. A
// successful controller reset clears the uncertainty between cases, but a
// truncated one does not.
void hci_minimum_lengths() {
    begin();
    for (const uint8_t subevent : {BLE_HCI_LE_SUBEV_CONN_COMPLETE, BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE,
                                   BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE_V2}) {
        const unsigned full = subevent == BLE_HCI_LE_SUBEV_CONN_COMPLETE ? 19
                              : subevent == BLE_HCI_LE_SUBEV_ENH_CONN_COMPLETE ? 31 : 34;
        for (unsigned length = 1; length < full; ++length) {
            std::vector<uint8_t> payload(length);
            payload[0] = subevent;
            hci_event(BLE_HCI_EVCODE_LE_META, payload);
            CHECK(peripheral_tracking_failed);
            controller_reset();
            CHECK(!peripheral_tracking_failed && !peripheral_active());
        }
        hci_event(BLE_HCI_EVCODE_LE_META, {subevent, 13});
        CHECK(!peripheral_active());
    }
    for (unsigned length = 0; length < 4; ++length) {
        hci_event(BLE_HCI_EVCODE_DISCONN_CMP, std::vector<uint8_t>(length));
        CHECK(peripheral_tracking_failed);
        controller_reset();
    }
    hci_event(BLE_HCI_EVCODE_LE_META, {});
    hci_event(BLE_HCI_EVCODE_LE_META, {0xff});
    for (unsigned length = 0; length < 4; ++length) hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE, std::vector<uint8_t>(length));
    CHECK(!peripheral_active());
    gap_connect(0x0f00);
    CHECK(peripheral_tracking_failed);
    // A reset completion too short for its status clears nothing.
    hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE, {1, 3, 0x0c});
    CHECK(peripheral_tracking_failed);
}

// A physical connection is visible before NimBLE delivers it and before the
// advertising-stop acknowledgement, so the acknowledgement cannot release it.
void hci_before_delivery() {
    begin();
    idle_matter();
    window_open = false;
    advert_stop_at = 100;
    disable_matter();
    on_dispatch = [](uint8_t *packet, void *) {
        if (packet[0] == BLE_HCI_EVCODE_LE_META) {
            CHECK(peripheral_active());
            CHECK(!desired_advertising);
        }
    };
    gap_connect(41);
    on_dispatch = [](uint8_t *, void *) {
        advertising = false;
        worker_once();
    };
    hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE, {1, 0x0a, 0x20, 0});
    CHECK(reservation().owner == RadioMode::matter_induction);
    on_dispatch = {};
    gap_disconnect(41);
    worker_once();
    CHECK(reservation().owner == RadioMode::operational);
}

// Raw HCI observation never takes the radio mutex, so it records a link while
// the application holds it.
void hci_under_radio_mutex() {
    begin();
    std::array<uint8_t, 21> packet{};
    packet[0] = BLE_HCI_EVCODE_LE_META;
    packet[1] = 19;
    packet[2] = BLE_HCI_LE_SUBEV_CONN_COMPLETE;
    packet[4] = 8;
    packet[6] = gap_role_slave;
    Lock lock;
    matter_hci_event(packet.data());
    CHECK(peripheral_active());
}

// Only a successful controller reset clears live links; other successful
// commands and failed resets do not.
void controller_reset_clears() {
    begin();
    idle_matter();
    window_open = advertising = false;
    disable_matter();
    gap_connect(1);
    gap_connect(2);
    hci_event(BLE_HCI_EVCODE_COMMAND_COMPLETE, {1, 0x0a, 0x20, 0});
    worker_once();
    CHECK(reservation().owner == RadioMode::matter_induction);
    controller_reset(1);
    worker_once();
    CHECK(peripheral_active());
    controller_reset();
    worker_once();
    CHECK(!peripheral_active() && reservation().owner == RadioMode::operational);
}

// Before start() every entry point fails closed. start() registers the remote
// and runtime hooks and the worker task, and refuses to run twice.
void start_registers() {
    script_sdk();
    uint64_t generation = 7;
    CHECK(!matter_will_advertise() && reserve_consortium(false, generation) == ESP_ERR_INVALID_STATE && generation == 0);
    CHECK(!remote_request(20, nullptr));
    remote_done(nullptr);
    matter_advertising_disabled();
    release(1);
    CHECK(amity_seam::calls().empty());
    CHECK(start() == ESP_OK);
    CHECK(admit_hook == &remote_request && done_hook == &remote_done && remote_context == nullptr);
    const auto tasks = amity_fake::tasks();
    CHECK(tasks.size() == 1 && tasks[0].name == "radio_owner" && tasks[0].entry == &worker);
    CHECK(tasks[0].stack_bytes == 3072 && tasks[0].priority == 4 && tasks[0].argument == nullptr);
    CHECK(start() == ESP_ERR_INVALID_STATE && amity_fake::tasks().size() == 1);
    uint64_t admitted = 0;
    CHECK(radio_hooks.reserve(true, admitted) == ESP_OK && reservation().owner == RadioMode::consortium_inviter);
    radio_hooks.release(admitted);
    CHECK(reservation().owner == RadioMode::transition);
    // The created task is the worker: it waits for cleanup, then delays.
    amity_fake::on_delay([](TickType_t) { throw amity_fake::LeaveTask{}; });
    amity_fake::run_task(tasks[0].handle);
    CHECK(amity_fake::tasks()[0].state == amity_fake::Task::State::left);
}

// start() reports a failed mutex or task creation.
void start_failures() {
    script_sdk();
    amity_fake::fail_queue_creations(1);
    CHECK(start() == ESP_ERR_NO_MEM && !mutex && amity_seam::calls().empty());
    amity_fake::fail_task_creations(1);
    CHECK(start() == ESP_ERR_NO_MEM && mutex && amity_fake::tasks().empty());
}

// A remote scan reserves the radio, and its completion releases that
// reservation once.
void remote_hooks() {
    begin();
    CHECK(admit_hook(20, nullptr));
    const auto scan = reservation();
    CHECK(scan.owner == RadioMode::remote_pairing && scan.deadline_ms == 20000 && remote_generation == scan.generation);
    CHECK(!admit_hook(20, nullptr));
    done_hook(nullptr);
    CHECK(reservation().owner == RadioMode::transition && remote_generation == 0);
    const auto cleanup = reservation();
    done_hook(nullptr);
    CHECK(reservation().generation == cleanup.generation);
}

// Matter advertising is refused while a workflow cannot be cancelled, while
// the remote cannot cancel its setup or pause, while another owner's cleanup
// is unfinished, and during a transition.
void advertising_refused() {
    begin();
    amity_seam::install<&runtime_cancel_radio_workflows>([] { return false; });
    CHECK(__wrap__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(nullptr, true) == CHIP_ERROR_BUSY);
    CHECK(!desired_advertising && reservation().owner == RadioMode::operational);
    amity_seam::install<&runtime_cancel_radio_workflows>([] { return true; });
    amity_seam::install<&ble_remote_cancel_setup>([] { return ESP_ERR_TIMEOUT; });
    CHECK(!matter_will_advertise() && reservation().owner == RadioMode::operational);
    amity_seam::install<&ble_remote_cancel_setup>([] { return ESP_OK; });
    cleanup_ready_at = 1;
    CHECK(!matter_will_advertise() && reservation().owner == RadioMode::matter_induction && pauses == 1);
    replace_owner();
    cleanup_ready_at = clock_ms() + 1000;
    CHECK(!matter_will_advertise() && reservation().owner == RadioMode::transition && pauses == 2);
    cleanup_ready_at = 0;
    CHECK(!matter_will_advertise() && reservation().owner == RadioMode::transition && pauses == 2);
}

}  // namespace test
}  // namespace amity::radio

// The CHIP BLE manager's advertising switch, which the firmware wraps. Only
// the SDK reaches it, holding the CHIP lock without the radio mutex.
extern "C" CHIP_ERROR __real__ZN4chip11DeviceLayer8Internal14BLEManagerImpl22_SetAdvertisingEnabledEb(void *, bool enabled) {
    using namespace amity::radio::test;
    CHECK(amity_fake::chip_stack_locked() && !radio_held() && !in_critical_section());
    if (advertising_error != CHIP_NO_ERROR) return advertising_error;
    desired_advertising = enabled;
    if (!enabled) {
        ++disables;
        if (!advert_stop_at) advertising = false;
        const auto hook = on_disable;
        if (hook) hook();
    }
    return CHIP_NO_ERROR;
}

// NimBLE's host delivery of an HCI event, after the wrapper observed it.
extern "C" int __real_ble_hs_hci_rx_evt(uint8_t *buffer, void *context) {
    using namespace amity::radio::test;
    CHECK(!radio_held() && !in_critical_section() && context == expected_context && buffer == expected_event);
    ++dispatches;
    const auto hook = on_dispatch;
    if (hook) hook(buffer, context);
    return 73;
}

// NimBLE's advertiser state, which the firmware queries outside the radio
// mutex and the critical section.
extern "C" int ble_gap_adv_active(void) {
    using namespace amity::radio::test;
    CHECK((!amity_fake::chip_stack_locked() || sdk_advertising_call) && !radio_held() && !in_critical_section());
    return advertising;
}

int main(int argc, char **argv) {
    using namespace amity::radio::test;
    return amity_test::run(argc, argv,
                           {{"idle-handoff-candidate", idle_handoff<false>},
                            {"idle-handoff-inviter", idle_handoff<true>},
                            {"active-denied", active_denied},
                            {"other-busy", other_busy},
                            {"stale-disable", stale_disable},
                            {"stale-cleanup", stale_cleanup},
                            {"bounded-timeout", bounded_timeout},
                            {"shared-budget", shared_budget},
                            {"peripheral-during-disable", peripheral_during_disable},
                            {"disable-refused", disable_refused},
                            {"handoff-after-other-owner", handoff_after_other_owner},
                            {"late-cleanup-handoff", late_cleanup<false>},
                            {"late-cleanup-release", late_cleanup<true>},
                            {"late-cleanup-result", late_cleanup_result},
                            {"late-advertising-cleanup", late_advertising_cleanup},
                            {"ordinary", ordinary},
                            {"worker-consumes-disable", worker_during_disable<false>},
                            {"worker-finishes-cleanup", worker_during_disable<true>},
                            {"disable-before-disconnect", disable_before_disconnect},
                            {"disconnect-before-disable", disconnect_before_disable},
                            {"unrelated-events", unrelated_events},
                            {"ip-state-releases", ip_state_releases},
                            {"ip-state-admits", ip_state_admits},
                            {"committed-disable-retry", committed_disable_retry},
                            {"disable-fences-generation", disable_fences_generation},
                            {"late-peripheral-in-transition", late_peripheral_in_transition},
                            {"expired-workflow", expired_workflow},
                            {"disable-before-matter-owns", disable_before_matter_owns},
                            {"invalid-role", invalid_role},
                            {"table-overflow", table_overflow},
                            {"hci-connection-forms", hci_connection_forms},
                            {"hci-minimum-lengths", hci_minimum_lengths},
                            {"hci-before-delivery", hci_before_delivery},
                            {"hci-under-radio-mutex", hci_under_radio_mutex},
                            {"controller-reset", controller_reset_clears},
                            {"start", start_registers},
                            {"start-failures", start_failures},
                            {"remote-hooks", remote_hooks},
                            {"advertising-refused", advertising_refused}});
}
