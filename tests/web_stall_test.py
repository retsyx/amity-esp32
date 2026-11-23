#!/usr/bin/env python3
"""Check the linkage the HTTPS stall deadlines rely on, which no host build can observe.

The pinned SDK must keep calling each wrapped function from another source
file, or the linker would silently bypass the deadline. Only the HTTPS server
may create esp-tls server sessions, since its handshake deadline is global.
web_stall_test.cpp runs the deadlines themselves.
"""
import os
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))


def function_body(source, name):
    start = re.search(r'^[\w ]*\b' + re.escape(name) + r'\(', source, re.M).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def check_sdk():
    http = IDF / 'components/esp_http_server/src'
    private = (http / 'esp_httpd_priv.h').read_text()
    for declaration in ('esp_err_t httpd_req_new(struct httpd_data *hd, struct sock_db *sd);',
                        'esp_err_t httpd_uri(struct httpd_data *hd);',
                        'esp_err_t httpd_req_delete(struct httpd_data *hd);'):
        assert declaration in private, declaration
    assert re.search(r'struct sock_db \{\s*int fd;', private)
    assert re.search(r'struct httpd_req_aux \{[^}]*size_t\s+remaining_len;', private)
    assert re.search(r'struct httpd_data \{[^}]*struct httpd_req hd_req;[^}]*struct httpd_req_aux hd_req_aux;', private)
    sources = {path.name: path.read_text() for path in http.glob('*.c')}
    definitions = {name: [file for file, text in sources.items()
                          if re.search(r'^esp_err_t ' + name + r'\(struct httpd_data', text, re.M)]
                   for name in ('httpd_req_new', 'httpd_uri', 'httpd_req_delete')}
    assert definitions == {'httpd_req_new': ['httpd_parse.c'], 'httpd_uri': ['httpd_uri.c'],
                           'httpd_req_delete': ['httpd_parse.c']}, definitions
    # Each wrapped call crosses a source file.
    process = function_body(sources['httpd_sess.c'], 'httpd_sess_process')
    assert 'httpd_req_new(hd, session)' in process and 'httpd_req_delete(hd)' in process
    assert 'return httpd_uri(hd);' in function_body(sources['httpd_parse.c'], 'httpd_parse_req')
    https = (IDF / 'components/esp_https_server/src/https_server.c').read_text()
    assert 'esp_tls_server_session_create(' in function_body(https, 'httpd_ssl_open')


def check_transfers():
    # Firmware uploads read their own bodies under their own deadline. Every
    # early exit closes without a drain, so no unread upload is purged.
    system = (ROOT / 'components/system_services/system_services.cpp').read_text()
    upload = function_body(system, 'upload_firmware')
    assert 'web::BodyReader reader(req,' in upload and 'web::response(' not in upload and 'web::json(' not in upload
    returns = re.findall(r'return ([^;]*);', upload)
    # The read helper's own true/false results aside, every exit before the
    # final response is a closing error.
    assert returns[-1].startswith('httpd_resp_sendstr(') and all(
        value.startswith(('web::error(', 'fail(')) or value in ('true', 'false') for value in returns[:-1]), returns
    assert re.search(r'auto fail=\[&\]\(const std::string& message\)\{[^}]*return web::error\(', upload)
    error = function_body((ROOT / 'components/web/request.cpp').read_text(), 'error')
    assert error.rstrip().endswith('return ESP_FAIL;\n}') or 'return ESP_FAIL;' in error.splitlines()[-2], error
    # Log downloads stream from their own task, outside the bounded handler.
    logs = (ROOT / 'components/system_services/log_download.cpp').read_text()
    assert 'httpd_req_async_handler_begin(' in logs and 'httpd_resp_send_chunk(' in logs and 'web::response(' not in logs


def check_handshake_owner():
    callers = []
    for directory in ('components', 'main', 'third_party'):
        for path in (ROOT / directory).rglob('*'):
            if path.suffix in ('.c', '.cpp') and 'esp_tls_server_session_create(' in path.read_text(errors='ignore'):
                callers.append(path.relative_to(ROOT).as_posix())
    assert callers == ['components/web/web.cpp'], callers


def main():
    check_sdk()
    check_handshake_owner()
    check_transfers()
    cmake = (ROOT / 'components/web/CMakeLists.txt').read_text()
    for name in ('httpd_req_new', 'httpd_uri', 'httpd_req_delete'):
        assert f'"-Wl,--wrap={name}"' in cmake, name
    print('PASS HTTPS stall linkage: wrapped HTTPD calls cross SDK source files; one handshake owner; '
          'uploads and log downloads keep their own bounds')


if __name__ == '__main__':
    main()
