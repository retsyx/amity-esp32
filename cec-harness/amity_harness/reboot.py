"""Observe an intentional member or main USB reboot during an activity."""

from contextvars import copy_context
import math
import threading
import time

from .components import PowerState
from .dut import AmityHTTPError, _positive_seconds
from .firmware import _READ_ERRORS, _peers, _retryable, saved_configuration, saved_device_settings
from .membership import _identifier
from .trace import event


# Power, routing, input, audio and key commands. A booting main may claim
# addresses, announce itself and query its equipment; it must not command it.
_EQUIPMENT_COMMANDS = frozenset((0x04, 0x0d, 0x36, 0x44, 0x45, 0x70, 0x80, 0x81, 0x82, 0x86, 0x9d))


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


class MainReboot:
    """USB-reboot only the original main while one of its activities is current.

    A main boots with no current activity. Its members keep their boots,
    connections and discovery caches, adopt no activity and send nothing on
    their CEC wires until a user action. Original physical frames cover the
    whole outage and ``hold`` seconds after the members are proven connected
    again: any member frame fails, and the booting main must not command its
    own equipment. Only then do the fixture's main boot fences move to the new
    boot, through a fresh UI scan. No activity selection or repair is
    submitted here.
    """

    def __init__(self, activity, members, *, saved=(), hold, timeout=None):
        self.activity, self.main = activity, activity.dut
        self.members, self.activities = tuple(members), (activity, *saved)
        self.bed, self.session = self.main._testbed, activity.session
        bed = self.bed
        proofs = getattr(bed, "_member_proofs", {}) if bed is not None else {}
        if (bed is None or not bed._entered or bed._closed or bed.session is not self.session
                or not self.members or len(self.members) != len(proofs)
                or any(proofs.get(member.device_id) is not member or member.main is not self.main
                       or not member._created or member._closed or member.candidate not in bed.members
                       for member in self.members)
                or len({id(item) for item in self.activities}) != len(self.activities)
                or any(item.dut is not self.main or item.session is not self.session
                       or not item._created or item._closed for item in self.activities)):
            raise ValueError("A main reboot needs this TestBed's saved activities and every verified member")
        candidates = tuple(member.candidate for member in self.members)
        self.components = tuple(bed.components)
        if (not any(component.dut in candidates for component in self.components)
                or any(component.dut is not self.main and component.dut not in candidates
                       for component in self.components)):
            raise ValueError("A main reboot observes every member wire and the main's own equipment")
        self.timeout = self.main._runtime.timeout if timeout is None else timeout
        _positive_seconds(self.timeout, "Main reboot timeout")
        if isinstance(hold, bool) or not isinstance(hold, (int, float)) or not math.isfinite(hold) or hold <= 0:
            raise ValueError("A main reboot needs a positive finite quiet hold")
        self.hold_ns = int(hold * 1_000_000_000)
        self._condition = threading.Condition()
        self._entered = self._closed = self._observing = self._attempted = self._verified = False
        self._frames, self._seen = [], set()
        self._invalid_clock = False
        self.boot = None

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Main reboot verification deadline expired")
        return remaining

    def _get(self, path, deadline):
        """Read the main's UI, reauthenticating across its restart."""
        while True:
            try:
                self.main.authenticate()
                return self.main.request(path, timeout=min(15, self._remaining(deadline)))
            except _READ_ERRORS as error:
                if isinstance(error, AmityHTTPError) and error.status == 401:
                    # A restart revokes sessions. Reads are safe to repeat.
                    self.main._token = None
                elif not _retryable(error):
                    raise
            if self._observing:
                self._physical()
            time.sleep(min(.25, self._remaining(deadline)))

    def _note(self, kind, **fields):
        self.report["checks"].append({"kind": kind, **fields})
        event("main_reboot." + kind, dut=self.main.name, **fields)

    def _scope(self):
        self.session.assert_healthy()
        if (not self._entered or self._closed or self.bed._closed or not self.bed._entered
                or self.bed.session is not self.session or self.session._stop.is_set()
                or self.main._testbed is not self.bed
                or any(self.bed._member_proofs.get(member.device_id) is not member for member in self.members)
                or any(component._session is not self.session or component.dut._testbed is not self.bed
                       or self._observing and self._observe not in component._wire_observers
                       for component in self.components)):
            raise AssertionError("The main reboot lost its physical TestBed observation")

    @staticmethod
    def _state(component):
        return (component.power, component.active_input, component.selected_input,
                getattr(component, "system_audio", None))

    def _identity(self, system):
        membership = system.get("membership", {})
        if (system.get("octopus_id") != self.main.identity.device_id
                or system.get("consortium_id") != self.group or system.get("hostname") != self.hostname
                or system.get("mode") != "coordinator" or membership.get("coordinator") is not True
                or membership.get("member") is not False):
            raise AssertionError("The main reboot changed the original main or its authority")
        peers = _peers(system)
        if set(peers) != set(self.boots):
            raise AssertionError("The main reboot changed the enrolled roster")
        for owner, row in peers.items():
            if row.get("hostname") != self.peers[owner].get("hostname"):
                raise AssertionError("The main reboot changed an enrolled hostname")
            # A member's boot is null until its link reconnects.
            if _identifier(row.get("boot_id")) and row["boot_id"] != self.boots[owner]:
                raise AssertionError("A member restarted while only the main rebooted")
        return peers

    def _settings(self, owner, boot, deadline):
        while True:
            response = self._get("/api/devices/" + owner + "/settings", deadline)
            body = response.get("settings") or {}
            if response.get("status") == "ready" and not response.get("error"):
                if body.get("boot_id") == boot:
                    return saved_device_settings(body)
                raise AssertionError("Device settings came from an unrelated boot")
            if self._observing:
                self._physical()
            time.sleep(min(.25, self._remaining(deadline)))

    def __enter__(self):
        if self._entered or self._closed:
            raise RuntimeError("A main reboot observation cannot be reused")
        self.activity.assert_active()
        deadline = time.monotonic() + self.timeout
        system = self._get("/api/system", deadline)
        self.peers = _peers(system)
        self.group, self.hostname = system.get("consortium_id"), system.get("hostname")
        self.old_boot = system.get("boot_id")
        self.boots = {owner: row.get("boot_id") for owner, row in self.peers.items()}
        if (not _identifier(self.group) or not _identifier(self.old_boot)
                or any(item.boot != self.old_boot for item in self.activities)
                or any(not _identifier(boot) for boot in self.boots.values())
                or any(member.boot != self.old_boot or self.boots.get(member.device_id) != member.member_boot
                       for member in self.members)):
            raise AssertionError("A main reboot needs proven main and member boots")
        self._identity(system)
        if (system.get("ready") is not True or system.get("configuration_pending") is not False
                or system.get("membership", {}).get("authority_ready") is not True
                or any(row.get("ready") is not True for row in self.peers.values())):
            raise AssertionError("Begin a main reboot with every enrolled Amity ready")
        configuration = self._get("/api/config", deadline)
        if (system.get("applied_config_revision") != configuration.get("config_revision")
                or system.get("applied_presentation_revision") != configuration.get("presentation_revision")):
            raise AssertionError("Begin a main reboot after saved configuration is applied")
        self.configuration = saved_configuration(configuration)
        self._saved_activities(deadline)
        owners = {self.main.identity.device_id: self.old_boot, **self.boots}
        self.settings = {owner: self._settings(owner, boot, deadline) for owner, boot in owners.items()}
        self.addresses = {}
        for component in self.session.components:
            self.addresses.setdefault(component.gpio, set()).add(component.address.logical)
        # "wires" are the member wires, which must stay silent.
        self.report = {"kind": "main_reboot", "owner_ids": sorted(owners), "main_boot": self.old_boot,
                       "owner_boots": dict(self.boots),
                       "activity_ids": [item.activity_id for item in self.activities],
                       "wires": sorted({component.gpio for component in self.components
                                        if component.dut is not self.main}), "checks": []}
        if not hasattr(self.bed, "control_observations"):
            self.bed.control_observations = []
        self.bed.control_observations.append(self.report)
        with self.session._lock:
            # Frames change component state only under this lock.
            self.states = {component: self._state(component) for component in self.components}
            self.started_ns = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(self._observe)
        self._entered = self._observing = True
        self.report["started_monotonic_ns"] = self.started_ns
        self._note("observing", main_boot=self.old_boot,
                   main_wires=sorted({component.gpio for component in self.components if component.dut is self.main}))
        return self

    def _saved_activities(self, deadline):
        document = self._get("/api/activity-editor", deadline)
        for item in self.activities:
            if ([row for row in document.get("activities", []) if row.get("activity_id") == item.activity_id]
                    != [item._created_row]):
                raise AssertionError("The saved activity changed across the main reboot")

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
                    self._frames.append((component, frame))
                    if len(self._frames) > 4096:
                        self._invalid_clock = True
            self._condition.notify_all()

    def _reject(self, kind, component, frame, message):
        self._note(kind, component=component.name, gpio=component.gpio,
                   frame=frame.raw.hex(":"), wire_timestamp_ns=frame.timestamp_ns)
        raise AssertionError(message)

    def _physical(self):
        self._scope()
        with self._condition:
            frames, invalid = tuple(self._frames), self._invalid_clock
        if invalid:
            raise AssertionError("The main reboot lost bounded, timestamped physical CEC evidence")
        for component, frame in frames:
            if component.dut is not self.main:
                self._reject("member_transmitted", component, frame,
                             "A member sent CEC while only the main rebooted")
            if frame.opcode in _EQUIPMENT_COMMANDS:
                self._reject("main_commanded", component, frame,
                             "The booting main commanded its equipment")
        for component in self.components:
            if self._state(component) != self.states[component]:
                self._note("state_changed", component=component.name, gpio=component.gpio)
                raise AssertionError("Equipment changed power or input while only the main rebooted")

    def _drain(self, through, deadline, check):
        wires = {component.gpio for component in self.components}
        while True:
            check()
            with self.session._lock:
                idle = dict(self.session._wire_idle_since_ns)
            if time.monotonic_ns() >= through and all(idle.get(gpio, 0) >= through for gpio in wires):
                return {str(gpio): idle[gpio] for gpio in sorted(wires)}
            with self._condition:
                self._condition.wait(min(.02, self._remaining(deadline)))

    def _recovered(self, system, deadline):
        """Return True once the new boot and its members are ready again."""
        if (system.get("membership", {}).get("authority_ready") is not True
                or system.get("mode") != "coordinator" or system.get("configuration_pending") is not False
                or system.get("ready") is not True):
            # A restarted main publishes its role and member links only once
            # its saved configuration and authority are ready.
            return False
        peers = self._identity(system)
        if any(row.get("ready") is not True or row.get("boot_id") != self.boots[owner]
               for owner, row in peers.items()):
            return False
        configuration = self._get("/api/config", deadline)
        if saved_configuration(configuration) != self.configuration:
            raise AssertionError("The main reboot changed saved equipment or activities")
        if (system.get("applied_config_revision") != configuration.get("config_revision")
                or system.get("applied_presentation_revision") != configuration.get("presentation_revision")):
            return False
        state = self._get("/api/activity-editor/status", deadline)
        if state.get("expected_boot_id") != system["boot_id"]:
            return False
        if state.get("current_activity") != -1:
            raise AssertionError("The main restored an activity across its reboot")
        return True

    def reboot(self):
        """Reboot the main; verify a quiet, unchanged system before the new scan."""
        self._scope()
        if self._attempted:
            raise RuntimeError("Submit exactly one main USB reboot per observation")
        self._physical()
        self._attempted = True
        deadline = time.monotonic() + self.timeout
        self._note("usb_reboot", main_boot=self.old_boot)
        self.main.usb_reboot()
        boot = None
        while True:
            self._physical()
            system = self._get("/api/system", deadline)
            if system.get("octopus_id") != self.main.identity.device_id:
                raise AssertionError("The main reboot reached another device")
            current = system.get("boot_id")
            if _identifier(current) and current != self.old_boot:
                if boot is not None and current != boot:
                    raise AssertionError("The main restarted again during reboot recovery")
                boot = current
                if self._recovered(system, deadline):
                    break
            time.sleep(min(.25, self._remaining(deadline)))
        owners = {self.main.identity.device_id: boot, **self.boots}
        for owner, owner_boot in owners.items():
            if self._settings(owner, owner_boot, deadline) != self.settings[owner]:
                raise AssertionError("The main reboot changed saved device settings")
        self._saved_activities(deadline)
        # The members were bound to the new boot before they were reported
        # ready. Keep watching for the hold, then drain every observed wire.
        through = time.monotonic_ns() + self.hold_ns
        idle = self._drain(through, deadline, self._physical)
        with self._condition:
            main_frames = len(self._frames)
        self._stop_observing()
        self._note("members_quiet", main_boot=boot, member_frame_count=0, main_frame_count=main_frames,
                   observation_end_monotonic_ns=through, idle_receive_since_ns=idle)
        # Only the main's boot changed. Each member keeps its proven boot,
        # original roster subset and saved baseline.
        for member in self.members:
            member.boot = boot
            member._initial_system["boot_id"] = boot
        for item in self.activities:
            item.boot = boot
        self.boot = self.report["main_boot"] = boot
        fresh = self.main.scan_hdmi()
        fresh.assert_contains(*self.components)
        fresh.assert_contains_only(*self.components)
        self._saved_activities(time.monotonic() + self.timeout)
        for item in self.activities:
            item.scan = fresh
            item.refresh_scan(fresh)
        self._verified = True
        self._note("rebooted", old_main_boot=self.old_boot, main_boot=boot, owner_boots=dict(self.boots))
        return self

    def assert_no_activity_policy(self, front, source):
        """Announce a member adapter's back source while nothing is current.

        That adapter's owner must broadcast Standby on its front and back
        wires. From the announcement until ``hold`` seconds after the later
        Standby, no Amity may send anything else on any observed wire, and
        both components must still be in Standby.
        """
        if not self._verified or self._closed:
            raise RuntimeError("Verify the main reboot before probing a member adapter")
        if (front not in self.components or source not in self.components or front.dut is self.main
                or source.dut is not front.dut or source.tentacle != front.tentacle
                or front.direction != "front" or source.direction != "back"):
            raise ValueError("Probe one member adapter with its front equipment and back source")
        self.session.assert_healthy()
        scan = self.main.last_scan
        initiators = dict(scan.initiators)
        frames, seen, invalid = [], set(), []

        def observe(component, frame, transport):
            if transport != "wire" or frame.source in self.addresses[component.gpio]:
                return
            with self._condition:
                if type(frame.timestamp_ns) is not int or frame.timestamp_ns <= 0:
                    invalid.append(True)
                elif frame.timestamp_ns >= started:
                    key = (component.gpio, frame.timestamp_ns, frame.raw)
                    if key not in seen:
                        seen.add(key)
                        frames.append((component, frame))
                self._condition.notify_all()

        with self.session._lock:
            started = time.monotonic_ns()
            for component in self.components:
                component._wire_observers.add(observe)
        try:
            announcement = source.activate()
            if not announcement.ok:
                raise AssertionError("The member source's Active Source was not transmitted")
            proofs = []
            for component in (front, source):
                component.wait_for_power(PowerState.STANDBY, since=announcement.timestamp_ns,
                                         command="standby", broadcast=True)
                proofs.append(component.power_assertions[-1])
            through = max(proof["wire_timestamp_ns"] for proof in proofs) + self.hold_ns
            expected = {(component.gpio, proof["frame"]) for component, proof in zip((front, source), proofs)}

            def check():
                self.session.assert_healthy()
                with self._condition:
                    observed = tuple(frames)
                if invalid:
                    raise AssertionError("The no-activity probe lost timestamped physical CEC evidence")
                for component, frame in observed:
                    standby = (frame.source == initiators.get(component.gpio)
                               and (component.gpio, frame.raw.hex(":")) in expected)
                    if not standby or sum(1 for other, item in observed
                                          if other.gpio == component.gpio and item.raw == frame.raw) > 1:
                        self._reject("policy_extra_frame", component, frame,
                                     "An Amity sent more than the no-activity Standby")

            idle = self._drain(through, time.monotonic() + self.timeout, check)
        finally:
            with self.session._lock:
                for component in self.components:
                    component._wire_observers.discard(observe)
        for component in (front, source):
            component.assert_power(PowerState.STANDBY)
        self._note("no_activity_policy", component=source.name, gpio=source.gpio,
                   announcement_frame=announcement.frame.raw.hex(":"),
                   announcement_wire_timestamp_ns=announcement.timestamp_ns,
                   standby_frames=[proof["frame"] for proof in proofs],
                   standby_wire_timestamp_ns=[proof["wire_timestamp_ns"] for proof in proofs],
                   observation_end_monotonic_ns=through, idle_receive_since_ns=idle)
        return self

    def _stop_observing(self):
        if self._observing:
            with self.session._lock:
                for component in self.components:
                    component._wire_observers.discard(self._observe)
            self._observing = False

    def close(self):
        if self._entered and not self._closed:
            self._stop_observing()
            self._closed = True

    def __exit__(self, exc_type, exc, traceback):
        self.close()
        if exc_type is None and not self._verified:
            raise RuntimeError("The main reboot was not verified")
        return False
