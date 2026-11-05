"""Observe an intentional member USB reboot while its activity stays active."""

from contextvars import copy_context
import threading
import time

from .components import PowerState
from .dut import _positive_seconds
from .firmware import _peers, saved_configuration, saved_device_settings
from .membership import _identifier
from .trace import event


class ActiveMemberReboot:
    """Prove continuity before refreshing the fixture's intentional boot fence.

    The main's UI supplies identity, activity and persisted-state evidence.
    Original physical frames cover the outage, including changes to the DUT's
    CEC initiator address. No activity selection or repair is submitted here.
    """

    def __init__(self, activity, member, *, timeout=None):
        self.activity, self.member, self.main = activity, member, activity.dut
        self.bed, self.session = self.main._testbed, activity.session
        bed = self.bed
        if (bed is None or not bed._entered or bed._closed or bed.session is not self.session
                or not activity._created or activity._closed or member.main is not self.main
                or not member._created or member._closed
                or bed._member_proofs.get(member.device_id) is not member
                or member.candidate not in bed.members):
            raise ValueError("Active reboot needs this TestBed's saved activity and verified member")
        self.components = tuple(dict.fromkeys(component for component in activity.components.values()
                                              if component is not None))
        if not any(component.dut is member.candidate for component in self.components):
            raise ValueError("The rebooted member must own equipment in the active activity")
        self.timeout = self.main._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Active member reboot timeout")
        self._condition = threading.Condition()
        self._entered = self._closed = self._attempted = self._verified = False
        self._frames, self._seen = [], set()
        self._invalid_clock = False

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Active member reboot verification deadline expired")
        return remaining

    def _get(self, path, deadline):
        return self.main.request(path, timeout=min(15, self._remaining(deadline)))

    def _note(self, kind, **fields):
        self.report["checks"].append({"kind": kind, **fields})
        event("member_reboot." + kind, dut=self.main.name,
              **{"member_boot": self.member.member_boot, **fields})

    def _scope(self):
        self.session.assert_healthy()
        if (not self._entered or self._closed or self.bed._closed or not self.bed._entered
                or self.bed.session is not self.session or self.session._stop.is_set()
                or self.main._testbed is not self.bed
                or self.bed._member_proofs.get(self.member.device_id) is not self.member
                or any(component._session is not self.session
                       or component.dut._testbed is not self.bed
                       or self._observe not in component._wire_observers for component in self.components)):
            raise AssertionError("Active member reboot lost its physical TestBed observation")

    def _activity_current(self, deadline):
        state = self._get("/api/activity-editor/status", deadline)
        if (state.get("expected_boot_id") != self.boots[self.main.identity.device_id]
                or state.get("current_activity") != self.current_activity
                or state.get("activity_settling") is not False or state.get("activity_error") != ""):
            raise AssertionError("The main's activity changed or stopped during the member reboot")
        return state

    def _saved_activity(self, deadline):
        document = self._get("/api/activity-editor", deadline)
        matches = [index for index, row in enumerate(document.get("activities", []))
                   if row.get("activity_id") == self.activity.activity_id]
        if (matches != [self.current_activity]
                or document["activities"][self.current_activity] != self.activity._created_row):
            raise AssertionError("The saved activity changed across member reboot")

    def _identity(self, system, *, rebooting=False):
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id
                or system.get("boot_id") != self.boots[self.main.identity.device_id]
                or system.get("consortium_id") != self.group
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("member") is not False or membership.get("authority_ready") is not True):
            raise AssertionError("Member reboot changed the original main or its authority")
        peers = _peers(system)
        if set(peers) != set(self.peers):
            raise AssertionError("Member reboot changed the enrolled roster")
        for owner, peer in peers.items():
            if peer.get("hostname") != self.peers[owner].get("hostname"):
                raise AssertionError("Member reboot changed an enrolled hostname")
            if owner != self.member.device_id or not rebooting:
                if peer.get("boot_id") != self.boots[owner]:
                    raise AssertionError("An Amity restarted without the requested USB reboot")
        if system.get("hostname") != self.hostname:
            raise AssertionError("Member reboot changed the main's hostname")
        return peers

    def _settings(self, owner, boot, deadline):
        while True:
            self._scope()
            self._activity_current(deadline)
            response = self._get("/api/devices/" + owner + "/settings", deadline)
            body = response.get("settings") or {}
            if response.get("status") == "ready" and not response.get("error"):
                # A preceding cached settings response can still describe the
                # old boot while the newly connected peer is being read.
                if body.get("boot_id") == boot:
                    return saved_device_settings(body)
                if body.get("boot_id") not in (self.boots[owner], boot):
                    raise AssertionError("Member settings came from an unrelated boot")
            self._physical()
            time.sleep(min(.25, self._remaining(deadline)))

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("An active member reboot observation cannot be reused")
        self.activity.assert_active()
        self.activity.scan.validate_owners(self.components)
        deadline = time.monotonic() + self.timeout
        system = self._get("/api/system", deadline)
        self.peers = _peers(system)
        self.group, self.hostname = system.get("consortium_id"), system.get("hostname")
        self.boots = {self.main.identity.device_id: system.get("boot_id"),
                      **{owner: row.get("boot_id") for owner, row in self.peers.items()}}
        if (not _identifier(self.group) or any(not _identifier(boot) for boot in self.boots.values())
                or self.boots[self.member.device_id] != self.member.member_boot):
            raise AssertionError("Active member reboot has no proven current owner boots")
        self._identity(system)
        if (system.get("ready") is not True or system.get("configuration_pending") is not False
                or any(row.get("ready") is not True for row in self.peers.values())):
            raise AssertionError("Begin an active member reboot with every enrolled Amity ready")
        document = self._get("/api/activity-editor", deadline)
        matches = [index for index, row in enumerate(document.get("activities", []))
                   if row.get("activity_id") == self.activity.activity_id]
        if (len(matches) != 1 or document["activities"][matches[0]] != self.activity._created_row):
            raise AssertionError("Active member reboot requires the exact saved activity")
        self.current_activity = matches[0]
        self._activity_current(deadline)
        configuration = self._get("/api/config", deadline)
        if (system.get("applied_config_revision") != configuration.get("config_revision")
                or system.get("applied_presentation_revision") != configuration.get("presentation_revision")):
            raise AssertionError("Begin an active member reboot after saved configuration is applied")
        self.configuration = saved_configuration(configuration)
        self.routes = {component: (component.active_input, component.selected_input) for component in self.components}
        self.audio = {component: component.system_audio for component in self.components
                      if hasattr(component, "system_audio")}
        for component in self.components:
            component.assert_power(PowerState.ON)
        self.addresses = {}
        for component in self.session.components:
            self.addresses.setdefault(component.gpio, set()).add(component.address.logical)
        self.report = {"kind": "active_member_reboot", "owner_ids": sorted(self.boots),
                       "owner_boots": dict(self.boots), "activity_ids": [self.activity.activity_id],
                       "wires": sorted({component.gpio for component in self.components}), "checks": []}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self.report)
        with self.session._lock:
            self.started_ns = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(self._observe)
        self._entered = True
        self.report["started_monotonic_ns"] = self.started_ns
        try:
            self.settings = {owner: self._settings(owner, boot, deadline) for owner, boot in self.boots.items()}
            self._physical()
        except BaseException:
            self.close()
            raise
        self._note("observing", main_boot=self.boots[self.main.identity.device_id])
        return self

    def _observe(self, component, frame, transport):
        if transport != "wire" or frame.source in self.addresses[component.gpio]:
            return
        with self._condition:
            if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                self._invalid_clock = True
            elif frame.timestamp_ns >= self.started_ns:
                key = (component.gpio, frame.timestamp_ns, frame.raw)
                if key not in self._seen:
                    self._seen.add(key)
                    self._frames.append((component.gpio, frame))
                    if len(self._frames) > 4096:
                        self._invalid_clock = True
            self._condition.notify_all()

    def _physical(self):
        self._scope()
        with self._condition:
            frames, invalid = tuple(self._frames), self._invalid_clock
        if invalid:
            raise AssertionError("Active member reboot lost bounded, timestamped physical CEC evidence")
        for component in self.components:
            expected_route, expected_input = self.routes[component]
            if (component.power != PowerState.ON or component.active_input != expected_route
                    or component.selected_input != expected_input
                    or component in self.audio and component.system_audio != self.audio[component]):
                raise AssertionError("Active equipment lost power, audio or its selected input during member reboot")
            for gpio, frame in frames:
                if gpio != component.gpio or frame.destination not in (component.address.logical, 15):
                    continue
                off = frame.opcode == 0x36 and not frame.operands
                off |= (frame.destination == component.address.logical and frame.opcode == 0x44
                        and len(frame.operands) == 1 and frame.operands[0] in (0x40, 0x6b, 0x6c))
                route = None
                if (component.can_route and frame.destination == 15
                        and frame.opcode in (0x82, 0x86) and len(frame.operands) == 2):
                    route = int.from_bytes(frame.operands, "big")
                elif (component.can_route and frame.destination == component.address.logical
                      and frame.opcode == 0x70 and len(frame.operands) == 2):
                    route = int.from_bytes(frame.operands, "big")
                wrong_route = route is not None and route != expected_route
                wrong_input = (component.can_route and frame.destination == component.address.logical
                               and frame.opcode == 0x44 and len(frame.operands) == 2
                               and frame.operands[0] == 0x69 and frame.operands[1] != expected_input)
                changed_audio = (component in self.audio and frame.destination == component.address.logical
                                 and frame.opcode == 0x70 and len(frame.operands) in (0, 2)
                                 and bool(frame.operands) != self.audio[component])
                if off or wrong_route or wrong_input or changed_audio:
                    self._note("continuity_failed", component=component.name, gpio=component.gpio,
                               reply_frame=frame.raw.hex(":"), reply_wire_timestamp_ns=frame.timestamp_ns)
                    raise AssertionError("The DUT interrupted power, audio or input during member reboot")

    def _drain(self, through, deadline):
        while True:
            self._physical()
            with self.session._lock:
                idle = dict(self.session._wire_idle_since_ns)
                if all(idle.get(component.gpio, 0) >= through for component in self.components):
                    return {str(component.gpio): idle[component.gpio] for component in self.components}
            with self._condition:
                self._condition.wait(min(.02, self._remaining(deadline)))

    def _persisted(self, system, peers, deadline):
        configuration = self._get("/api/config", deadline)
        if saved_configuration(configuration) != self.configuration:
            raise AssertionError("Member reboot changed saved equipment or activities")
        if (system.get("configuration_pending") is not False
                or system.get("applied_config_revision") != configuration.get("config_revision")
                or system.get("applied_presentation_revision") != configuration.get("presentation_revision")):
            return False
        for owner, expected in self.settings.items():
            boot = system["boot_id"] if owner == self.main.identity.device_id else peers[owner]["boot_id"]
            if self._settings(owner, boot, deadline) != expected:
                raise AssertionError("Member reboot changed saved device settings")
        return True

    def reboot(self):
        self._scope()
        if self._attempted:
            raise RuntimeError("Submit exactly one USB reboot per active observation")
        deadline = time.monotonic() + self.timeout
        self._activity_current(deadline)
        self._physical()
        self._attempted = True
        errors = []

        def reset():
            try:
                self.member.candidate.usb_reboot()
            except BaseException as error:
                errors.append(error)

        # Keep observing the authenticated main while the bounded USB worker
        # runs. The worker touches only the headless candidate's USB connection.
        worker = threading.Thread(target=copy_context().run, args=(reset,), name="active-member-usb-reboot", daemon=True)
        worker.start()
        try:
            while worker.is_alive():
                self._activity_current(deadline)
                self._physical()
                worker.join(min(.25, self._remaining(deadline)))
        finally:
            worker.join(31)
        if errors:
            raise errors[0]
        observed_boot = None
        while True:
            self._physical()
            self._activity_current(deadline)
            system = self._get("/api/system", deadline)
            peers = self._identity(system, rebooting=True)
            boot = peers[self.member.device_id].get("boot_id")
            if _identifier(boot) and boot != self.boots[self.member.device_id]:
                if observed_boot is not None and boot != observed_boot:
                    raise AssertionError("The member restarted again during reboot recovery")
                observed_boot = boot
            if (_identifier(boot) and boot != self.boots[self.member.device_id]
                    and system.get("ready") is True and all(row.get("ready") is True for row in peers.values())
                    and self._persisted(system, peers, deadline)):
                break
            time.sleep(min(.25, self._remaining(deadline)))
        # Continuity must pass before a UI scan can refresh discovery proof.
        through = time.monotonic_ns()
        idle = self._drain(through, deadline)
        self._activity_current(deadline)
        # Activity.assert_active also validates its discovery owner's old
        # boot. Verify the same saved row directly until the intentional new
        # boot has fresh physical discovery proof below.
        self._saved_activity(deadline)
        self._note("continued_before_scan", member_boot=boot,
                   observation_end_monotonic_ns=through, idle_receive_since_ns=idle)
        self.member.member_boot = boot
        for proof in self.bed._member_proofs.values():
            for row in proof._initial_system["membership"]["peers"]:
                if row.get("octopus_id") == self.member.device_id:
                    row["boot_id"] = boot
        self.boots[self.member.device_id] = boot
        self.report["owner_boots"] = dict(self.boots)
        fresh = self.main.scan_hdmi(timeout=self._remaining(deadline))
        fresh.assert_contains(*self.bed.components)
        fresh.assert_contains_only(*self.bed.components)
        self._saved_activity(deadline)
        self.activity.scan = fresh
        self.activity.refresh_scan(fresh)
        self._verified = True
        self.assert_continued()
        return self

    def assert_continued(self):
        self._scope()
        if not self._verified:
            raise RuntimeError("Verify the requested member reboot before asserting completion")
        deadline = time.monotonic() + self.timeout
        through = time.monotonic_ns()
        idle = self._drain(through, deadline)
        self._activity_current(deadline)
        system = self._get("/api/system", deadline)
        peers = self._identity(system)
        if (system.get("ready") is not True or any(row.get("ready") is not True for row in peers.values())
                or not self._persisted(system, peers, deadline)):
            raise AssertionError("The rebooted member did not remain ready with unchanged configuration")
        self.activity.assert_active()
        self._physical()
        self._note("continued", member_boot=self.member.member_boot, configuration_unchanged=True,
                   settings_unchanged=True, observation_end_monotonic_ns=through,
                   observed_frame_count=len(self._frames), idle_receive_since_ns=idle)
        return self

    def close(self):
        if self._entered and not self._closed:
            with self.session._lock:
                for component in self.components:
                    component._wire_observers.discard(self._observe)
            self._closed = True

    def __exit__(self, exc_type, exc, traceback):
        try:
            if exc_type is None:
                self.assert_continued()
        finally:
            self.close()
        return False
