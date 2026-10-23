#!/usr/bin/env python3
"""Exercise the production HomeKit HTTP state transitions with host SDK stubs."""
from pathlib import Path
import re
import subprocess
import tempfile


def function(source: str, name: str) -> str:
    start = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*?\)\s*\{", source, re.M)
    if not start:
        raise AssertionError(f"Production function not found: {name}")
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, start.end() - 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start.start():token.end()]
    raise AssertionError(f"Unclosed production function: {name}")


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    core = root / "third_party/esp-homekit-sdk/components/esp_hap_core/src"
    http = (core / "esp_hap_ip_services.c").read_text()
    setup = (core / "esp_hap_pair_setup.c").read_text()
    body_type = re.search(r"typedef struct \{[^}]*\} hap_request_body_t;", http)
    assert body_type
    extracted = [body_type.group()]
    extracted.extend(function(setup, name) for name in (
        "hap_pair_setup_get_received_state", "hap_pair_setup_ctx_clean",
        "hap_pair_setup_context_init", "hap_pair_setup_process"))
    extracted.extend(function(http, name) for name in (
        "hap_http_session_not_authorized", "hap_httpd_get_data",
        "hap_http_pair_setup_handler", "hap_locked_request"))
    fixture = (root / "tests/homekit_http_fixture.c").read_text()
    marker = "/* INSERT PRODUCTION FUNCTIONS */"
    assert fixture.count(marker) == 1
    with tempfile.TemporaryDirectory(prefix="amity-homekit-http-") as directory:
        source = Path(directory) / "homekit_http_test.c"
        executable = Path(directory) / "homekit_http_test"
        source.write_text(fixture.replace(marker, "\n\n".join(extracted)))
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-g",
            "-fsanitize=address,undefined", str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
