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
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

constexpr uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;
// Pinned IDF NimBLE host/include/host/ble_att.h: BLE_ATT_ATTR_MAX_LEN.
constexpr size_t BLE_ATT_ATTR_MAX_LEN = 512;
uint16_t client_connection = 31;
uint64_t client_generation = 7;
uint32_t client_record = 0;
std::mutex client_mutex;
std::string client_response;
int on_write = 0, on_read_fragment = 0;
unsigned fake_mtu = 23;
uint32_t request_record = 0;
unsigned request_endpoint = 0;
size_t response_offset = 0, max_write = 0, max_read = 0;
std::string received, answer;
bool bad_attribute = false, bad_frame = false;
uint64_t now_ms() { static uint64_t clock = 0; return ++clock; }
void vTaskDelay(unsigned) {}
unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
unsigned ble_att_mtu(uint16_t) { return fake_mtu; }
void clear_done() {}
bool wait(unsigned) { return true; }
unsigned u16(const uint8_t *value) { return (unsigned(value[0]) << 8) | value[1]; }
uint32_t u32(const uint8_t *value) {
    return (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) | (uint32_t(value[2]) << 8) | value[3];
}
int ble_gattc_write_flat(uint16_t, uint16_t, const void *input, size_t length, int, void *) {
    max_write = std::max(max_write, length);
    // Model controller/remote ATT value constraints before application callback.
    if (length > BLE_ATT_ATTR_MAX_LEN || length > fake_mtu - 3) { bad_attribute = true; return 90; }
    const auto *bytes = static_cast<const uint8_t *>(input);
    if (length == 7 && bytes[0] == 2) {
        if (u32(bytes + 1) != request_record) { bad_frame = true; return 91; }
        response_offset = u16(bytes + 5); return 0;
    }
    if (length <= 10 || bytes[0] != 1 || u16(bytes + 8) != received.size()) { bad_frame = true; return 91; }
    request_record = u32(bytes + 2); request_endpoint = bytes[1];
    received.append(reinterpret_cast<const char *>(bytes + 10), length - 10);
    if (received.size() > u16(bytes + 6)) { bad_frame = true; return 91; }
    return 0;
}
int ble_gattc_read(uint16_t, uint16_t, int, void *) {
    const size_t count = std::min<size_t>({500, fake_mtu - 13, answer.size() - response_offset});
    uint8_t header[12] = {1, 1, uint8_t(request_record >> 24), uint8_t(request_record >> 16),
        uint8_t(request_record >> 8), uint8_t(request_record), uint8_t(answer.size() >> 8),
        uint8_t(answer.size()), uint8_t(request_endpoint), 0, uint8_t(response_offset >> 8), uint8_t(response_offset)};
    client_response.assign(reinterpret_cast<char *>(header), sizeof(header));
    client_response.append(answer, response_offset, count);
    max_read = std::max(max_read, client_response.size());
    return 0;
}
'''

SUITE = r'''
int main() {
    int failures = 0;
    for (const unsigned mtu : {23, 24, 64, 247, 512, 515, 516, 517, 527}) {
        fake_mtu = mtu; client_record = 0; request_record = 0;
        received.clear(); client_response.clear(); max_write = max_read = response_offset = 0;
        bad_attribute = bad_frame = false;
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
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'components/consortium/gatt_transport.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    actual = function(source, 'bool gatt_client_exchange(bool encrypted,')
    with tempfile.TemporaryDirectory(prefix='amity-gatt-fragment-') as directory:
        cpp = Path(directory) / 'actual_fragment_test.cpp'
        binary = Path(directory) / 'actual_fragment_test'
        # The actual exchange references its discovered attribute handle.
        cpp.write_text(PREFIX + '\nuint16_t client_attribute = 17;\n' + actual + SUITE)
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) +
                       ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                        str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
