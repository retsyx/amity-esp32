"""Hostname edits and intentional reboots through existing bench controls."""

from copy import deepcopy
import http.client
import re
import time
from urllib.error import URLError
from uuid import uuid4

from .dut import AmityHTTPError, _positive_seconds
from .firmware import saved_configuration, saved_device_settings
from .membership import _identifier
from .trace import event


_READ_ERRORS = (OSError, URLError, http.client.HTTPException, AmityHTTPError)


class HostnameChanges:
    """Restore canonical names after success and preserve failed DUT state.

    Only an explicitly requested reboot can refresh fixture boot fences. A
    rename must preserve every boot. USB performs reset only on headless
    members; settings, state and all verification use the shipping HTTPS UI.
    """

    _cleanup_priority = -1
    _cleanup_retryable = False

    def __init__(self, main, members=(), activities=(), *, timeout=None):
        self.main, self.members, self.activities = main, tuple(members), tuple(activities)
        self.bed = main._testbed
        if self.bed is None or not self.bed._entered or self.bed._closed:
            raise RuntimeError("Change hostnames inside an active TestBed")
        self.owners = (main, *(member.candidate for member in self.members))
        self.ids = {dut.identity.device_id for dut in self.owners}
        if len(self.ids) != len(self.owners) or any(
                member.main is not main or not member._created or member._closed
                or self.bed._member_proofs.get(member.device_id) is not member for member in self.members):
            raise ValueError("Hostname tests require this TestBed's verified members")
        if any(activity.dut is not main or not activity._created or activity._closed for activity in self.activities):
            raise ValueError("Hostname tests require this main's saved activities")
        self.timeout = main._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Hostname timeout")
        self._closed, self._pending, self._rebooting = False, None, None
        self._probe = None
        self.report = {"kind": "hostname", "owner_ids": sorted(self.ids), "checks": []}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self.report)
        self.bed.add_cleanup(self.close)
        deadline = time.monotonic() + self.timeout
        system = self._get("/api/system", deadline)
        self.group = system.get("consortium_id")
        peers = self._identity(system)
        self.boots = {main.identity.device_id: system["boot_id"],
                      **{owner: peer["boot_id"] for owner, peer in peers.items()}}
        self.names = {main.identity.device_id: system["hostname"],
                      **{owner: peer["hostname"] for owner, peer in peers.items()}}
        self.configuration = saved_configuration(self._get("/api/config", deadline))
        self._ready(deadline)
        self.settings = {owner: saved_device_settings(self._settings(owner, deadline, hostname=self.names[owner]))
                         for owner in self.ids}

    def _note(self, check, **values):
        self.report["checks"].append({"check": check, **values})
        event("hostname." + check, dut=self.main.name, **values)

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Hostname verification deadline expired")
        return remaining

    def _pause(self, deadline):
        time.sleep(min(0.25, self._remaining(deadline)))

    def _get(self, path, deadline):
        while True:
            self._remaining(deadline)
            try:
                if self._probe is not None:
                    health = self.main.probe_ready(timeout=min(15, self._remaining(deadline)))
                    if (health.get("device_id") != self.main.identity.device_id
                            or health.get("boot_id") != self.boots[self.main.identity.device_id]
                            or health.get("hostname") != self._probe):
                        raise AssertionError("The new hostname resolved to another device or boot")
                    if health.get("ready") is not True:
                        self._pause(deadline)
                        continue
                    self.main._runtime.admin_origins[self.main.name] = self.main.url
                    self._probe = None
                self.main.authenticate()
                return self.main.request(path, timeout=min(15, self._remaining(deadline)))
            except _READ_ERRORS as error:
                if isinstance(error, AmityHTTPError):
                    if error.status == 401:
                        self.main._token = None
                    elif error.status not in {408, 429, 500, 502, 503, 504}:
                        raise
                self._pause(deadline)

    def _identity(self, system):
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id
                or system.get("consortium_id") != self.group
                or not _identifier(self.group) or not _identifier(system.get("boot_id"))
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("member") is not False):
            raise AssertionError("Hostname test lost the original main or system identity")
        rows = membership.get("peers")
        if (not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows)
                or len({row.get("octopus_id") for row in rows}) != len(rows)):
            raise AssertionError("Hostname test returned an invalid member roster")
        peers = {row.get("octopus_id"): row for row in rows}
        if (set(peers) != self.ids - {self.main.identity.device_id}
                or any(row.get("enrolled") is not True for row in rows)):
            raise AssertionError("Hostname test changed the enrolled member roster")
        return peers

    def _ready(self, deadline, *, rebooted=()):
        """Verify connectivity and persisted state before any fresh scan."""
        rebooted = set(rebooted)
        while True:
            self.bed.session.assert_healthy()
            system = self._get("/api/system", deadline)
            peers = self._identity(system)
            boots = {self.main.identity.device_id: system["boot_id"],
                     **{owner: row.get("boot_id") for owner, row in peers.items()}}
            for owner, boot in boots.items():
                if owner not in rebooted and boot != self.boots[owner]:
                    raise AssertionError("An Amity restarted without a requested reboot")
            ready = (system.get("ready") is True
                     and system.get("membership", {}).get("authority_ready") is True
                     and system.get("configuration_pending") is False
                     and all(row.get("ready") is True and _identifier(row.get("boot_id")) for row in peers.values())
                     and all(_identifier(boots[owner]) and boots[owner] != self.boots[owner] for owner in rebooted))
            if ready:
                observed = {self.main.identity.device_id: system.get("hostname"),
                            **{owner: row.get("hostname") for owner, row in peers.items()}}
                names = system.get("named_devices", [])
                if observed != self.names or any(
                        [row.get("hostname") for row in names if row.get("octopus_id") == owner] != [name]
                        for owner, name in self.names.items()):
                    self._pause(deadline)
                    continue
                config = self._get("/api/config", deadline)
                if saved_configuration(config) != self.configuration:
                    raise AssertionError("Hostname change or reboot changed saved equipment or activities")
                if (system.get("applied_config_revision") != config.get("config_revision")
                        or system.get("applied_presentation_revision") != config.get("presentation_revision")):
                    self._pause(deadline)
                    continue
                for owner, expected in getattr(self, "settings", {}).items():
                    body = self._settings(owner, deadline, hostname=self.names[owner], boot=boots[owner])
                    if saved_device_settings(body) != expected:
                        raise AssertionError("Hostname change or reboot changed saved device settings")
                return system, peers, boots
            self._pause(deadline)

    def _owner(self, dut):
        if not any(dut is owner for owner in self.owners):
            raise ValueError("Hostname operation targets an unrelated DUT")
        return dut.identity.device_id

    def _settings(self, owner, deadline, *, hostname=None, revision=None, boot=None):
        while True:
            response = self._get("/api/devices/" + owner + "/settings", deadline)
            body = response.get("settings") or {}
            if response.get("status") == "ready" and not response.get("error"):
                if body.get("boot_id") != (self.boots[owner] if boot is None else boot):
                    raise AssertionError("Hostname settings belong to another owner boot")
                value = body.get("hostname_revision")
                if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or int(value) <= 0:
                    raise AssertionError("Hostname settings have no valid revision")
                if ((hostname is None or body.get("hostname") == hostname)
                        and (revision is None or value == revision)):
                    return body
            self._pause(deadline)

    def _standby(self, deadline):
        state = self._get("/api/activity-editor/status", deadline)
        if (state.get("expected_boot_id") != self.boots[self.main.identity.device_id]
                or state.get("current_activity") != -1 or state.get("activity_settling") is not False):
            raise AssertionError("Rename and reboot checks must begin in standby")

    def _rebind(self, system, peers):
        for member in self.members:
            member.boot = system["boot_id"]
            member.member_boot = peers[member.device_id]["boot_id"]
            member.name = self.names[member.device_id]
            member._initial_system["boot_id"] = system["boot_id"]
            for row in member._initial_system["membership"]["peers"]:
                row["boot_id"] = peers[row["octopus_id"]]["boot_id"]
                row["hostname"] = self.names[row["octopus_id"]]
        for activity in self.activities:
            activity.boot = system["boot_id"]

    def _finish_rename(self, deadline):
        pending = self._pending
        while True:
            operation = self._get("/api/operations/" + pending["request_id"], deadline)
            if (operation.get("operation_id") != pending["request_id"]
                    or operation.get("boot_id") != self.boots[self.main.identity.device_id]):
                raise AssertionError("Hostname operation changed identity or main boot")
            if operation.get("stage") == "applied" and operation.get("rebooting") is False:
                break
            if operation.get("stage") != "admitted":
                raise AssertionError("Hostname write was not confirmed applied without restart")
            self._pause(deadline)
        body = self._settings(pending["owner"], deadline, hostname=pending["hostname"],
                              revision=str(int(pending["before"]["hostname_revision"]) + 1))
        before = pending["before"]
        after_saved, before_saved = saved_device_settings(body), saved_device_settings(before)
        if (after_saved["settings_revision"] != before_saved["settings_revision"]
                or after_saved["adapters"] != before_saved["adapters"]):
            raise AssertionError("Hostname edit changed adapter settings")
        self.names[pending["owner"]] = pending["hostname"]
        self.settings[pending["owner"]] = after_saved
        system, peers, _ = self._ready(deadline)
        self._rebind(system, peers)
        self._note("renamed", device_id=pending["owner"], hostname=pending["hostname"],
                   request_id=pending["request_id"], main_boot=system["boot_id"])
        self._pending = None

    def rename(self, dut, hostname):
        owner = self._owner(dut)
        if not isinstance(hostname, str) or re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?", hostname) is None:
            raise ValueError("Use a valid Amity hostname")
        deadline = time.monotonic() + self.timeout
        if self._pending is not None:
            self._finish_rename(deadline)
        self._ready(deadline)
        self._standby(deadline)
        before = deepcopy(self._settings(owner, deadline, hostname=self.names[owner]))
        if before["hostname"] == hostname:
            return self.assert_hostname(dut, hostname)
        request_id = uuid4().hex
        self._pending = {"owner": owner, "hostname": hostname, "before": before, "request_id": request_id}
        payload = {"request_id": request_id, "expected_boot_id": before["boot_id"],
                   "expected_hostname_revision": before["hostname_revision"], "hostname": hostname, "changes": []}
        try:
            self.main.request("/api/devices/" + owner + "/settings", payload,
                              timeout=min(15, self._remaining(deadline)))
        except AmityHTTPError as error:
            if error.status < 500:
                self._pending = None
                raise
        except _READ_ERRORS:
            pass  # Reconcile the exact operation; never replay a hostname POST.
        if dut is self.main:
            self.main.reconnect_hostname(hostname)
            self._probe = hostname
        self._finish_rename(deadline)
        return self

    def assert_hostname(self, dut, hostname):
        owner = self._owner(dut)
        if self.names[owner] != hostname:
            raise AssertionError("Unexpected requested hostname")
        deadline = time.monotonic() + self.timeout
        self._ready(deadline)
        self._settings(owner, deadline, hostname=hostname)
        self._note("hostname_verified", device_id=owner, hostname=hostname, owner_boot=self.boots[owner])
        return self

    def canonicalize(self):
        for dut in self.owners:
            self.rename(dut, dut.name)
        return self

    def reboot(self, *duts):
        owners = {self._owner(dut) for dut in duts}
        if not owners or len(owners) != len(duts):
            raise ValueError("Choose distinct reboot targets")
        deadline = time.monotonic() + self.timeout
        self._ready(deadline)
        self._standby(deadline)
        self._rebooting = set()
        # Reset the secondary first so the main remains available to observe it.
        for dut in duts:
            if dut is not self.main:
                self._rebooting.add(dut.identity.device_id)
                dut.usb_reboot()
        if self.main in duts:
            self._rebooting.add(self.main.identity.device_id)
            try:
                result = self.main.request("/api/reboot", {}, timeout=min(15, self._remaining(deadline)))
                if result.get("ok") is not True:
                    raise AssertionError("The main did not acknowledge its restart")
            except _READ_ERRORS as error:
                if isinstance(error, AmityHTTPError):
                    raise
            self.main._token = None
            self.main.close()
        self._finish_reboot(deadline)
        return self

    def _finish_reboot(self, deadline):
        system, peers, boots = self._ready(deadline, rebooted=self._rebooting)
        self.boots = boots
        self._rebind(system, peers)
        self._rebooting = None
        self._note("reboot_verified", main_boot=system["boot_id"], owner_boots=boots)
        # The activity framework requires current physical discovery proof
        # after a boot. Connectivity and hostname persistence passed above.
        fresh = self.main.scan_hdmi(timeout=self._remaining(deadline))
        fresh.assert_contains(*self.bed.components)
        fresh.assert_contains_only(*self.bed.components)
        document = self._get("/api/activity-editor", deadline)
        for activity in self.activities:
            if [row for row in document.get("activities", []) if row.get("activity_id") == activity.activity_id] != [activity._created_row]:
                raise AssertionError("The saved activity changed across reboot")
            activity.scan = fresh
            activity.refresh_scan(fresh)

    def restore(self):
        if not hasattr(self, "names"):
            return
        deadline = time.monotonic() + self.timeout
        if self._rebooting is not None or self._pending is not None:
            raise RuntimeError("Keep the failed hostname operation unchanged for debugging")
        self._standby(deadline)
        for dut in reversed(self.owners):
            self.rename(dut, dut.name)
            self.assert_hostname(dut, dut.name)
        self._note("canonical_names_restored")

    def close(self):
        if not self._closed:
            self.restore()
            self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        if exc is None:
            self.close()
        else:
            self._note("failed_state_retained")
        return False
