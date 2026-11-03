"""Temporary adapter settings through the original main's shipping UI API."""

from copy import deepcopy
import time
from uuid import uuid4

from .config import current_config
from .dut import AmityHTTPError, _positive_seconds
from .membership import AddedAmity, MembershipConflict, _identifier
from .topology import format_physical_address
from .trace import event


def _counter(value):
    return isinstance(value, str) and value.isascii() and value.isdecimal() and int(value) > 0


class _LocalOwner:
    """Use the same settings transaction checks for the original main."""

    _settings_match = staticmethod(AddedAmity._settings_match)

    def __init__(self, main):
        bed = getattr(main, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed
                or not any(dut is main for dut in bed.duts)
                or any(dut is main for dut in getattr(bed, "members", ()))):
            raise RuntimeError("Local adapter settings require this TestBed's original main")
        self.main, self.bed = main, bed
        self.device_id = main.identity.device_id
        self.timeout = current_config().timeout
        self.boot = self.member_boot = self.group = None
        system = self._system()
        self.boot = self.member_boot = system["boot_id"]
        self.group = system["consortium_id"]

    def _system(self):
        if (self.main._testbed is not self.bed or not self.bed._entered or self.bed._closed):
            raise MembershipConflict("The local adapter settings TestBed changed")
        self.bed.session.assert_healthy()
        system = self.main.get_system()
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.device_id or system.get("mode") != "coordinator"
                or membership.get("coordinator") is not True or membership.get("authority_ready") is not True
                or membership.get("member") is not False):
            raise MembershipConflict("The original main no longer has its controller authority")
        if (not _identifier(system.get("boot_id")) or not _identifier(system.get("consortium_id"))
                or self.boot is not None and (system["boot_id"] != self.boot
                                             or system["consortium_id"] != self.group)):
            raise MembershipConflict("The original main restarted or changed system identity")
        return system

    def _connected_snapshot(self, *, deadline):
        system = self._system()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Adapter settings operation deadline expired")
        config = self.main.request("/api/config", timeout=min(15, remaining))
        if (not isinstance(config, dict) or config.get("controller_octopus_id") != self.device_id
                or config.get("consortium_id") != self.group):
            raise MembershipConflict("The original main's saved configuration changed identity")
        boards = [board for board in config.get("octopuses", [])
                  if isinstance(board, dict) and board.get("octopus_id") == self.device_id]
        if (len(boards) != 1 or type(boards[0].get("supported_tentacles")) is not int
                or not 1 <= boards[0]["supported_tentacles"] <= 4):
            raise MembershipConflict("The original main has no proven adapter capacity")
        ready = (system.get("ready") is True and system.get("configuration_pending") is False
                 and system.get("applied_config_revision") == config.get("config_revision")
                 and system.get("applied_presentation_revision") == config.get("presentation_revision"))
        return system, {"boot_id": self.boot, "supported_tentacles": boards[0]["supported_tentacles"]}, ready

    def assert_connected(self, *, timeout):
        deadline = time.monotonic() + timeout
        while True:
            _, peer, ready = self._connected_snapshot(deadline=deadline)
            if ready:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("Adapter settings operation deadline expired")
                settings = self.main.request("/api/devices/" + self.device_id + "/settings",
                                             timeout=min(15, remaining))
                if self._settings_match(settings, peer):
                    self._system()
                    return self
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("The original main did not become ready for adapter settings")
            time.sleep(min(0.25, remaining))


