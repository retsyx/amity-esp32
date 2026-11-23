#!/usr/bin/env python3
"""Check that the linker routes HTTPS handshakes through Amity, which no host build can observe.

web_server_test.cpp runs the HTTPS startup and pre-handshake policy itself.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    assert '"-Wl,--wrap=esp_tls_server_session_create"' in (ROOT / 'components/web/CMakeLists.txt').read_text()
    print('PASS HTTPS handshake linkage: esp_tls_server_session_create is wrapped')


if __name__ == '__main__':
    main()
