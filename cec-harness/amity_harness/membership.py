"""Add/remove a test-owned Amity through the shipping main-device UI.

The candidate is control-only or a headless member with CEC components,
never an HTTP client. Remove revokes it and schedules its factory reset.
No setup codes, bearer tokens or opaque discovery tokens enter reports.
"""

from copy import deepcopy
import logging
import re
import time
from uuid import uuid4

from .discovery import _identifier
from .dut import AmityHTTPError, _positive_seconds
from .trace import event


class MembershipConflict(RuntimeError):
    pass


class MembershipCleanupError(RuntimeError):
    pass


class AddedAmity:
    _cleanup_priority = 2
    _cleanup_retryable = True

    def __init__(self, main, candidate, *, name=None, timeout=None, setup_code=None):
        bed = getattr(main, "_testbed", None)
        if (bed is None or not bed._entered or bed._closed or candidate._testbed is not bed
                or candidate not in (*bed.control_only, *getattr(bed, "members", ()))
                or main in (*bed.control_only, *getattr(bed, "members", ()))):
            raise RuntimeError("Add Amity needs an active main and an explicit headless candidate in the same TestBed")
        if main is candidate or main.identity.device_id == candidate.identity.device_id:
            raise ValueError("An Amity cannot add itself")
        name = candidate.name if name is None else name
        if not isinstance(name, str):
            raise ValueError("An added Amity needs a hostname")
        name = name.strip().lower()
        if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?", name):
            raise ValueError("An Amity name needs 1-32 letters, digits or hyphens")
        if setup_code is not None and (not isinstance(setup_code, str) or not setup_code or len(setup_code) > 256):
            raise ValueError("Use the candidate's existing printed Matter setup code")
        self.main, self.candidate, self.name, self.session = main, candidate, name, bed.session
        self.timeout = main._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Membership timeout")
        self.device_id = candidate.identity.device_id
        self.scan_id, self.request_id = uuid4().hex, uuid4().hex
        self._setup_code = setup_code
        self._baseline = self._initial_system = None
        self.boot = self.group = self.member_boot = None
        self._scan_attempted = self._add_attempted = self._created = self._closed = False
        self._add_rejected = self._remove_attempted = False
        self._add_operation = self._remove_operation = None
        self._remove_id = None
        self._admission_phase = None
        self._cleanup_deferred = False
        self._record("prepare")
        bed.add_cleanup(self.close)

    def _record(self, phase, **values):
        record = {"phase": phase, "candidate": self.candidate.name,
                  "device_id": self.device_id, "name": self.name, **values}
        self.main.membership_operations.append(record)
        self._trace = record
        self._log("membership.phase", phase=phase)
        return record

    def _log(self, name, *, level=logging.INFO, **fields):
        event(name, level=level, dut=self.main.name, device_id=self.main.identity.device_id,
              boot=self.boot, candidate=self.candidate.name, candidate_id=self.device_id,
              assigned_name=self.name, member_boot=self.member_boot,
              request_id=self.request_id, scan_id=self.scan_id, **fields)

    def _step(self, record, step, **values):
        record.update(step=step, **values)
        self._log("membership.step", phase=record["phase"], step=step)

    def _observe_http(self, path, call, *, method="GET"):
        request = {"method": method, "path": path, "started_ns": time.monotonic_ns()}
        self._trace.setdefault("requests", []).append(request)
        try:
            response = call()
            if path == "/api/membership/admission" and isinstance(response, dict):
                phase = response.get("phase")
                if isinstance(phase, str) and phase in {"idle", "scanning", "found", "connecting", "finishing", "complete", "failed", "cancelled"}:
                    request["admission_phase"] = phase
                    if phase != self._admission_phase:
                        self._admission_phase = phase
                        self._log("membership.admission", phase=phase)
            return response
        except BaseException as error:
            request["error_type"] = type(error).__name__
            status = getattr(error, "status", None)
            if type(status) is int and 100 <= status <= 599:
                request["http_status"] = status
            raise
        finally:
            request["finished_ns"] = time.monotonic_ns()
            request["duration_ms"] = (request["finished_ns"] - request["started_ns"]) / 1_000_000

    @staticmethod
    def _active(system):
        peers = system.get("membership", {}).get("peers")
        if not isinstance(peers, list) or any(not isinstance(p, dict) or not _identifier(p.get("octopus_id")) for p in peers):
            raise MembershipConflict("The main returned an invalid membership snapshot")
        result = {p["octopus_id"]: p for p in peers if p.get("enrolled") is True or p.get("provisional") is True}
        if len({p["octopus_id"] for p in peers}) != len(peers):
            raise MembershipConflict("Membership identities are duplicated")
        return result

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Amity membership operation deadline expired")
        return remaining

    def _request(self, path, data=None, *, deadline):
        return self._observe_http(path, lambda: self.main.request(path, data, timeout=min(15, self._remaining(deadline))),
                                  method="GET" if data is None else "POST")

    def _system(self, *, check_health=True):
        if check_health:
            self.session.assert_healthy()
        system = self._observe_http("/api/system", self.main.get_system)
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id or system.get("mode") != "coordinator"
                or membership.get("coordinator") is not True or membership.get("authority_ready") is not True
                or membership.get("member") is not False):
            raise MembershipConflict("The original main no longer has its controller authority")
        if self.boot is not None and (system.get("boot_id") != self.boot or system.get("consortium_id") != self.group):
            raise MembershipConflict("The original main restarted or changed system identity")
        if not _identifier(system.get("boot_id")) or not _identifier(system.get("consortium_id")):
            raise MembershipConflict("The main has no proven boot/system identity")
        return system

    @staticmethod
    def _owned_components(config, owners):
        return {component.get("component_id") for component in config.get("components", [])
                if any(name.get("octopus_id") in owners for name in component.get("qualified_names", []))}

    @classmethod
    def _affected_activities(cls, config, owners):
        components = cls._owned_components(config, owners)
        return [activity.get("activity_id") for activity in config.get("activities", [])
                if any(activity.get(role) in components for role in ("display", "source", "audio", "switch_device"))]

    def _removed_owners(self):
        """Verified removals in this bed may change a nested saved baseline.

        An unconfirmed physical reset still fails that resource's cleanup, but
        a proven saved-inventory deletion must not block independent members.
        """
        bed = self.main._testbed
        candidates = {dut.identity.device_id for dut in
                      (*bed.control_only, *getattr(bed, "members", ()))}
        start = getattr(bed, "_initial_membership_counts", {}).get(id(self.main), 0)
        return {row.get("device_id") for row in self.main.membership_operations[start:]
                if row.get("phase") == "cleanup" and row.get("device_id") in candidates
                and row.get("operation_stage") == "complete"
                and (row.get("inventory_removed") is True or row.get("reset") == "scheduled"
                     and row.get("retained_member") is False and not row.get("error_type"))}

    def _config(self, *, deadline, removed=False):
        config = self._request("/api/config", deadline=deadline)
        if (not isinstance(config, dict) or config.get("controller_octopus_id") != self.main.identity.device_id
                or config.get("consortium_id") != self.group
                or any(not isinstance(config.get(k), list) for k in ("activities", "components", "octopuses"))):
            raise MembershipConflict("The main returned an invalid saved configuration")
        if self._baseline is not None:
            removed_owners = self._removed_owners() | ({self.device_id} if removed else set())
            removed_components = self._owned_components(self._baseline, removed_owners)
            for key, value in self._baseline.items():
                if key == "external_slots" and self._owned_retired_slots(value, config.get(key)):
                    continue
                if key == "components":
                    value = [row for row in value if row.get("component_id") not in removed_components]
                if key not in {"config_revision", "presentation_revision", "octopuses", "adapter_settings"} and config.get(key) != value:
                    raise MembershipConflict("Existing saved activity/equipment configuration changed")
            for key in ("octopuses", "adapter_settings"):
                original = [row for row in self._baseline.get(key, [])
                            if row.get("octopus_id") not in removed_owners]
                current = config.get(key, [])
                if (not isinstance(current, list)
                        or any(row.get("octopus_id") in removed_owners for row in current)
                        or [row for row in current if row in original] != original
                        or any(row not in original and row.get("octopus_id") != self.device_id for row in current)):
                    raise MembershipConflict("Existing Amity settings changed")
            if self._owned_components(config, removed_owners) or self._affected_activities(config, removed_owners):
                raise MembershipConflict("Removed Amity equipment remains in saved configuration")
        return config

    def _owned_retired_slots(self, before, after):
        """Allow only completed fixture activities' newly retired home slots."""
        if not isinstance(before, list) or not isinstance(after, list):
            return False
        if after == before:
            return True
        if after[:len(before)] != before:
            return False
        bed = self.main._testbed
        start = getattr(bed, "_initial_activity_counts", {}).get(id(self.main), 0)
        operations = self.main.activity_operations[start:]
        created = {op.get("activity_id") for op in operations if op.get("phase") == "create"
                   and op.get("operation", {}).get("stage") == "applied"}
        removed = {op.get("activity_id") for op in operations if op.get("phase") == "cleanup"
                   and op.get("removed_activity") is True and not op.get("retained_equipment")}
        owned = {identity for identity in created & removed if _identifier(identity)}
        extras = after[len(before):]
        next_slot = max((slot.get("slot", -1) for slot in before), default=-1) + 1
        return (len({slot.get("slot") for slot in after if isinstance(slot, dict)}) == len(after)
                and len({slot.get("activity_id") for slot in extras if isinstance(slot, dict)}) == len(extras)
                and all(isinstance(slot, dict) and set(slot) == {"slot", "activity_id", "tombstone", "hidden", "alias"}
                        and type(slot["slot"]) is int and slot["slot"] == next_slot + index and slot["slot"] < 16
                        and slot.get("activity_id") in owned
                        and slot.get("tombstone") is True and slot.get("hidden") is False
                        and slot.get("alias") == "" for index, slot in enumerate(extras)))

    def _preserved_members(self, system, *, added):
        active = self._active(system)
        removed = self._removed_owners()
        previous = {identity: peer for identity, peer in self._active(self._initial_system).items()
                    if identity not in removed}
        if set(active) != set(previous) | ({self.device_id} if added else set()):
            raise MembershipConflict("The set of added Amity devices changed unexpectedly")
        for identity, old in previous.items():
            current = active[identity]
            if any(current.get(k) != old.get(k) for k in ("hostname", "boot_id", "enrolled")):
                raise MembershipConflict("A pre-existing Amity changed during the test")
        return active

    def _admission(self, *, deadline):
        state = self._request("/api/membership/admission", deadline=deadline)
        if not isinstance(state, dict) or state.get("scan_id") != self.scan_id:
            raise MembershipConflict("The Add Amity search changed")
        actual = state.get("candidate_id")
        if actual is not None and actual != self.device_id:
            raise MembershipConflict("The selected candidate has a different full device identity")
        return state

    def _wait_operation(self, operation, request_id, *, deadline, record):
        previous_stage = object()
        while True:
            if (not isinstance(operation, dict) or operation.get("operation_id") != request_id
                    or operation.get("boot_id") != self.boot):
                raise MembershipConflict("The membership operation identity or main boot changed")
            stage = operation.get("stage")
            if request_id == self.request_id:
                self._add_operation = operation
            if previous_stage != stage:
                self._log("membership.operation", operation_id=request_id, stage=stage)
                previous_stage = stage
            record["operation_stage"] = stage
            if stage == "complete":
                return operation
            if stage not in {"admitted", "preparing", "executing", "configuring", "activating", "stopping"}:
                raise MembershipConflict("The membership operation did not complete successfully")
            time.sleep(min(0.4, self._remaining(deadline)))
            operation = self._request("/api/operations/" + request_id, deadline=deadline)

    def create(self):
        if self._created:
            return self
        if self._scan_attempted or self._closed:
            raise RuntimeError("An Add Amity attempt cannot be repeated")
        deadline = time.monotonic() + self.timeout
        record = self._record("add", request_id=self.request_id, scan_id=self.scan_id)
        self._step(record, "preflight")
        try:
            system = self._system()
            self.boot, self.group = system["boot_id"], system["consortium_id"]
            record["boot"] = self.boot
            self._initial_system = deepcopy(system)
            active = self._active(system)
            if self.device_id in active:
                raise MembershipConflict("The candidate is already an added Amity")
            names = system.get("named_devices", [])
            if (system.get("hostname") == self.name or any(p.get("hostname") == self.name for p in active.values())
                    or any(n.get("hostname") == self.name and n.get("octopus_id") != self.device_id for n in names)):
                raise MembershipConflict("The requested Amity name is already assigned")
            capacity = system.get("capabilities", {}).get("supported_octopuses")
            if type(capacity) is not int or len(active) + 1 >= capacity:
                raise MembershipConflict("The main has no capacity for another Amity")
            if system.get("configuration_pending") is not False:
                raise MembershipConflict("The main is still applying configuration")
            state = self._observe_http("/api/activity-editor/status", self.main.get_state)
            if state.get("expected_boot_id") != self.boot or state.get("current_activity") != -1 or state.get("activity_settling") is not False:
                raise MembershipConflict("Adding an Amity in this test requires the main to be idle")
            current = self._request("/api/membership/admission", deadline=deadline)
            if current.get("phase") not in {"idle", "complete", "cancelled", "failed"}:
                raise MembershipConflict("Another Add Amity search is in progress")
            self._baseline = deepcopy(self._config(deadline=deadline))
            if self._affected_activities(self._baseline, {self.device_id}):
                raise MembershipConflict("This test cannot own a candidate referenced by existing saved activities")
            self._scan_attempted = True
            self._step(record, "start_scan")
            scan = self._request("/api/membership/scan", {"request_id": self.scan_id}, deadline=deadline)
            self._wait_operation(scan, self.scan_id, deadline=deadline, record=record)
            self._step(record, "open_window", reset_gpio=self.candidate.wiring.reset_gpio(self.candidate.name), hold_seconds=3.5)
            self._log("membership.window.start", reset_gpio=record["reset_gpio"], hold_seconds=3.5)
            self.candidate.press_fw_reset(3.5)
            record["released"] = True
            self._log("membership.window.complete", hold_seconds=3.5)
            self._remaining(deadline)
            label = "Amity " + self.device_id[-6:]
            self._step(record, "discover_candidate")
            while True:
                found = self._admission(deadline=deadline)
                if found.get("phase") in {"failed", "cancelled"}:
                    raise MembershipConflict("The Add Amity search ended without a candidate")
                if found.get("phase") == "found":
                    matches = [c for c in found.get("candidates", []) if isinstance(c, dict) and c.get("label") == label]
                    if len(matches) != 1 or not isinstance(matches[0].get("token"), str) or not matches[0]["token"]:
                        raise MembershipConflict("The documented candidate is missing or ambiguous in the Add search")
                    break
                time.sleep(min(0.4, self._remaining(deadline)))
            self._step(record, "confirm_main")
            self._system()
            required = found.get("requires_setup_code")
            if type(required) is not bool or required and not self._setup_code:
                raise MembershipConflict("This candidate requires its existing printed Matter setup code")
            payload = {"request_id": self.request_id, "scan_id": self.scan_id,
                       "candidate_token": matches[0]["token"], "name": self.name}
            if required:
                payload["setup_code"] = self._setup_code
            self._add_attempted = True
            self._step(record, "submit_add")
            try:
                self._add_operation = self._request("/api/membership/add", payload, deadline=deadline)
            except AmityHTTPError as error:
                self._add_rejected = error.status < 500
                raise
            finally:
                self._setup_code = None
                payload.clear()
            self._finish_add(deadline=deadline, record=record)
            result = self.assert_connected(timeout=self._remaining(deadline))
            self._log("membership.add.complete")
            return result
        except BaseException as error:
            self._setup_code = None
            record["error_type"] = type(error).__name__
            self._log("membership.add.failed", level=logging.ERROR, step=record.get("step"),
                      error_type=type(error).__name__)
            raise

    def _finish_add(self, *, deadline, record):
        self._step(record, "confirm_add")
        operation = self._add_operation
        if operation is None:
            operation = self._request("/api/operations/" + self.request_id, deadline=deadline)
        self._add_operation = self._wait_operation(operation, self.request_id, deadline=deadline, record=record)
        state = self._admission(deadline=deadline)
        if state.get("candidate_id") != self.device_id or state.get("phase") != "complete":
            raise MembershipConflict("The addition did not confirm the documented candidate identity")
        self._created = True
        record.update(candidate_id=self.device_id, boot=self.boot)

    def _connected_snapshot(self, *, deadline):
        system = self._system()
        peer = self._preserved_members(system, added=True)[self.device_id]
        if peer.get("hostname") != self.name:
            raise MembershipConflict("The added Amity did not retain its requested name")
        config = self._config(deadline=deadline)
        boards = [b for b in config["octopuses"] if b.get("octopus_id") == self.device_id]
        names = [n for n in system.get("named_devices", []) if n.get("octopus_id") == self.device_id]
        ready = (peer.get("enrolled") is True and peer.get("ready") is True and _identifier(peer.get("boot_id"))
                 and len(names) == 1 and names[0].get("hostname") == self.name and len(boards) == 1
                 and type(peer.get("supported_tentacles")) is int and peer["supported_tentacles"] > 0
                 and type(boards[0].get("supported_tentacles")) is int
                 and boards[0].get("supported_tentacles") == peer["supported_tentacles"]
                 and system.get("configuration_pending") is False
                 and system.get("applied_config_revision") == config.get("config_revision")
                 and system.get("applied_presentation_revision") == config.get("presentation_revision"))
        return system, peer, ready

    @staticmethod
    def _settings_match(settings, peer):
        body = settings.get("settings") or {}
        adapters = body.get("adapters", [])
        return (settings.get("status") == "ready" and settings.get("error") == ""
                and body.get("boot_id") == peer["boot_id"] and isinstance(adapters, list)
                and all(isinstance(a, dict) and type(a.get("tentacle_id")) is int for a in adapters)
                and sorted(a.get("tentacle_id") for a in adapters) == list(range(peer["supported_tentacles"])))

    def assert_connected(self, *, timeout=None):
        if not self._created or self._closed:
            raise RuntimeError("This test has no added Amity to verify")
        timeout = self.timeout if timeout is None else timeout
        _positive_seconds(timeout, "Membership verification timeout")
        deadline = time.monotonic() + timeout
        record = self._record("assert_connected", boot=self.boot)
        try:
            while True:
                _, peer, ready = self._connected_snapshot(deadline=deadline)
                if ready:
                    if self.member_boot is not None and peer["boot_id"] != self.member_boot:
                        raise MembershipConflict("The added Amity restarted during verification")
                    # Poll this shipping endpoint directly: its two-second
                    # cache can expire while unrelated TLS requests run.
                    while True:
                        settings = self._request("/api/devices/" + self.device_id + "/settings", deadline=deadline)
                        if self._settings_match(settings, peer):
                            system, confirmed, current = self._connected_snapshot(deadline=deadline)
                            if confirmed.get("boot_id") != peer["boot_id"]:
                                raise MembershipConflict("The added Amity restarted during verification")
                            if current and self._settings_match(settings, confirmed):
                                self.member_boot = confirmed["boot_id"]
                                bed = self.main._testbed
                                if not hasattr(bed, "_member_proofs"):
                                    bed._member_proofs = {}
                                bed._member_proofs[self.device_id] = self
                                record.update(member_boot=self.member_boot,
                                              membership_revision=system["membership"].get("membership_revision"))
                                self._log("membership.assert_connected.passed",
                                          membership_revision=record["membership_revision"])
                                return self
                            break
                        time.sleep(min(0.25, self._remaining(deadline)))
                time.sleep(min(0.25, self._remaining(deadline)))
        except BaseException as error:
            record["error_type"] = type(error).__name__
            self._log("membership.assert_connected.failed", level=logging.ERROR,
                      error_type=type(error).__name__)
            raise

    def front_physical_address(self, tentacle, address, *, timeout=None):
        """Temporarily change a configured adapter through the main's UI settings.

        Both applying and restoring restart this added member and prove its
        new boot before returning. Close activities before restoring settings.
        """
        from .settings import FrontPhysicalAddress
        return FrontPhysicalAddress(self, tentacle, address, timeout=timeout).apply()

    def close(self):
        if self._closed:
            return
        self._cleanup_deferred = False
        bed = self.main._testbed
        pending = list(getattr(bed, "_activities", ()))
        pending.extend(owner for callback in getattr(bed, "_cleanups", ())
                       if (owner := getattr(callback, "__self__", None)) is not None
                       and getattr(owner, "_cleanup_priority", 2) < self._cleanup_priority)
        if (not getattr(bed, "_closing", False) and self._created
                and any(getattr(resource, "dut", getattr(resource, "main", None)) is self.main
                        and not resource._closed for resource in pending)):
            self._cleanup_deferred = True
            self._log("membership.cleanup.waiting_for_resources")
            return
        proofs = getattr(self.main._testbed, "_member_proofs", {})
        if proofs.get(self.device_id) is self:
            del proofs[self.device_id]
        deadline = time.monotonic() + self.timeout
        record = self._record("cleanup", retained_member=False)
        try:
            if self._add_attempted and not self._add_rejected:
                if not self._created:
                    try:
                        self._finish_add(deadline=deadline, record=record)
                    except MembershipConflict:
                        operation = self._add_operation or {}
                        state = self._admission(deadline=deadline)
                        if (operation.get("operation_id") == self.request_id and operation.get("boot_id") == self.boot
                                and operation.get("stage") in {"failed", "rejected", "cancelled"}
                                and state.get("phase") in {"failed", "cancelled"}):
                            self._preserved_members(self._system(check_health=False), added=False)
                            self._config(deadline=deadline)
                            self._closed = True
                            self._log("membership.cleanup.complete", retained_member=False,
                                      add_completed=False)
                            return
                        raise
                for review_attempt in range(3):
                    if self._remove_attempted:
                        break
                    system = self._system(check_health=False)
                    active = self._preserved_members(system, added=True)
                    peer = active[self.device_id]
                    if not _identifier(peer.get("boot_id")):
                        raise MembershipConflict("The added Amity has no reviewed boot for removal")
                    if self.member_boot is not None and peer["boot_id"] != self.member_boot:
                        raise MembershipConflict("The added Amity restarted before removal review")
                    config = self._config(deadline=deadline)
                    affected = self._affected_activities(config, {self.device_id})
                    if affected:
                        raise MembershipConflict("Test-owned activities must be cleaned up before removing their Amity")
                    self._remove_id = uuid4().hex
                    payload = {"request_id": self._remove_id, "octopus_id": self.device_id, "confirm": True,
                               "expected_membership_revision": system["membership"]["membership_revision"],
                               "expected_target_boot_id": peer["boot_id"],
                               "expected_config_revision": config["config_revision"],
                               "expected_presentation_revision": config["presentation_revision"],
                               "affected_activity_ids": affected}
                    self._remove_attempted = True
                    self._log("membership.remove.submit", operation_id=self._remove_id,
                              target_boot=peer["boot_id"],
                              membership_revision=system["membership"]["membership_revision"])
                    try:
                        self._remove_operation = self._request("/api/membership/remove", payload, deadline=deadline)
                    except AmityHTTPError as error:
                        if error.status != 409:
                            raise
                        # The server explicitly rejected this review before
                        # admission. A fresh review gets a new request identity;
                        # boot/configuration/affected-activity fences above must
                        # still pass. Unknown outcomes keep the original ID and
                        # are reconciled below, never submitted a second time.
                        self._remove_attempted = False
                        self._remove_operation = None
                        self._remove_id = None
                        self._log("membership.remove.review_rejected", attempt=review_attempt + 1, http_status=error.status)
                        if review_attempt == 2:
                            raise
                        time.sleep(min(0.25, self._remaining(deadline)))
                        continue
                    break
                if self._remove_operation is None:
                    self._remove_operation = self._request("/api/operations/" + self._remove_id, deadline=deadline)
                operation = self._wait_operation(self._remove_operation, self._remove_id, deadline=deadline, record=record)
                record["reset"] = operation.get("reset")
                while True:
                    system = self._system(check_health=False)
                    if self.device_id not in self._active(system):
                        self._preserved_members(system, added=False)
                        if any(row.get("octopus_id") == self.device_id for row in system.get("named_devices", [])):
                            raise MembershipConflict("The removed Amity remains in the device inventory")
                        self._config(deadline=deadline, removed=True)
                        record.update(inventory_removed=True, retained_member=False)
                        break
                    time.sleep(min(0.25, self._remaining(deadline)))
                if operation.get("reset") != "scheduled":
                    raise MembershipConflict("The removed Amity did not acknowledge its scheduled factory reset")
                self._log("membership.remove.reset_scheduled", operation_id=self._remove_id)
            elif self._scan_attempted:
                state = self._admission(deadline=deadline)
                if state.get("phase") in {"scanning", "found"}:
                    request_id = uuid4().hex
                    self._log("membership.scan.cancel", operation_id=request_id)
                    response = self._request("/api/membership/cancel", {"request_id": request_id, "scan_id": self.scan_id}, deadline=deadline)
                    self._wait_operation(response, request_id, deadline=deadline, record=record)
            self._closed = True
            self._log("membership.cleanup.complete", retained_member=False,
                      reset_scheduled=record.get("reset") == "scheduled")
        except BaseException as error:
            record.update(error_type=type(error).__name__, retained_member=self._add_attempted and not self._add_rejected
                          and not record.get("inventory_removed", False))
            self._log("membership.cleanup.failed", level=logging.ERROR,
                      error_type=type(error).__name__, retained_member=record["retained_member"])
            raise MembershipCleanupError("Add Amity cleanup could not confirm removal/reset; inspect its report") from error

    def __enter__(self):
        return self.create()

    def __exit__(self, exc_type, exc, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            if exc is None:
                raise
            self._log("membership.cleanup.deferred", level=logging.ERROR,
                      error_type=type(cleanup_error).__name__, primary_error_type=type(exc).__name__)
            note = getattr(exc, "add_note", None)
            if callable(note):
                note("Amity removal cleanup failed; the registered TestBed cleanup will retry it.")
        return False
