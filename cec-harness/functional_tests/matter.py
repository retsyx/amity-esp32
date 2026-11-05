"""Exercise the shipping Matter accessory against physical CEC recipients."""

from contextlib import nullcontext
from copy import deepcopy
import time

from amity_harness import DUT, TestBed, TvComponent, AvrComponent, PlaybackComponent, PowerState, Key
from amity_harness.activity import ActivityAction
from amity_harness.keys import _prepare_key
from amity_harness.matter import MatterAccessory


KEYPAD_KEYS = (*range(0x0E), 0x10, 0x11, *range(0x20, 0x2A))
UNSUPPORTED_KEYS = (0x0E, 0x30, 0x40, 0x41, 0x44, 0xFF)


def _boundary(main, scan, kind):
    scan.validate_owners(scan.expected)
    state = main.get_state()
    if state.get("expected_boot_id") != scan._boot:
        raise AssertionError("The Matter accessory changed its main boot")
    return ActivityAction(main, scan._session, time.monotonic_ns(), scan._boot,
                          dict(scan.initiators), kind, "", None, state=deepcopy(state),
                          scan=scan, testbed=main._testbed,
                          owner_boots=dict(scan.owner_boots), owner_duts=dict(scan.owner_duts))


def _settled(action, activity=None):
    main = action.dut
    rows = main.get_configuration()["activities"]
    target = -1 if activity is None else next(
        index for index, row in enumerate(rows) if row["activity_id"] == activity.activity_id)

    def selected(state):
        for field in ("expected_boot_id", "expected_config_revision", "expected_presentation_revision"):
            if state.get(field) != action.state.get(field):
                raise AssertionError("Matter control changed the main boot or saved configuration")
        return state.get("current_activity") == target and state.get("activity_settling") is False

    action.state = main.wait_for(selected, timeout=main._runtime.timeout)
    action.scan.validate_owners(action.scan.expected)
    if activity is not None:
        activity._last_start_terminal = True
        activity.assert_active()
    return action


def _select(matter, activity, slot):
    action = _boundary(activity.dut, activity.scan, "matter_input")
    # Selection may have reached the accessory even if its response is lost.
    activity._needs_standby = True
    activity._last_start_terminal = False
    if matter.select_input(slot) != 0:
        raise AssertionError("Matter rejected a published input")
    _settled(action, activity)
    if matter.read_inputs()["current_input"] != slot:
        raise AssertionError("Matter CurrentInput does not match the selected activity slot")
    return action


def _key(matter, main, component, key, command):
    action = _prepare_key(main, key, 0)
    action.kind = "matter_key"
    action.trace["interface"] = "matter"
    if command() != 0:
        raise AssertionError("Matter rejected a supported accessory command")
    # CASE establishment and worker shutdown are part of the command call.
    # Retain its original wire boundary while allowing the usual CEC budget
    # after the caller returns; no second key event is sent.
    elapsed = (time.monotonic_ns() - action.started_ns) / 1e9
    action.assert_received_by(component, timeout=elapsed + main._runtime.cec_timeout)
    return action


def _assert_rejected_frames(records, initiators, player, *, started, through, recovery):
    if recovery.trace.get("presses") != 1:
        raise AssertionError("Matter recovery emitted more than its single key press")
    press_stamp = recovery.trace["press_wire_timestamp_ns"]
    release_stamp = recovery.trace["release_wire_timestamp_ns"]
    # Include the recovery command's entire wire interval: a rejected command
    # must not leak a delayed press, release, power or routing operation.
    for record in records:
        if (record.get("direction") != "rx" or record.get("transport") != "wire"
                or record.get("source") != initiators.get(record.get("gpio"))
                or not started <= record.get("wire_timestamp_ns", 0) <= through
                or record.get("opcode") not in (0x04, 0x0D, 0x36, 0x44, 0x45, 0x82, 0x86)):
            continue
        expected = (record.get("gpio") == player.gpio
                    and record.get("destination") == player.address.logical
                    and (record["opcode"] == 0x44 and record["wire_timestamp_ns"] == press_stamp
                         and record["frame"].split(":")[2:] == ["00"]
                         or record["opcode"] == 0x45 and record["wire_timestamp_ns"] == release_stamp
                         and len(record["frame"].split(":")) == 2))
        if not expected:
            raise AssertionError("A rejected Matter command emitted a physical CEC control command")


