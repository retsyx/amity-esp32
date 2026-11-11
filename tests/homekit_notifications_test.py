#!/usr/bin/env python3
"""Exercise the production bounded HomeKit event queue and JSON/socket writer."""
from pathlib import Path
import re
import subprocess
import tempfile

from homekit_http_test import function


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    core = root / "third_party/esp-homekit-sdk/components/esp_hap_core/src"
    http = (core / "esp_hap_ip_services.c").read_text()
    chars = (core / "esp_hap_char.c").read_text()
    app = (root / "components/amity_homekit/homekit.cpp").read_text()
    capacity = re.search(r"config.max_event_notif_chars=(\d+);", app)
    assert capacity and int(capacity[1]) == 16
    prefix = re.search(r"static const char hap_notif_prefix\[\] = .*?;", http)
    stream = re.search(r"typedef struct \{\s*int fd;\s*bool ok;\s*\} hap_notif_stream_t;", http)
    assert prefix and stream
    extracted = [f"#define TEST_NOTIF_CAPACITY {capacity[1]}", prefix.group(), stream.group()]
    extracted += [function(chars, name) for name in (
        "hap_event_queue_init", "hap_event_queue_deinit", "hap_get_pending_notif_char",
        "hap_queue_event", "hap_char_is_ctrl_subscribed", "hap_char_is_ctrl_owner")]
    extracted += [function(http, name) for name in (
        "hap_add_char_val_json", "hap_notif_send_all", "hap_notif_header", "hap_notif_flush",
        "hap_notif_encode_char", "hap_notif_send_batch", "hap_notif_send_large",
        "hap_send_notification_unlocked")]
    fixture = (root / "tests/homekit_notifications_fixture.c").read_text()
    generator = root / "managed_components/espressif__json_generator"
    cjson = root / "managed_components/espressif__cjson/cJSON"
    with tempfile.TemporaryDirectory(prefix="amity-homekit-notifications-") as directory:
        source = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        source.write_text(fixture.replace("/* INSERT PRODUCTION FUNCTIONS */", "\n\n".join(extracted)))
        subprocess.run([
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-g", "-fsanitize=address,undefined",
            f"-I{generator / 'include'}", f"-I{cjson}", str(source),
            str(generator / "src/json_generator.c"), str(cjson / "cJSON.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
