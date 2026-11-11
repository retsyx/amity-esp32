"""Fresh firmware log bytes bounded by authenticated same-boot UI captures."""

import re


_SECTION = re.compile(rb"\n===== (Main Amity|Amity)(?:: [^\r\n]+)? =====\n"
                      rb"Device: ([0-9a-f]{32})\nBoot: ([0-9a-f]{32}|unknown)\n\n")


def _main_log_bytes(capture):
    """Extract the authenticated main's ring from the shipping log download."""
    sections = list(_SECTION.finditer(capture.data))
    if (not sections or sections[0].start() != 0 or sections[0][1] != b"Main Amity"
            or sections[0][2].decode() != capture.device_id
            or sections[0][3].decode() != capture.boot_before
            or any(section[1] != b"Amity" for section in sections[1:])
            or len({section[2] for section in sections}) != len(sections)
            or capture.data.count(b"\n===== ") != len(sections)):
        raise AssertionError("CEC policy logs need the exact main device and boot section")
    end = sections[1].start() if len(sections) > 1 else len(capture.data)
    data = capture.data[sections[0].end():end]
    if data == b"[No logs captured.]\n":
        return b""
    if not data or data.startswith(b"[Local logs unavailable:") or b"[Log collection interrupted;" in data:
        raise AssertionError("CEC policy logs need an available complete main capture")
    return data


def fresh_log_delta(before, after):
    """Return only newly appended complete log lines, failing on lost overlap."""
    before.assert_same_boot()
    after.assert_same_boot()
    if (before.device_id != after.device_id or before.boot_after != after.boot_before
            or before.finished_ns > after.started_ns):
        raise AssertionError("CEC policy logs need ordered captures of the same device and boot")
    previous, current = _main_log_bytes(before), _main_log_bytes(after)
    if any(data and not data.endswith(b"\n") for data in (previous, current)):
        raise AssertionError("CEC policy log captures need complete lines")
    overlap = min(len(previous), len(current))
    while overlap and previous[-overlap:] != current[:overlap]:
        overlap -= 1
    # The firmware ring wraps at a byte cursor, so the retained prefix can
    # begin midway through a line. Require a whole final old line inside the
    # byte overlap; a coincidental newline alone is not a freshness witness.
    final_line = previous.rstrip(b"\n").rsplit(b"\n", 1)[-1] + b"\n"
    if previous and overlap < len(final_line):
        raise AssertionError("CEC policy log captures lost their exact suffix/prefix overlap")
    return current[overlap:].decode("utf-8", errors="strict")


def failed_key_attempts(before, after, *, gpio, header, physical_attempts, evidence=None):
    """Cross-check main-owner diagnostics against independently counted NACKs.

    Header-only physical frames cannot identify their opcode. These existing
    firmware diagnostics supply that attribution; they are not opcode-visible
    wire evidence. Reject a retried press even if the total header count fits
    the combined press/release budget.
    """
    text = fresh_log_delta(before, after)
    pattern = re.compile(r"cec: TX failed \(GPIO (\d+), (\d+) bytes: "
                         r"([0-9a-f ]+), result ([A-Za-z]+), attempts (\d+), started (true|false)\)")
    rows = []
    if evidence is not None:
        evidence["failed_transmissions"] = rows
    for line in text.splitlines():
        if "cec: TX failed (GPIO " + str(gpio) + "," not in line:
            continue
        match = pattern.search(line)
        if match is None:
            raise AssertionError("Unrecognized CEC failure diagnostic on the observed wire")
        raw = bytes.fromhex(match[3])
        if not raw or raw[0] != header:
            continue
        if (len(raw) != int(match[2]) or raw not in (bytes([header, 0x44, 4]), bytes([header, 0x45]))
                or match[4] != "Nack" or match[6] != "true"):
            raise AssertionError("The failed BLE hold needs exact NACKed press/release diagnostics")
        rows.append({"frame": raw.hex(":"), "attempts": int(match[5])})
    presses = [row for row in rows if row["frame"] == bytes([header, 0x44, 4]).hex(":")]
    releases = [row for row in rows if row["frame"] == bytes([header, 0x45]).hex(":")]
    if len(presses) != 1 or presses[0]["attempts"] != 1:
        raise AssertionError("A failed BLE key press must have exactly one transmission attempt")
    if len(releases) > 1 or any(not 1 <= row["attempts"] <= 3 for row in releases):
        raise AssertionError("A failed BLE release exceeded its cleanup budget")
    if [row["frame"] for row in rows] != [row["frame"] for row in presses + releases]:
        raise AssertionError("Failed BLE press/release diagnostics are out of order")
    if sum(row["attempts"] for row in rows) != physical_attempts:
        raise AssertionError("CEC failure diagnostics do not match the physical NACK count")
    return rows