def _rejected(matter, main, scan, activity, player):
    action = _boundary(main, scan, "matter_rejected")
    checks = []
    for code in UNSUPPORTED_KEYS:
        status = matter.send_key(code)
        checks.append({"key": code, "status": status})
        if status != 1:
            raise AssertionError("An unsupported Matter keypad key was not rejected")
    if matter.select_input(255) != 1:
        raise AssertionError("Matter accepted an unpublished input")
    _settled(action, activity)
    recovered = _key(matter, main, player, Key.SELECT, lambda: matter.send_key(Key.SELECT))
    through = time.monotonic_ns()
    deadline = time.monotonic() + main._runtime.cec_timeout
    wires = set(scan.initiators)
    while True:
        scan._session.assert_healthy()
        with scan._session._lock:
            drained = all(scan._session._wire_idle_since_ns.get(gpio, 0) >= through for gpio in wires)
        if drained:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError("CEC observations did not drain after rejected Matter commands")
        time.sleep(.02)
    _assert_rejected_frames(scan._session.records, scan.initiators, player,
                           started=action.started_ns, through=through, recovery=recovered)
    activity.assert_active()
    checks.extend(({"kind": "invalid_input", "index": 255, "status": 1, "passed": True},
                   {"kind": "recovery", "key": int(Key.SELECT), "passed": True}))
    main._testbed.control_observations.append(
        {"kind": "matter_rejected_commands", "checks": checks,
         "started_monotonic_ns": action.started_ns, "observation_end_monotonic_ns": through})


def _matter_accessory(*, remote):
    main = DUT("amity-dut")
    target = DUT("amity-dut-2") if remote else main
    connection = target.tentacle(0)
    tv = TvComponent(connection.front, "Matter TV")
    avr = AvrComponent(connection.back, "Matter AVR")
    first = PlaybackComponent(avr, "Matter First", port=1)
    second = PlaybackComponent(avr, "Matter Second", port=2, address_type="tuner")
    components = (tv, avr, first, second)
    duts = (main, target) if remote else (main,)
    with TestBed(*duts, *components, members=(target,) if remote else ()) as bed:
        with (main.add_amity(target) if remote else nullcontext(main)) as owner:
            if remote:
                owner.assert_connected()
            scan = main.scan_hdmi()
            scan.assert_contains_only(*components)
            for component in components:
                scan.assert_on(component, component.endpoint)
            with main.create_activity("Functional Matter first", display=tv, source=first, audio=avr) as initial:
                with main.create_activity("Functional Matter second", display=tv, source=second, audio=avr) as following:
                    document = main.request("/api/config")
                    slots = {}
                    for activity in (initial, following):
                        matches = [row["slot"] for row in document["external_slots"]
                                   if row.get("activity_id") == activity.activity_id
                                   and not row.get("hidden") and not row.get("tombstone")]
                        if len(matches) != 1:
                            raise AssertionError("The test activity has no unique published accessory slot")
                        slots[activity.activity_id] = matches[0]
                    if not hasattr(bed, "control_observations"):
                        bed.control_observations = []
                    with MatterAccessory(main) as matter:
                        published = matter.read_inputs()["inputs"]
                        expected = [{"index": slots[activity.activity_id], "name": activity.name}
                                    for activity in (initial, following)]
                        if sorted(published, key=lambda row: row["index"]) != sorted(expected, key=lambda row: row["index"]):
                            raise AssertionError("Matter InputList does not match the saved activities")
                        bed.control_observations.append(
                            {"kind": "matter_inputs", "checks": [{"inputs": published, "passed": True}]})
                        started = _select(matter, initial, slots[initial.activity_id])
                        for component in (tv, avr, first):
                            component.wait_for_power(PowerState.ON, since=started)
                        second.assert_power(PowerState.STANDBY)
                        tv.wait_for_amity_input(since=started)
                        avr.wait_for_input(first, since=started)
                        for code in KEYPAD_KEYS:
                            _key(matter, main, first, code, lambda code=code: matter.send_key(code))
                        for command, code in (("play", Key.PLAY), ("pause", Key.PAUSE), ("stop", Key.STOP),
                                              ("previous", 0x4C), ("next", 0x4B)):
                            _key(matter, main, first, code, lambda command=command: matter.playback(command))
                        for steps, code in ((1, Key.VOLUME_UP), (-1, Key.VOLUME_DOWN)):
                            _key(matter, main, avr, code, lambda steps=steps: matter.step_volume(steps))
                        for muted, code in ((True, 0x65), (False, 0x66)):
                            _key(matter, main, avr, code, lambda muted=muted: matter.set_mute(muted))
                        _rejected(matter, main, scan, initial, first)
                        changed = _select(matter, following, slots[following.activity_id])
                        first.wait_for_power(PowerState.STANDBY, since=changed, command="standby", directed=True)
                        second.wait_for_power(PowerState.ON, since=changed)
                        for component in (tv, avr):
                            component.assert_power(PowerState.ON)
                        tv.wait_for_amity_input(since=changed)
                        avr.wait_for_input(second, since=changed)
                        _key(matter, main, second, Key.RIGHT, lambda: matter.send_key(Key.RIGHT))
                        stopped = _boundary(main, scan, "matter_power_off")
                        if matter.set_power(False) != 0:
                            raise AssertionError("Matter rejected accessory power off")
                        _settled(stopped)
                        for component in (tv, avr, second):
                            component.wait_for_power(PowerState.STANDBY, since=stopped, command="standby")
                        first.assert_power(PowerState.STANDBY)
            if remote:
                owner.assert_connected()


def test_matter_accessory_local():
    _matter_accessory(remote=False)


def test_matter_accessory_remote():
    _matter_accessory(remote=True)
