#!/usr/bin/env python3
"""Check the TLS settings and linkage the bounded HTTPS response path relies on.

No host build can observe these. https_response_write_test.cpp runs the
response path itself, over the pinned SDK's send path.
"""
import os
from pathlib import Path

from slice_support import function

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))


def main():
    defaults = (ROOT / 'sdkconfig.defaults').read_text()
    for setting in ('CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y',
                    'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384',
                    'CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096'):
        assert setting in defaults.splitlines(), 'Application bound must preserve full TLS capacities: ' + setting
    # All project-owned response bodies share this helper. SDK-generated small
    # HTTP error responses retain their normal implementation.
    for directory in ('components/web', 'components/web_admin'):
        for source in (ROOT / directory).rglob('*.cpp'):
            if source.name != 'request.cpp':
                assert 'httpd_resp_send(' not in source.read_text(), 'Unbounded application response in ' + str(source)
    assert '"-Wl,--wrap=esp_tls_conn_write"' in (ROOT / 'components/web/CMakeLists.txt').read_text()
    https = (IDF / 'components/esp_https_server/src/https_server.c').read_text()
    assert 'esp_tls_conn_write(tls, buf, buf_len)' in function(https, 'static int httpd_ssl_send(')
    print('PASS HTTPS response linkage: full TLS buffers; every project response through web::response; '
          'HTTPS sends reach the bounded TLS write')


if __name__ == '__main__':
    main()