class FrontPhysicalAddress:
    """Save, verify and restore an adapter default on the main or added member.

    Applying and restoring preserve the verified owner boot and scan proofs.
    A save selects the latest stored scanned address when one exists,
    otherwise the new default. Ordinary scans do not change the selection.
    """

    _cleanup_priority = 1
    _cleanup_retryable = True

    def __init__(self, member, tentacle, address, *, timeout=None):
        bed = getattr(member.main, "_testbed", None)
        if not isinstance(member, _LocalOwner) and (bed is None or not bed._entered or bed._closed or not member._created
                or member._closed or member.candidate._testbed is not bed
                or getattr(bed, "_member_proofs", {}).get(member.device_id) is not member):
            raise RuntimeError("Adapter settings require this TestBed's verified added member")
        if type(tentacle) is not int or not 0 <= tentacle < 4:
            raise ValueError("Tentacle must be an integer from zero through three")
        self.member, self.main, self.bed = member, member.main, bed
        self.tentacle = tentacle
        self.address = format_physical_address(address).upper()
        self.timeout = member.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Adapter settings timeout")
        self.path = "/api/devices/" + member.device_id + "/settings"
        self._before = None
        self._pending = None
        self._changed = self._started = self._closed = False
        if not hasattr(self.main, "adapter_settings_operations"):
            self.main.adapter_settings_operations = []
        bed.add_cleanup(self.close)

    @classmethod
    def local(cls, main, tentacle, address, *, timeout=None):
        return cls(_LocalOwner(main), tentacle, address, timeout=timeout)

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Adapter settings operation deadline expired")
        return remaining

    def _request(self, path, data=None, *, deadline):
        return self.main.request(path, data, timeout=min(15, self._remaining(deadline)))

    def _pause(self, deadline):
        time.sleep(min(0.25, self._remaining(deadline)))

    def _idle(self):
        state = self.main.get_state()
        if (state.get("expected_boot_id") != self.member.boot
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False):
            raise MembershipConflict("Adapter settings require the original main to be idle on its proven boot")

    def _snapshot(self, deadline):
        while True:
            self.member._system()
            result = self._request(self.path, deadline=deadline)
            if result.get("status") == "ready":
                body = result.get("settings")
                if (not isinstance(body, dict) or body.get("boot_id") != self.member.member_boot
                        or not _counter(body.get("settings_revision"))):
                    raise MembershipConflict("Adapter settings do not match the proven member boot/revision")
                snapshot, selected = self._validate_snapshot(body)
                if selected["effective_front_physical_address"] is not None:
                    return snapshot, selected
                self._pause(deadline)
                continue
            if result.get("status") not in {"reading", "offline", "error"}:
                raise MembershipConflict("The member cannot edit adapter settings")
            self._pause(deadline)

    def _validate_snapshot(self, body, *, require_configured=True):
        adapters = body.get("adapters")
        if (not isinstance(adapters, list) or any(not isinstance(row, dict) for row in adapters)
                or any(type(row.get("tentacle_id")) is not int for row in adapters)
                or len({row["tentacle_id"] for row in adapters}) != len(adapters)):
            raise MembershipConflict("Adapter settings returned invalid adapter identities")
        selected = [row for row in adapters if row["tentacle_id"] == self.tentacle]
        if len(selected) != 1 or require_configured and selected[0].get("configured") is not True:
            row = selected[0] if len(selected) == 1 else {}
            record = {"phase": "review_failed", "dut": self.main.name, "device_id": self.member.device_id,
                      "tentacle_id": self.tentacle, "member_boot": self.member.member_boot,
                      "address": self.address, "stage": "not_configured",
                      "configured": row.get("configured") if type(row.get("configured")) is bool else None}
            self.main.adapter_settings_operations.append(record)
            event("adapter_settings.not_configured", **record)
            raise MembershipConflict("The reviewed adapter has no configured CEC wires")
        for row in adapters:
            try:
                if not isinstance(row.get("default_front_physical_address"), str):
                    raise ValueError("A UI physical address must be text")
                format_physical_address(row.get("default_front_physical_address"))
                for name in ("observed_front_physical_address", "effective_front_physical_address"):
                    value = row.get(name)
                    if value is not None:
                        if not isinstance(value, str):
                            raise ValueError("A UI physical address must be text")
                        format_physical_address(value)
            except (TypeError, ValueError) as error:
                raise MembershipConflict("Adapter settings returned an invalid physical address") from error
        return deepcopy(body), deepcopy(selected[0])

    @staticmethod
    def _defaults(snapshot):
        return {row["tentacle_id"]: row["default_front_physical_address"].upper()
                for row in snapshot["adapters"]}

    def _operation(self, operation, pending, deadline):
        while True:
            if operation is None:
                operation = self._request("/api/operations/" + pending["request_id"], deadline=deadline)
            if (not isinstance(operation, dict) or operation.get("operation_id") != pending["request_id"]
                    or operation.get("boot_id") != self.member.boot):
                raise MembershipConflict("The adapter settings operation changed identity or main boot")
            pending["record"]["stage"] = operation.get("stage")
            if operation.get("stage") == "applied" and operation.get("rebooting") is False:
                return
            if operation.get("stage") != "admitted":
                raise MembershipConflict("The adapter settings write was not confirmed applied without restart")
            self._pause(deadline)
            operation = None

    def _finish(self, deadline):
        pending = self._pending
        self._operation(pending.get("operation"), pending, deadline)
        while True:
            _, peer, ready = self.member._connected_snapshot(deadline=deadline)
            if peer.get("boot_id") != pending["owner_boot"]:
                raise MembershipConflict("The member restarted during the live adapter settings change")
            if ready:
                result = self._request(self.path, deadline=deadline)
                if self.member._settings_match(result, peer):
                    body, selected = self._validate_snapshot(result["settings"], require_configured=False)
                    expected = dict(pending["defaults"])
                    expected[self.tentacle] = pending["address"]
                    if (self._defaults(body) != expected or not _counter(body.get("settings_revision"))
                            or int(body["settings_revision"]) <= int(pending["revision"])):
                        raise MembershipConflict("The member did not apply exactly the reviewed adapter address")
                    effective = selected.get("effective_front_physical_address")
                    if selected.get("configured") is not True or effective is None:
                        self._pause(deadline)
                        continue
                    effective = format_physical_address(effective).upper()
                    if effective != pending["selected_address"]:
                        raise MembershipConflict("The selected address did not follow the reviewed scanned address or default")
                    self.member.assert_connected(timeout=self._remaining(deadline))
                    pending["record"].update(member_boot=peer["boot_id"], settings_revision=body["settings_revision"],
                                              selected_address=effective)
                    self._changed = pending["phase"] != "restore"
                    self._pending = None
                    event("adapter_settings.verified", **pending["record"])
                    return
            self._pause(deadline)

    def _change(self, snapshot, selected, address, phase, deadline):
        request_id = uuid4().hex
        record = {"phase": phase, "dut": self.main.name, "device_id": self.member.device_id,
                  "tentacle_id": self.tentacle, "request_id": request_id,
                  "owner_boot": self.member.member_boot, "address": address,
                  "previous_address": selected["default_front_physical_address"]}
        self.main.adapter_settings_operations.append(record)
        self._pending = {"phase": phase, "request_id": request_id, "owner_boot": self.member.member_boot,
                         "address": address, "defaults": self._defaults(snapshot),
                         "selected_address": format_physical_address(
                             selected["observed_front_physical_address"]).upper()
                             if selected.get("observed_front_physical_address") is not None else address,
                         "revision": snapshot["settings_revision"], "record": record}
        payload = {"request_id": request_id, "expected_boot_id": snapshot["boot_id"],
                   "expected_settings_revision": snapshot["settings_revision"],
                   "changes": [{"tentacle_id": self.tentacle,
                                "default_front_physical_address": address}]}
        event("adapter_settings.submit", **record)
        try:
            self._pending["operation"] = self._request(self.path, payload, deadline=deadline)
        except AmityHTTPError as error:
            if error.status < 500:
                self._pending = None
            record["error_type"] = type(error).__name__
            raise
        self._finish(deadline)

    def apply(self):
        if self._started:
            raise RuntimeError("An adapter settings context cannot be reused")
        self._started = True
        deadline = time.monotonic() + self.timeout
        self.member.assert_connected(timeout=self._remaining(deadline))
        self._idle()
        snapshot, selected = self._snapshot(deadline)
        self._before = selected["default_front_physical_address"].upper()
        if self._before == self.address:
            return self
        self._change(snapshot, selected, self.address, "apply", deadline)
        return self

    def read(self):
        """Read this adapter's current UI values on the same proven owner boot."""
        deadline = time.monotonic() + self.timeout
        self.member.assert_connected(timeout=self._remaining(deadline))
        _, selected = self._snapshot(deadline)
        return selected

    def assert_address(self, *, detected, selected, default=None):
        """Check the saved, scanned and selected addresses independently."""
        row = self.read()
        expected = {"default_front_physical_address": self.address if default is None else default,
                    "observed_front_physical_address": detected,
                    "effective_front_physical_address": selected}
        for key, value in expected.items():
            wanted = None if value is None else format_physical_address(value).upper()
            actual = row[key]
            actual = None if actual is None else format_physical_address(actual).upper()
            if actual != wanted:
                raise AssertionError(f"Adapter {self.tentacle} {key}: expected {wanted}, got {actual}")
        record = {"phase": "assert_address", "dut": self.main.name, "device_id": self.member.device_id,
                  "owner_boot": self.member.member_boot, "tentacle_id": self.tentacle,
                  "address": row["default_front_physical_address"].upper(),
                  "detected_address": row["observed_front_physical_address"].upper()
                  if row["observed_front_physical_address"] is not None else None,
                  "selected_address": row["effective_front_physical_address"].upper()}
        self.main.adapter_settings_operations.append(record)
        event("adapter_settings.address", **record)
        return self

    def close(self):
        if self._closed:
            return
        deadline = time.monotonic() + self.timeout
        if self._pending is not None:
            self._finish(deadline)
        if self._changed:
            self.member.assert_connected(timeout=self._remaining(deadline))
            self._idle()
            snapshot, selected = self._snapshot(deadline)
            if selected["default_front_physical_address"].upper() != self.address:
                raise MembershipConflict("The adapter settings changed before fixture cleanup")
            self._change(snapshot, selected, self._before, "restore", deadline)
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            if exc is None:
                raise
            event("adapter_settings.cleanup_deferred", device_id=self.member.device_id,
                  error_type=type(cleanup_error).__name__, primary_error_type=type(exc).__name__)
            note = getattr(exc, "add_note", None)
            if callable(note):
                note("Adapter settings cleanup failed; the registered TestBed cleanup will retry it.")
        return False
