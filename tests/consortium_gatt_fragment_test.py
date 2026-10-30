#!/usr/bin/env python3
"""Exercise actual client record fragmentation with an ATT-512 controller seam."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from consortium_gatt_callback_test import function


PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;
// Pinned IDF NimBLE host/include/host/ble_att.h: BLE_ATT_ATTR_MAX_LEN.
constexpr size_t BLE_ATT_ATTR_MAX_LEN = 512;
uint16_t client_connection = 31;
uint64_t client_generation = 7;
uint32_t client_callback_cookie = 7;
uint32_t client_record = 0;
std::mutex client_mutex;
std::mutex state_mutex;
std::atomic_int client_error{0};
std::string client_response;
int on_write = 0, on_read_fragment = 0;
unsigned fake_mtu = 23;
uint32_t request_record = 0;
unsigned request_endpoint = 0;
size_t response_offset = 0, max_write = 0, max_read = 0;
uint32_t response_record = 0;
uint8_t response_endpoint = 0, response_status = 1;
uint16_t server_connection = 31;
uint64_t generation = 7, server_session = 9, deadline = 100000000;
bool server_open = true;
std::atomic_uint failed_proofs{0};
std::string srv_response;
constexpr int BLE_GATT_ACCESS_OP_WRITE_CHR = 1, BLE_GATT_ACCESS_OP_READ_CHR = 2;
constexpr int BLE_ATT_ERR_INSUFFICIENT_AUTHEN = 11, BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN = 12,
              BLE_ATT_ERR_UNLIKELY = 13, BLE_ATT_ERR_INSUFFICIENT_RES = 14, BLE_ATT_ERR_INVALID_OFFSET = 15;
struct os_mbuf { std::string bytes; };
#define OS_MBUF_PKTLEN(value) ((value)->bytes.size())
int os_mbuf_copydata(os_mbuf* value, size_t offset, size_t count, void* out) {
    if (offset > value->bytes.size() || count > value->bytes.size() - offset) return 1;
    memcpy(out, value->bytes.data() + offset, count); return 0;
}
int os_mbuf_append(os_mbuf* value, const void* data, size_t count) {
    value->bytes.append(static_cast<const char*>(data), count); return 0;
}
struct ble_gatt_access_ctxt { int op; os_mbuf* om; unsigned offset = 0; };
struct Work {
    uint64_t generation = 0, session = 0;
    uint16_t connection = 0, total = 0, offset = 0, length = 0;
    uint32_t record = 0; uint8_t endpoint = 0, bytes[512]{};
};
void* work_queue = nullptr;
unsigned uxQueueMessagesWaiting(void*) { return 0; }
bool post_work(const Work&) { return true; }
int access(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*);
std::string received, answer;
bool bad_attribute = false, bad_frame = false;
uint64_t clock_ms = 100;
unsigned operations = 0, request_writes = 0, cursor_writes = 0, reads = 0, waits = 0;
unsigned pending_reads_left = 0, fail_operation = 0, fail_wait = 0, wait_ms = 11;
bool rejected_response = false, malformed_response = false, publish_before_cursor = false;
std::vector<unsigned> wait_limits;
std::vector<std::string> timing_lines;
uint64_t now_ms() { return clock_ms; }
void vTaskDelay(unsigned ms) { clock_ms += ms; }
unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
unsigned ble_att_mtu(uint16_t) { return fake_mtu; }
struct ble_gap_conn_desc { uint16_t conn_itvl = 32, conn_latency = 0; };
int ble_gap_conn_find(uint16_t, ble_gap_conn_desc* out) { *out = {}; return 0; }
void clear_done() {}
bool wait(unsigned timeout) {
    ++waits; wait_limits.push_back(timeout);
    if (waits == fail_wait) { clock_ms += timeout; client_error = 77; return false; }
    clock_ms += wait_ms; return true;
}
void timing_log(const char* tag, const char* format, ...) {
    bool unlocked = false;
    std::thread probe([&] {
        if (client_mutex.try_lock()) { client_mutex.unlock();
            if (state_mutex.try_lock()) { state_mutex.unlock(); unlocked = true; }
        }
    }); probe.join();
    if (!unlocked) throw std::runtime_error("timing logger ran under transport locks");
    char text[768]; va_list args; va_start(args, format);
    const auto size = vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (std::string(tag) != "setup_timing" || size < 0 || size >= 384)
        throw std::runtime_error("unbounded timing output");
    timing_lines.emplace_back(text);
}
#define ESP_LOGI(...) timing_log(__VA_ARGS__)
unsigned u16(const uint8_t *value) { return (unsigned(value[0]) << 8) | value[1]; }
uint32_t u32(const uint8_t *value) {
    return (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) | (uint32_t(value[2]) << 8) | value[3];
}
int ble_gattc_write_flat(uint16_t, uint16_t, const void *input, size_t length, int, void *) {
    clock_ms += 7; if (++operations == fail_operation) return 90;
    max_write = std::max(max_write, length);
    // Model controller/remote ATT value constraints before application callback.
    if (length > BLE_ATT_ATTR_MAX_LEN || length > fake_mtu - 3) { bad_attribute = true; return 90; }
    const auto *bytes = static_cast<const uint8_t *>(input);
    if (length == 7 && bytes[0] == 2) {
        ++cursor_writes;
        if (u32(bytes + 1) != request_record) { bad_frame = true; return 91; }
        os_mbuf value{std::string(static_cast<const char*>(input), length)};
        ble_gatt_access_ctxt context{BLE_GATT_ACCESS_OP_WRITE_CHR, &value};
        return access(client_connection, 17, &context, nullptr);
    }
    ++request_writes;
    if (length <= 10 || bytes[0] != 1 || u16(bytes + 8) != received.size()) { bad_frame = true; return 91; }
    request_record = u32(bytes + 2); request_endpoint = bytes[1];
    received.append(reinterpret_cast<const char *>(bytes + 10), length - 10);
    if (received.size() > u16(bytes + 6)) { bad_frame = true; return 91; }
    if (publish_before_cursor && received.size() == u16(bytes + 6)) {
        response_record = request_record; response_endpoint = uint8_t(request_endpoint);
        response_status = 1; srv_response = answer;
    }
    return 0;
}
int ble_gattc_read(uint16_t, uint16_t, int, void *) {
    clock_ms += 7; ++reads; if (++operations == fail_operation) return 90;
    if (pending_reads_left) --pending_reads_left;
    else if (response_record != request_record) {
        // Match actual worker publication: record/endpoint/status/body change,
        // but the cursor set before publication is deliberately untouched.
        response_record = request_record; response_endpoint = uint8_t(request_endpoint);
        response_status = rejected_response ? 2 : 1; srv_response = answer;
    }
    os_mbuf value; ble_gatt_access_ctxt context{BLE_GATT_ACCESS_OP_READ_CHR, &value};
    const auto error = access(client_connection, 17, &context, nullptr);
    if (error) return error;
    client_response = std::move(value.bytes);
    if (malformed_response && response_record == request_record) client_response[11] = 1;
    max_read = std::max(max_read, client_response.size());
    return 0;
}
'''

SUITE = r'''
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
uint64_t metric(const std::string& line, const std::string& key) {
    const auto start = line.find(key + "=");
    if (start == std::string::npos) throw std::runtime_error("missing timing metric");
    return std::stoull(line.substr(start + key.size() + 1));
}
void reset(unsigned mtu = 527) {
    fake_mtu = mtu; client_record = request_record = 0; client_error = 0;
    received.clear(); client_response.clear(); max_write = max_read = response_offset = 0;
    response_record = 0; response_endpoint = 0; response_status = 1;
    srv_response = "old-private-reply"; response_offset = 5;
    bad_attribute = bad_frame = rejected_response = malformed_response = publish_before_cursor = false;
    clock_ms = 100; operations = request_writes = cursor_writes = reads = waits = 0;
    pending_reads_left = fail_operation = fail_wait = 0; wait_ms = 11;
    wait_limits.clear(); timing_lines.clear();
}
void timing_tests() {
    const std::string secret = "private-password-SSID-token-certificate";
    for (const bool encrypted : {false, true}) {
        reset(); pending_reads_left = 2; answer.assign(900, 'z'); std::string response;
        check(gatt_client_exchange(encrypted, secret, response), "timing changed successful record bytes");
        check(received == secret && response == answer && !bad_frame, "framed request/reply bytes changed");
        check(timing_lines.size() == 1, "per-packet timing noise was emitted");
        const auto& line = timing_lines.front();
        check(line.find(secret) == std::string::npos && line.find(answer) == std::string::npos,
              "timing disclosed request or response content");
        check(metric(line, "record") == 1 && metric(line, "endpoint") == unsigned(encrypted) &&
              metric(line, "writes") == 1 && metric(line, "cursors") == 2 && metric(line, "reads") == 4 &&
              metric(line, "pending") == 2 && metric(line, "fragments") == 2,
              "timing counters do not match actual unchanged ATT operations");
        check(cursor_writes == 2 && reads == 4 && response_record == 1 && response_offset == 500,
              "pending reads rewrote the same cursor or late publication changed its position");
        check(metric(line, "att_wait_ms") == 126 && metric(line, "att_max_ms") == 18 &&
              metric(line, "elapsed_ms") == 176 && metric(line, "request_ms") == 18 &&
              metric(line, "reply_ms") == 158 && metric(line, "at_ms") == 276,
              "timing did not measure SDK submission, callback wait and pending delay separately");
        check(wait_limits == std::vector<unsigned>(7, 5000), "ATT wait bounds changed");
    }
    reset(); publish_before_cursor = true; answer.assign(900, 'z'); std::string ready_response;
    check(gatt_client_exchange(true, secret, ready_response) && ready_response == answer &&
          cursor_writes == 2 && reads == 2 && response_offset == 500,
          "already-published response did not reset its stale cursor before the first read");
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset(); answer = "reply"; std::string response;
        if (failure == 0) fail_operation = 1;
        if (failure == 1) fail_wait = 1;
        if (failure == 2) rejected_response = true;
        if (failure == 3) malformed_response = true;
        if (failure == 4) { pending_reads_left = 1000; wait_ms = 1000; }
        check(!gatt_client_exchange(true, secret, response) && timing_lines.size() == 1,
              "failure timing changed outcome or emitted per-packet logs");
        const auto& line = timing_lines.front();
        const auto stage = failure < 2 ? "request_write" : failure == 2 ? "response_rejected" : failure == 3 ? "response_frame" : "response_deadline";
        check(line.find(std::string("result=failed failure=") + stage) != std::string::npos &&
              line.find(secret) == std::string::npos, "failure timing lost static stage or disclosed content");
        if (failure < 2) check(operations == 1 && reads == 0, "failed ATT request was retried");
        if (failure == 4) check(clock_ms >= 45100 && clock_ms < 50100,
                               "reply polling deadline changed");
    }
    reset(); answer = "reply"; std::string response = "unchanged";
    check(!gatt_client_exchange(true, "", response) && response == "unchanged" && operations == 0 &&
          timing_lines.size() == 1 && timing_lines.front().find("failure=precondition") != std::string::npos,
          "invalid-record timing changed precondition behavior");
    std::cout << "PASS actual client/server cursor: late publication preserves offset, one write per advanced position, exact bytes/deadlines, safe timing and fail-fast outcomes\n";
}
int main() {
    int failures = 0;
    for (const unsigned mtu : {23, 24, 64, 247, 512, 515, 516, 517, 527}) {
        reset(mtu);
        const std::string request(4096, 'x'); answer.assign(1700, 'y'); std::string response;
        const bool completed = gatt_client_exchange(true, request, response);
        if (!completed || bad_attribute || bad_frame || received != request || response != answer ||
            max_write > 512 || max_write > mtu - 3 || max_read > 512 || max_read > mtu - 1) {
            ++failures;
            std::cerr << "FAIL MTU " << mtu << ": exchange=" << completed << " max_attribute_write=" << max_write
                      << " received=" << received.size() << " max_attribute_read=" << max_read << '\n';
        } else {
            std::cout << "PASS MTU " << mtu << ": 4096-byte request / 1700-byte reply, max_attribute_write=" << max_write
                      << " max_attribute_read=" << max_read << '\n';
        }
    }
    try { timing_tests(); } catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL " << error.what() << '\n';
    }
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'components/consortium/gatt_transport.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    timing = function(source, 'struct GattTiming {').rstrip() + ';\n' + function(source, 'void capture_gatt_link(')
    server = function(source, 'int access(uint16_t connection,')
    actual = function(source, 'bool gatt_client_exchange(bool encrypted,')
    with tempfile.TemporaryDirectory(prefix='amity-gatt-fragment-') as directory:
        cpp = Path(directory) / 'actual_fragment_test.cpp'
        binary = Path(directory) / 'actual_fragment_test'
        # The actual exchange references its discovered attribute handle.
        cpp.write_text(PREFIX + '\nuint16_t client_attribute = 17;\n' + timing + server + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
