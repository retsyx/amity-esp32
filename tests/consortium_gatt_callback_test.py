#!/usr/bin/env python3
"""Compile actual GATT callbacks against a cancel-without-event controller.

Only the controller, advertisement parser, and FreeRTOS notification seams are
faked. The completion, generation checks, GAP callback, and disconnect function
are extracted unchanged from the firmware source. No Bluetooth hardware is used.
--source permits running the same assertions against an earlier source snapshot.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


PREFIX = r'''
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

constexpr int BLE_GAP_EVENT_DISC = 1, BLE_GAP_EVENT_DISC_COMPLETE = 2;
constexpr int BLE_GAP_EVENT_CONNECT = 3, BLE_GAP_EVENT_DISCONNECT = 4;
constexpr int BLE_HS_ENOENT = 11, BLE_HS_ENOTCONN = 12;
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;
constexpr int BLE_ERR_REM_USER_CONN_TERM = 19;
struct ble_addr_t { uint8_t type = 0; uint8_t val[6]{}; };
struct ble_gap_event {
    int type = 0;
    struct { const uint8_t *data = nullptr; uint8_t length_data = 0; ble_addr_t addr{}; } disc;
    struct { int status = 0; uint16_t conn_handle = 0; } connect;
    struct { struct { uint16_t conn_handle = 0; } conn; } disconnect;
};
struct ble_hs_adv_fields { const uint8_t *name = nullptr; uint8_t name_len = 0; };
int ble_hs_adv_parse_fields(ble_hs_adv_fields *fields, const uint8_t *data, uint8_t length) {
    fields->name = data; fields->name_len = length; return 0;
}
struct Identity { std::string str() const { return "00000000000000000000000000abcdef"; } };
std::mutex client_mutex;
uint64_t client_generation = 7;
uint16_t client_connection = BLE_HS_CONN_HANDLE_NONE, cancelled_client = BLE_HS_CONN_HANDLE_NONE;
ble_addr_t candidate_address{};
Identity expected_candidate;
bool found_candidate = false, client_workflow = true, client_discovery_waiting = true;
int client_error = 0;
void *client_done = nullptr;
int wakes = 0, cancellations = 0, cancellation_error = 0, connection_cancellations = 0, terminations = 0;
uint16_t terminated_handle = BLE_HS_CONN_HANDLE_NONE;
void xSemaphoreGive(void *) { ++wakes; }
int ble_gap_disc_cancel() {
    ++cancellations;
    // Pinned NimBLE ble_gap_disc_cancel_no_lock disables discovery and resets
    // its master state. It does NOT deliver BLE_GAP_EVENT_DISC_COMPLETE.
    return cancellation_error;
}
int ble_gap_conn_cancel() { ++connection_cancellations; return 0; }
int ble_gap_terminate(uint16_t handle, int) { ++terminations; terminated_handle = handle; return 0; }
'''

SUITE = r'''
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void reset() {
    client_generation = 7; client_connection = cancelled_client = BLE_HS_CONN_HANDLE_NONE;
    candidate_address = {}; found_candidate = false; client_workflow = client_discovery_waiting = true;
    client_error = wakes = cancellations = cancellation_error = connection_cancellations = terminations = 0;
    terminated_handle = BLE_HS_CONN_HANDLE_NONE;
}
void *token(uintptr_t generation = 7) { return reinterpret_cast<void *>(generation); }
ble_gap_event advertisement(const std::string &name) {
    ble_gap_event event; event.type = BLE_GAP_EVENT_DISC;
    event.disc.data = reinterpret_cast<const uint8_t *>(name.data());
    event.disc.length_data = static_cast<uint8_t>(name.size()); event.disc.addr.val[0] = 42;
    return event;
}
void match_wakes_without_controller_completion() {
    const std::string name = "A-abcdef"; auto event = advertisement(name); client_gap(&event, token());
    check(found_candidate && candidate_address.val[0] == 42, "matching address not retained");
    check(cancellations == 1, "matching scan was not cancelled exactly once");
    check(wakes == 1 && client_error == 0, "matching advertisement did not wake scan waiter without DISC_COMPLETE");
    check(!client_discovery_waiting, "completed discovery is still armed");
}
void late_completion_cannot_wake_connection_waiter() {
    match_wakes_without_controller_completion(); wakes = 0;
    ble_gap_event event; event.type = BLE_GAP_EVENT_DISC_COMPLETE; client_gap(&event, token());
    check(wakes == 0, "late scan completion woke subsequent connection waiter");
    event.type = BLE_GAP_EVENT_CONNECT; event.connect.conn_handle = 31; client_gap(&event, token());
    check(wakes == 1 && client_connection == 31 && client_error == 0, "connection callback did not wake its own waiter");
}
void repeated_match_cannot_complete_twice() {
    match_wakes_without_controller_completion();
    const std::string name = "A-abcdef"; auto event = advertisement(name); client_gap(&event, token());
    check(wakes == 1 && cancellations == 1, "duplicate advertisement completed discovery twice");
}
void unrelated_advertisement_and_natural_completion() {
    const std::string name = "A-000000"; auto event = advertisement(name); client_gap(&event, token());
    check(!found_candidate && wakes == 0 && cancellations == 0, "unrelated candidate admitted");
    event.type = BLE_GAP_EVENT_DISC_COMPLETE; client_gap(&event, token());
    check(wakes == 1 && client_error == BLE_HS_ENOENT && !client_discovery_waiting, "empty natural scan did not fail once");
    client_gap(&event, token()); check(wakes == 1, "natural completion delivered twice");
}
void cancellation_failure_wakes_failure() {
    cancellation_error = 88; const std::string name = "A-abcdef"; auto event = advertisement(name);
    client_gap(&event, token());
    check(wakes == 1 && client_error == 88, "cancel failure was hidden or scan waiter remained blocked");
    event.type = BLE_GAP_EVENT_DISC_COMPLETE; client_gap(&event, token());
    check(wakes == 1 && client_error == 88, "late completion overwrote cancellation failure");
}
void stale_generation_cannot_wake_or_replace_address() {
    const std::string name = "A-abcdef"; auto event = advertisement(name); client_gap(&event, token(6));
    check(!found_candidate && wakes == 0 && cancellations == 0 && candidate_address.val[0] == 0, "stale scan generation admitted");
    event.type = BLE_GAP_EVENT_CONNECT; event.connect.conn_handle = 31; client_gap(&event, token(6));
    check(wakes == 0 && client_connection == BLE_HS_CONN_HANDLE_NONE, "stale connection generation admitted");
}
void explicit_disconnect_fences_old_callbacks() {
    client_connection = 31; gatt_client_disconnect();
    check(client_generation == 8 && !client_workflow && !client_discovery_waiting, "disconnect did not fence workflow");
    check(terminations == 1 && terminated_handle == 31 && cancelled_client == 31, "disconnect did not close exact own connection");
    check(cancellations == 1 && connection_cancellations == 1, "disconnect did not cancel own pending procedures");
    check(wakes == 0 && client_connection == BLE_HS_CONN_HANDLE_NONE, "disconnect fabricated successful completion");
    const std::string name = "A-abcdef"; auto event = advertisement(name); client_gap(&event, token());
    event.type = BLE_GAP_EVENT_DISC_COMPLETE; client_gap(&event, token());
    event.type = BLE_GAP_EVENT_DISCONNECT; event.disconnect.conn.conn_handle = 31; client_gap(&event, token());
    check(wakes == 0 && cancellations == 1 && !found_candidate, "cancelled generation delivered late completion");
    gatt_client_disconnect(); check(client_generation == 8 && terminations == 1, "repeated disconnect changed generation or terminated twice");
}
void active_disconnect_only_completes_matching_handle() {
    client_connection = 31; ble_gap_event event; event.type = BLE_GAP_EVENT_DISCONNECT;
    event.disconnect.conn.conn_handle = 32; client_gap(&event, token());
    check(wakes == 0 && client_connection == 31, "unrelated connection disconnect completed waiter");
    event.disconnect.conn.conn_handle = 31; client_gap(&event, token());
    check(wakes == 1 && client_connection == BLE_HS_CONN_HANDLE_NONE && client_error == BLE_HS_ENOTCONN, "own disconnect was not surfaced");
}
int main() {
    struct Case { const char *name; void (*run)(); };
    const Case cases[] = {
        {"matching cancellation wakes without DISC_COMPLETE", match_wakes_without_controller_completion},
        {"late DISC_COMPLETE cannot complete connection", late_completion_cannot_wake_connection_waiter},
        {"repeated advertisement completes once", repeated_match_cannot_complete_twice},
        {"unrelated advertisement / natural timeout", unrelated_advertisement_and_natural_completion},
        {"cancellation failure remains failure", cancellation_failure_wakes_failure},
        {"stale generation has no effects", stale_generation_cannot_wake_or_replace_address},
        {"explicit disconnect fences callbacks", explicit_disconnect_fences_old_callbacks},
        {"only matching disconnect completes", active_disconnect_only_completes_matching_handle},
    };
    int failures = 0;
    for (const auto &test : cases) {
        reset();
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception &error) { ++failures; std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    return failures ? 1 : 0;
}
'''


def function(source: str, declaration: str) -> str:
    start = source.index(declaration)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'components/consortium/gatt_transport.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    actual = ''.join(function(source, declaration) for declaration in (
        'void complete(int error)', 'bool current(void *token)',
        'int client_gap(ble_gap_event *event,void *token)', 'void gatt_client_disconnect()'))
    with tempfile.TemporaryDirectory(prefix='amity-gatt-callback-') as directory:
        cpp = Path(directory) / 'actual_callback_test.cpp'
        binary = Path(directory) / 'actual_callback_test'
        cpp.write_text(PREFIX + actual + SUITE)
        compiler = shlex.split(os.environ.get('CXX', 'c++'))
        subprocess.run(compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g',
                                  '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
