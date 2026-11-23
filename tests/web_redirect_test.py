#!/usr/bin/env python3
"""Check the redirect server's port and the lwIP socket budget, which no host build can observe.

web_redirect_test.cpp runs the redirect server itself.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def config_value(text, name):
    match = re.search(r'^' + re.escape(name) + r'=(\d+)$', text, re.M)
    assert match, name
    return int(match.group(1))


def check_socket_pool():
    # Each HTTPD holds its listener and two control sockets besides sessions.
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    https = int(re.search(r'cfg\.httpd\.max_open_sockets=(\d+);',
                          (ROOT / 'components/web/web.cpp').read_text()).group(1))
    redirect = 1 + int(re.search(r'constexpr size_t client_slots=(\d+);',
                                 (ROOT / 'components/web/redirect.cpp').read_text()).group(1))
    homekit = config_value(defaults, 'CONFIG_HAP_HTTP_MAX_OPEN_SOCKETS')
    limits = (ROOT / 'components/consortium/include/amity/consortium/limits.hpp').read_text()
    peers = int(re.search(r'maximum_roster_peers=(\d+);', limits).group(1))
    pending = int(re.search(r'maximum_pending_connections=(\d+);', limits).group(1))
    needed = (https + 3) + (homekit + 3) + redirect + (1 + peers + pending)
    available = config_value(defaults, 'CONFIG_LWIP_MAX_SOCKETS')
    assert needed <= available, (needed, available)


def main():
    assert 'esp_err_t start_redirect(uint16_t port=80);' in (ROOT / 'components/web/include/web.hpp').read_text()
    check_socket_pool()
    print('PASS HTTP redirect configuration: port 80 by default; every server fits the lwIP socket pool')


if __name__ == '__main__':
    main()
