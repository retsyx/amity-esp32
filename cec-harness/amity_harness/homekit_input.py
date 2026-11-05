"""Shared physical CEC proofs for commands from the shipping HomeKit accessory."""

from copy import deepcopy
import time

from .activity import ActivityAction
from .keys import Key, _prepare_key


UNSUPPORTED_KEYS = (12, 13, 14, 16, 255)
INVALID_VALUE = -70410
SELECT_REMOTE_KEY = 8
INVALID_INPUT = 17


def _input_slots(homekit, main, activities):
    document = main.request("/api/config")
    slots = {}
    for activity in activities:
        matches = [row["slot"] for row in document["external_slots"]
                   if row.get("activity_id") == activity.activity_id
                   and not row.get("hidden") and not row.get("tombstone")]
        if len(matches) != 1:
            raise AssertionError("The test activity has no unique published accessory slot")
        slots[activity.activity_id] = matches[0] + 1
    published = homekit.read_inputs()["inputs"]
    expected = [{"index": slots[activity.activity_id], "name": activity.name} for activity in activities]
    if sorted(published, key=lambda row: row["index"]) != sorted(expected, key=lambda row: row["index"]):
        raise AssertionError("HomeKit input sources do not match the saved activities")
    bed = main._testbed
    if not hasattr(bed, "control_observations"):
        bed.control_observations = []
    bed.control_observations.append({"kind": "homekit_inputs", "checks": [{"inputs": published, "passed": True}]})
    return slots


def _boundary(main, scan, kind):
    scan.validate_owners(scan.expected)
    state = main.get_state()
    if state.get("expected_boot_id") != scan._boot:
        raise AssertionError("The HomeKit accessory changed its main boot")
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
                raise AssertionError("HomeKit control changed the main boot or saved configuration")
        return state.get("current_activity") == target and state.get("activity_settling") is False

    action.state = main.wait_for(selected, timeout=main._runtime.timeout)
    action.scan.validate_owners(action.scan.expected)
    if activity is not None:
        activity._last_start_terminal = True
        activity.assert_active()
    return action


def _select(homekit, activity, slot):
    action = _boundary(activity.dut, activity.scan, "homekit_input")
    # Selection may have reached the accessory even if its response is lost.
    activity._needs_standby = True
    activity._last_start_terminal = False
    status = homekit.select_input(slot)
    if isinstance(status, bool) or status != 0:
        raise AssertionError("HomeKit rejected a published input")
    _settled(action, activity)
    if homekit.read_inputs()["current_input"] != slot:
        raise AssertionError("HomeKit ActiveIdentifier does not match the selected activity slot")
    return action


def _standby(homekit, main, scan):
    action = _boundary(main, scan, "homekit_power_off")
    status = homekit.set_power(False)
    if isinstance(status, bool) or status != 0:
        raise AssertionError("HomeKit rejected accessory power off")
    return _settled(action)


def _key(homekit, main, component, key, command):
    action = _prepare_key(main, key, 0)
    action.kind = "homekit_key"
    action.trace["interface"] = "homekit"
    status = command()
    if isinstance(status, bool) or status != 0:
        raise AssertionError("HomeKit rejected a supported accessory command")
    # The accessory call may establish its authenticated connection.
    # Retain its original wire boundary while allowing the usual CEC budget
    # after the caller returns; no second key event is sent.
    elapsed = (time.monotonic_ns() - action.started_ns) / 1e9
    action.assert_received_by(component, timeout=elapsed + main._runtime.cec_timeout)
    if action.trace.get("presses") != 1:
        raise AssertionError("HomeKit command emitted more than its single key press")
    return action


def _assert_rejected_frames(records, initiators, player, *, started, through, recovery):
    if recovery.trace.get("presses") != 1:
        raise AssertionError("HomeKit recovery emitted more than its single key press")
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
            raise AssertionError("A rejected HomeKit command emitted a physical CEC control command")


def _rejected(homekit, main, scan, activity, player):
    action = _boundary(main, scan, "homekit_rejected")
    checks = []
    for code in UNSUPPORTED_KEYS:
        status = homekit.send_key(code)
        checks.append({"key": code, "status": status})
        # The SDK can acknowledge a dropped, validly typed write with no
        # status payload. Wire effects and recovery establish that it was inert.
        if type(status) is not int or status not in (0, INVALID_VALUE):
            raise AssertionError("HomeKit returned an unexpected status for an unsupported remote key")
    input_status = homekit.select_input(INVALID_INPUT)
    if type(input_status) is not int or input_status not in (0, INVALID_VALUE):
        raise AssertionError("HomeKit returned an unexpected status for an unpublished input")
    _settled(action, activity)
    recovered = _key(homekit, main, player, Key.SELECT, lambda: homekit.send_key(SELECT_REMOTE_KEY))
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
            raise TimeoutError("CEC observations did not drain after rejected HomeKit commands")
        time.sleep(.02)
    _assert_rejected_frames(scan._session.records, scan.initiators, player,
                           started=action.started_ns, through=through, recovery=recovered)
    activity.assert_active()
    checks.extend(({"kind": "invalid_input", "index": INVALID_INPUT, "status": input_status, "passed": True},
                   {"kind": "recovery", "key": SELECT_REMOTE_KEY, "passed": True}))
    main._testbed.control_observations.append(
        {"kind": "homekit_rejected_commands", "checks": checks,
         "started_monotonic_ns": action.started_ns, "observation_end_monotonic_ns": through})
