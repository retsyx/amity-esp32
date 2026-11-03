"""Temporary adapter settings through the original main's shipping UI API."""

from copy import deepcopy
import time
from uuid import uuid4

from .dut import AmityHTTPError, _positive_seconds
from .membership import MembershipConflict
from .topology import format_physical_address
from .trace import event


def _counter(value):
    return isinstance(value, str) and value.isascii() and value.isdecimal() and int(value) > 0


class FrontPhysicalAddress:
    """Save, verify and restore one test-owned member's adapter default.

    Applying and restoring preserve the verified member boot and scan proofs.
    A saved fallback changes the selected address only while that selection
    originates from the default, independently of matching address numbers.
    """

    _cleanup_priority = 1
    _cleanup_retryable = True

    def __init__(self, member, tentacle, address, *, timeout=None):
        bed = getattr(member.main, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed or not member._created
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
                if type(row.get("selected_from_default")) is not bool:
                    raise ValueError("The selected address needs its actual selection source")
            except (TypeError, ValueError) as error:
                raise MembershipConflict("Adapter settings returned an invalid physical address or selection source") from error
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
                    if (selected["selected_from_default"] != pending["selected_from_default"]
                            or effective != pending["selected_address"]):
                        raise MembershipConflict("The selected address did not follow its reviewed selection source")
                    self.member.assert_connected(timeout=self._remaining(deadline))
                    pending["record"].update(member_boot=peer["boot_id"], settings_revision=body["settings_revision"],
                                              selected_address=effective,
                                              selected_from_default=selected["selected_from_default"])
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
                         "selected_from_default": selected["selected_from_default"],
                         "selected_address": address if selected["selected_from_default"] else
                                             format_physical_address(selected["effective_front_physical_address"]).upper(),
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
            if (selected["selected_from_default"]
                    and selected["effective_front_physical_address"].upper() != self.address):
                raise MembershipConflict("The selected fallback differs from its saved default")
            return self
        self._change(snapshot, selected, self.address, "apply", deadline)
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
