"""Temporary activity ownership uses existing UI transactions and safe cleanup."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.activity import Activity, ActivityCleanupError, ActivityConflict
from amity_harness import AvrComponent, DUT, Frame, PlaybackComponent, TvComponent, assign_addresses
from amity_harness.dut import AmityHTTPError
import test_scan_assertions as scan_fixture


class Fixture:
    def __init__(self):
        source = scan_fixture.ScanAssertionTests()
        source.setUp()
        self.source = source
        self.dut, self.tv, self.avr, self.player = source.dut, source.tv, source.avr, source.player
        self.healthy, self.callbacks, self.calls, self.operations = True, [], [], {}
        self.session = SimpleNamespace(assert_healthy=self.health, records=source.result.records)
        self.bed = SimpleNamespace(session=self.session, _entered=True, _closed=False, add_cleanup=self.callbacks.append)
        self.dut._testbed = self.bed
        for component in source.components:
            component._session = self.session
        source.result._session = self.session
        self.dut.last_scan = source.result
        self.rows = [{"activity_id": "9" * 32, "name": "Existing activity", "display": "", "source": "", "audio": ""}]
        self.full = {**deepcopy(source.config), "activities": [], "external_slots": [{"slot": 0, "activity_id": "9" * 32}],
                     "controller_octopus_id": self.dut.identity.device_id, "primary_zone_id": "d" * 32,
                     "components": [{"component_id": "f" * 32, "label": "Preexisting equipment", "qualified_names": []}]}
        self.selectors = {component.name: row["name"] for component, row in zip(source.components, source.rows)}
        self.choice_roles = {self.tv.name: ["display", "audio"], self.avr.name: ["audio", "switch"],
                             self.player.name: ["source"]}
        self.current, self.pending_stop, self.state_revision, self.polls = None, 0, 1, 0
        self.next_activity_number = 10
        self.reject_create = self.lose_create_reply = self.conflict_cleanup = self.change_boot_on_action = self.reject_start = False
        self.dut.get_configuration, self.dut.get_state = self.editor, self.state
        self.dut.request, self.dut.wait_for = self.request, self.wait_for
        self.dut.wait_operation = lambda result, **kwargs: self.operations[result["operation_id"]]
        self.dut.get_system = lambda: deepcopy(source.system)
        self.sync_full()

    def add_member(self):
        self.member = DUT("amity-dut-2")
        self.member._testbed = self.bed
        self.bed.duts, self.bed.members = (self.dut, self.member), (self.member,)
        self.member_boot = "d" * 32
        proof = SimpleNamespace(main=self.dut, candidate=self.member, session=self.session,
                                device_id=self.member.identity.device_id, boot=self.source.boot,
                                group=self.source.group, member_boot=self.member_boot, _created=True, _closed=False)
        self.bed._member_proofs = {self.member.identity.device_id: proof}
        self.source.system.update(mode="coordinator", ready=True,
                                  membership={"coordinator": True, "authority_ready": True, "member": False,
                                              "peers": [{"octopus_id": self.member.identity.device_id,
                                                         "boot_id": self.member_boot, "enrolled": True,
                                                         "ready": True, "provisional": False}]})
        connector = self.member.tentacle(0)
        self.member_tv = TvComponent(connector.front, "Other TV")
        self.member_avr = AvrComponent(connector.back, "Other AVR")
        self.member_player = PlaybackComponent(self.member_avr, "Other Player", port=1)
        added = (self.member_tv, self.member_avr, self.member_player)
        originals = self.source.components
        assign_addresses((*originals, *added))
        records = list(self.source.result.records)
        for index, (old, component) in enumerate(zip(originals, added)):
            component._session = self.session
            name = deepcopy(self.source.system["observations"][index]["qualified_name"])
            name.update(octopus_id=self.member.identity.device_id, osd_name=component.name)
            runtime = {key: value for key, value in name.items() if key != "osd_name"}
            runtime["logical_address"] = component.address.logical
            self.source.system["observations"].append({"qualified_name": name, "runtime_address": runtime,
                                                       "kind": component.device_type, "resolved": True, "stale": False})
            row = {**self.source.rows[index], "name": scan_fixture.native_selector(name), "title": component.name}
            self.source.rows.append(row)
            self.selectors[component.name] = row["name"]
            self.choice_roles[component.name] = self.choice_roles[old.name]
            for original in self.source.result.records:
                if original["component"] == old.name:
                    record = {**original, "component": component.name, "gpio": component.gpio}
                    if record["direction"] == "tx" and record["opcode"] == 0x47:
                        record["frame"] = Frame(record["source"], record["destination"], 0x47,
                                                component.name.encode("ascii")).raw.hex(":")
                    records.append(record)
        self.source.components = (*originals, *added)
        scan = self.source.result
        scan.expected, scan.records = self.source.components, tuple(records)
        scan.initiators.update({c.gpio: 4 if c.direction == "front" else 0 for c in added})
        scan.owner_boots = {self.dut.identity.device_id: self.source.boot,
                            self.member.identity.device_id: self.member_boot}
        scan.owner_duts = {self.dut.identity.device_id: self.dut, self.member.identity.device_id: self.member}
        scan._testbed, scan._group = self.bed, self.source.group
        self.session.components = self.source.components

        def forbidden(*args, **kwargs):
            raise AssertionError("Headless activity owner received an HTTP request")

        self.member.get_state = self.member.get_system = self.member.get_configuration = self.member.request = forbidden
        return self.member

    def health(self):
        if not self.healthy:
            raise RuntimeError("injected CEC responder failure")

    def sync_full(self):
        self.full["activities"] = [{"activity_id": row["activity_id"], "label": row["name"],
                                     **{role: row.get(role, "").removeprefix("component:") or None
                                        for role in ("display", "source", "audio")},
                                     "switch_device": (row.get("switch") or {}).get("device", "").removeprefix("component:") or None,
                                     "switch_input": (row.get("switch") or {}).get("input", 0)}
                                    for row in self.rows]

    def editor(self):
        choices = [{"value": self.selectors[component.name], "roles": self.choice_roles[component.name]}
                   for component in self.source.components]
        return {"version": 1, "expected_config_revision": self.full["config_revision"],
                "expected_presentation_revision": self.full["presentation_revision"],
                "activities": deepcopy(self.rows), "device_choices": choices}

    def state(self):
        if self.pending_stop:
            self.pending_stop -= 1
            if not self.pending_stop:
                self.current = None
        state = deepcopy(self.source.result.status)
        state.update(expected_config_revision=self.full["config_revision"],
                     expected_presentation_revision=self.full["presentation_revision"],
                     expected_state_revision=str(self.state_revision),
                     current_activity=next((index for index, row in enumerate(self.rows)
                                            if row["activity_id"] == self.current), -1),
                     activity_settling=bool(self.pending_stop), activity_error="")
        state["expected_boot_id"] = self.source.system["boot_id"]
        state["devices"] = [{**row, "name": self.selectors[component.name]}
                            for component, row in zip(self.source.components, self.source.rows)]
        return state

    def wait_for(self, predicate, **kwargs):
        for _ in range(8):
            self.polls += 1
            state = self.state()
            if predicate(state):
                return state
        raise TimeoutError("scripted UI state did not settle")

    def bump(self):
        self.full["config_revision"] = str(int(self.full["config_revision"]) + 1)

    def allocate(self, rows):
        for index, component in enumerate(self.source.components):
            native = self.source.rows[index]["name"]
            if any(native in [row.get(role) for role in ("display", "source", "audio")]
                   or (row.get("switch") or {}).get("device") == native for row in rows):
                identity = f"{index + 11:032x}"
                if not any(record["component_id"] == identity for record in self.full["components"]):
                    self.full["components"].append({"component_id": identity, "label": component.name,
                                                     "qualified_names": [deepcopy(self.source.system["observations"][index]["qualified_name"])]})
                self.selectors[component.name] = "component:" + identity
                for row in rows:
                    for role in ("display", "source", "audio"):
                        if row.get(role) == native:
                            row[role] = self.selectors[component.name]
                    if (row.get("switch") or {}).get("device") == native:
                        row["switch"]["device"] = self.selectors[component.name]

    def request(self, path, data=None, **kwargs):
        self.calls.append((path, deepcopy(data)))
        if data is None:
            if path == "/api/config":
                return deepcopy(self.full)
            if path.startswith("/api/operations/"):
                identity = path.rsplit("/", 1)[1]
                if identity not in self.operations:
                    raise AmityHTTPError(404, "Unknown operation")
                return deepcopy(self.operations[identity])
            raise AssertionError(path)
        if data["expected_config_revision"] != self.full["config_revision"]:
            raise AmityHTTPError(409, "stale revision")
        if path == "/api/activity-editor":
            rows = deepcopy(data["activities"])
            creating = any("activity_id" not in row for row in rows)
            if self.conflict_cleanup and not creating:
                self.conflict_cleanup = False
                self.rows.append({"activity_id": "e" * 32, "name": "Concurrent addition", "display": "", "source": "", "audio": ""})
                self.bump()
                self.sync_full()
                raise AmityHTTPError(409, "concurrent edit")
            self.allocate(rows)
            for row in rows:
                if "activity_id" not in row:
                    row["activity_id"] = f"{self.next_activity_number:032x}"
                    self.next_activity_number += 1
            self.rows = rows
            self.bump()
            self.sync_full()
            if creating and self.reject_create:
                self.rows[-1]["activity_id"] = "e" * 32
                self.sync_full()
                raise AmityHTTPError(409, "another client created this name")
            result = {"operation_id": data["request_id"], "stage": "applied"}
            self.operations[data["request_id"]] = result
            if creating and self.lose_create_reply:
                self.lose_create_reply = False
                raise OSError("HTTP reply lost after accepted creation")
            return result
        if path == "/api/activity-editor/activity":
            if data["activity_id"] is not None and self.reject_start:
                raise AmityHTTPError(409, "start rejected before admission")
            self.state_revision += 1
            if data["activity_id"] is None:
                self.pending_stop = 3  # 'complete' below only acknowledges standby admission.
            else:
                self.current = data["activity_id"]
            if self.change_boot_on_action:
                self.source.system["boot_id"] = "d" * 32
            result = {"operation_id": data["request_id"], "stage": "complete"}
            self.operations[data["request_id"]] = result
            return result
        if path == "/api/config":
            self.full = deepcopy(data["configuration"])
            self.bump()
            result = {"operation_id": data["request_id"], "stage": "applied"}
            self.operations[data["request_id"]] = result
            return result
        raise AssertionError(path)

    def activity(self, name="Temporary test", **overrides):
        return self.dut.create_activity(name, **{"display": self.tv, "source": self.player,
                                                "audio": self.avr, **overrides})


class ActivityTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()

    def test_create_start_standby_and_remove_preserve_original_state(self):
        f = self.fixture
        original_rows = deepcopy(f.rows)
        with f.activity() as activity:
            self.assertEqual(f.rows[:-1], original_rows)
            started = activity.start()
            self.assertIs(started.dut, f.dut)
            self.assertIs(started.session, f.session)
            self.assertEqual(started.boot, f.dut.last_scan._boot)
            self.assertEqual(started.initiators, f.dut.last_scan.initiators)
            self.assertGreater(started.started_ns, 0)
            before = f.polls
            activity.standby()
            self.assertGreaterEqual(f.polls - before, 3)
        self.assertEqual(f.rows, original_rows)
        self.assertEqual([record["component_id"] for record in f.full["components"]], ["f" * 32])
        self.assertEqual(len([call for call in f.calls if call[0].endswith("/activity")]), 2)
        calls = len(f.calls)
        f.callbacks[0]()
        self.assertEqual(len(f.calls), calls)

    def test_cleanup_merges_unrelated_concurrent_activity_edits(self):
        f = self.fixture
        activity = f.activity()
        f.rows[0]["name"] = "User renamed activity"
        f.conflict_cleanup = True
        activity.close()
        self.assertEqual([row["name"] for row in f.rows], ["User renamed activity", "Concurrent addition"])

    def test_rejected_create_never_adopts_foreign_same_name_row(self):
        f = self.fixture
        f.reject_create = True
        with self.assertRaises(AmityHTTPError):
            f.activity()
        rows, components = deepcopy(f.rows), deepcopy(f.full["components"])
        f.callbacks[0]()
        self.assertEqual(f.rows, rows)
        self.assertEqual(f.full["components"], components)

    def test_lost_create_reply_recovers_original_operation_before_cleanup(self):
        f = self.fixture
        f.lose_create_reply = True
        with self.assertRaises(OSError):
            f.activity()
        f.callbacks[0]()
        self.assertEqual([row["name"] for row in f.rows], ["Existing activity"])
        self.assertTrue(any(path.startswith("/api/operations/") for path, _ in f.calls))

    def test_failed_cec_responder_does_not_block_http_cleanup(self):
        f = self.fixture
        activity = f.activity()
        activity.start()
        f.healthy = False
        activity.close()
        self.assertEqual(len(f.rows), 1)
        self.assertIsNone(f.current)

    def test_cleanup_preserves_new_equipment_reused_by_another_activity(self):
        f = self.fixture
        activity = f.activity()
        identity = f.selectors[f.player.name].removeprefix("component:")
        f.rows[0]["source"] = "component:" + identity
        f.sync_full()
        activity.close()
        self.assertEqual([record["component_id"] for record in f.full["components"]], ["f" * 32, identity])
        self.assertEqual(activity.retained_equipment, [identity])

    def test_rejected_start_does_not_require_a_nonexistent_operation_for_cleanup(self):
        f = self.fixture
        activity = f.activity()
        f.reject_start = True
        with self.assertRaises(AmityHTTPError):
            activity.start()
        activity.close()
        self.assertEqual(len(f.rows), 1)
        self.assertFalse(any(path.startswith("/api/operations/") for path, _ in f.calls))

    def test_cleanup_preserves_concurrently_edited_fixture_equipment(self):
        f = self.fixture
        activity = f.activity()
        record = next(record for record in f.full["components"] if record["label"] == f.tv.name)
        record["label"] = "User changed label"
        activity.close()
        self.assertEqual([record["label"] for record in f.full["components"]], ["Preexisting equipment", "User changed label"])

    def test_unrelated_playback_blocks_cleanup_writes_and_standby(self):
        f = self.fixture
        activity = f.activity()
        activity.start()
        f.current = "9" * 32
        mutations = len([data for _, data in f.calls if data is not None])
        with self.assertRaises(ActivityCleanupError):
            activity.close()
        self.assertEqual(len([data for _, data in f.calls if data is not None]), mutations)
        self.assertEqual(f.current, "9" * 32)
        self.assertTrue(any(row["activity_id"] == activity.activity_id for row in f.rows))

    def test_creation_requires_standby_and_fresh_scan_boot(self):
        f = self.fixture
        f.current = "9" * 32
        with self.assertRaises(ActivityConflict):
            f.activity()
        self.assertFalse(any(data is not None for _, data in f.calls))
        f.current = None
        f.source.system["boot_id"] = "d" * 32
        with self.assertRaises(AssertionError):
            f.activity()
        self.assertFalse(any(data is not None for _, data in f.calls))

    def test_action_rejects_reboot_before_claiming_completion(self):
        f = self.fixture
        activity = f.activity()
        f.change_boot_on_action = True
        with self.assertRaisesRegex(AssertionError, "restarted"):
            activity.start()

    def test_assert_active_uses_current_row_after_another_activity_was_created(self):
        f = self.fixture
        with f.activity() as first, f.activity("Another saved activity") as second:
            first.start()
            # An unrelated presentation reorder must not turn a saved row index
            # into the identity of the activity being asserted.
            f.rows[:] = f.rows[1:] + f.rows[:1]
            f.sync_full()
            mutations = len([data for _, data in f.calls if data is not None])
            self.assertIs(first.assert_active(), first)
            self.assertEqual(len([data for _, data in f.calls if data is not None]), mutations)
            record = f.dut.activity_operations[-1]
            self.assertEqual(record["phase"], "assert_active")
            self.assertEqual(record["activity_id"], first.activity_id)
            self.assertEqual(record["state"]["expected_boot_id"], first.boot)
            self.assertEqual(record["state"]["current_activity"], 0)
            with self.assertRaisesRegex(AssertionError, "not the current"):
                second.assert_active()
            first.standby()

    def test_assert_active_rejects_missing_duplicate_or_changed_saved_selection(self):
        for changed in ("missing", "duplicate", "selector"):
            with self.subTest(changed=changed):
                f = Fixture()
                activity = f.activity()
                activity.start()
                row = next(row for row in f.rows if row["activity_id"] == activity.activity_id)
                if changed == "missing":
                    f.rows.remove(row)
                elif changed == "duplicate":
                    f.rows.append(deepcopy(row))
                else:
                    row["source"] = f.selectors[f.tv.name]
                with self.assertRaises(AssertionError):
                    activity.assert_active()
                self.assertEqual(f.dut.activity_operations[-1]["error_type"], "AssertionError")

    def test_assert_active_rejects_unsettled_failed_or_malformed_state(self):
        for field, value in (("activity_settling", True), ("activity_error", "owner execution failed"),
                             ("current_activity", None), ("current_activity", True)):
            with self.subTest(field=field, value=value):
                f = Fixture()
                activity = f.activity()
                activity.start()
                state = f.state
                f.dut.get_state = lambda: {**state(), field: value}
                with self.assertRaises(AssertionError):
                    activity.assert_active()

    def test_assert_active_rejects_reboot_config_read_race_and_closed_activity(self):
        f = self.fixture
        activity = f.activity()
        activity.start()
        f.source.system["boot_id"] = "d" * 32
        with self.assertRaisesRegex(AssertionError, "restarted"):
            activity.assert_active()
        f.source.system["boot_id"] = activity.boot
        state = f.state
        f.dut.get_state = lambda: {**state(), "expected_config_revision": "999"}
        with self.assertRaises(ActivityConflict):
            activity.assert_active()
        f.dut.get_state = state
        activity.standby()
        activity.close()
        with self.assertRaises(RuntimeError):
            activity.assert_active()

    def test_switch_with_non_cec_source_uses_nested_ui_selection_and_owned_cleanup(self):
        f = self.fixture
        native_switch = f.selectors[f.avr.name]
        original_rows = deepcopy(f.rows)
        with f.activity(source=None, switch_device=f.avr, switch_input=3) as activity:
            create = next(data for path, data in f.calls if path == "/api/activity-editor" and data)
            submitted = create["activities"][-1]
            self.assertEqual(submitted["source"], "")
            self.assertEqual(submitted["switch"], {"device": native_switch, "input": 3})
            self.assertNotIn("switch_device", submitted)
            self.assertNotIn("switch_input", submitted)
            persisted = f.rows[-1]
            self.assertEqual(persisted["switch"], {"device": f.selectors[f.avr.name], "input": 3})
            self.assertEqual(persisted["audio"], persisted["switch"]["device"])
            full = next(row for row in f.full["activities"] if row["activity_id"] == activity.activity_id)
            self.assertEqual(full["switch_device"], persisted["switch"]["device"].removeprefix("component:"))
            self.assertEqual(full["switch_input"], 3)
            activity.start()
            self.assertIs(activity.assert_active(), activity)
        self.assertEqual(f.rows, original_rows)
        self.assertEqual([record["component_id"] for record in f.full["components"]], ["f" * 32])
        self.assertFalse(activity.retained_equipment)

    def test_switch_input_accepts_uint8_endpoints(self):
        for value in (0, 255):
            with self.subTest(value=value):
                f = Fixture()
                with f.activity(source=None, audio=None, switch_device=f.avr, switch_input=value):
                    self.assertEqual(f.rows[-1]["switch"]["input"], value)

    def test_invalid_switch_inputs_fail_before_any_ui_mutation(self):
        for value in (-1, 256, True, False, 3.0, "3", None):
            with self.subTest(value=value):
                f = Fixture()
                with self.assertRaisesRegex(ValueError, "Switch input"):
                    f.activity(switch_device=f.avr, switch_input=value)
                self.assertFalse(f.calls)
                self.assertFalse(f.callbacks)
        f = self.fixture
        with self.assertRaisesRegex(ValueError, "requires a switch device"):
            f.activity(switch_input=3)
        self.assertFalse(f.calls)

    def test_switch_requires_current_ui_role_and_fresh_scanned_fixture(self):
        for failure in ("role", "evidence", "foreign"):
            with self.subTest(failure=failure):
                f = Fixture()
                switch = f.avr
                if failure == "role":
                    f.choice_roles[f.avr.name] = ["audio"]
                elif failure == "evidence":
                    f.dut.last_scan.records = tuple(record for record in f.dut.last_scan.records
                                                    if record["component"] != f.avr.name)
                else:
                    switch = Fixture().avr
                with self.assertRaises((ValueError, AssertionError)):
                    f.activity(audio=None, switch_device=switch, switch_input=3)
                self.assertFalse(any(data is not None for _, data in f.calls))

    def test_changed_switch_selection_or_input_rejected_before_start_and_assert_active(self):
        for phase in ("start", "assert_active"):
            for change in ("input", "boolean", "device", "missing"):
                with self.subTest(phase=phase, change=change):
                    f = Fixture()
                    activity = f.activity(source=None, switch_device=f.avr, switch_input=3)
                    if phase == "assert_active":
                        activity.start()
                    row = next(row for row in f.rows if row["activity_id"] == activity.activity_id)
                    if change == "input":
                        row["switch"]["input"] = 4
                    elif change == "boolean":
                        row["switch"]["input"] = True
                    elif change == "device":
                        row["switch"]["device"] = f.selectors[f.tv.name]
                    else:
                        del row["switch"]
                    mutations = len([data for _, data in f.calls if data is not None])
                    with self.assertRaisesRegex(AssertionError, "equipment changed"):
                        getattr(activity, phase)()
                    self.assertEqual(len([data for _, data in f.calls if data is not None]), mutations)

    def test_concurrently_added_switch_is_not_ignored_for_an_unswitched_activity(self):
        f = self.fixture
        activity = f.activity()
        f.rows[-1]["switch"] = {"device": f.selectors[f.avr.name], "input": 3}
        with self.assertRaisesRegex(AssertionError, "equipment changed"):
            activity.start()

    def test_switch_only_equipment_is_pruned_or_retained_if_reused(self):
        for reused in (False, True):
            with self.subTest(reused=reused):
                f = Fixture()
                activity = f.activity(source=None, audio=None, switch_device=f.avr, switch_input=3)
                identity = f.selectors[f.avr.name].removeprefix("component:")
                self.assertIn(identity, activity._new_equipment)
                if reused:
                    f.rows[0]["switch"] = {"device": "component:" + identity, "input": 7}
                    f.sync_full()
                activity.close()
                expected = ["f" * 32, identity] if reused else ["f" * 32]
                self.assertEqual([record["component_id"] for record in f.full["components"]], expected)
                self.assertEqual(activity.retained_equipment, [identity] if reused else [])
                if reused:
                    self.assertEqual(f.rows[0]["switch"], {"device": "component:" + identity, "input": 7})

    def test_lost_switch_creation_reply_recovers_and_removes_only_owned_equipment(self):
        f = self.fixture
        f.lose_create_reply = True
        with self.assertRaises(OSError):
            f.activity(source=None, audio=None, switch_device=f.avr, switch_input=3)
        f.callbacks[0]()
        self.assertEqual([row["name"] for row in f.rows], ["Existing activity"])
        self.assertEqual([record["component_id"] for record in f.full["components"]], ["f" * 32])

    def test_member_activity_uses_main_ui_and_carries_both_owner_boots(self):
        f = self.fixture
        f.add_member()
        original = deepcopy(f.rows)
        with f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr) as activity:
            started = activity.start()
            self.assertIs(started.dut, f.dut)
            self.assertIs(started.scan, f.dut.last_scan)
            self.assertIs(started.testbed, f.bed)
            self.assertEqual(started.owner_boots, f.dut.last_scan.owner_boots)
            self.assertIs(started.owner_duts[f.member.identity.device_id], f.member)
            self.assertIs(activity.assert_active(), activity)
            stopped = activity.standby()
            self.assertEqual(stopped.owner_boots, started.owner_boots)
        self.assertEqual(f.rows, original)
        self.assertEqual([r["component_id"] for r in f.full["components"]], ["f" * 32])
        self.assertEqual(len([r for r in f.dut.activity_operations if r["phase"] == "start"]), 1)

    def test_mixed_owner_activity_matches_actual_owner_selectors(self):
        f = self.fixture
        f.add_member()
        with f.activity(display=f.tv, source=f.member_player, audio=f.member_avr) as activity:
            records = {record["component_id"]: record for record in f.full["components"]}
            row = f.rows[-1]
            for role, owner in (("display", f.dut), ("source", f.member), ("audio", f.member)):
                selector = records[row[role].removeprefix("component:")]["qualified_names"][0]
                self.assertEqual(selector["octopus_id"], owner.identity.device_id)
            activity.start()
            activity.assert_active()

    def test_member_boot_or_membership_changes_reject_activity_without_mutating_main(self):
        for phase in ("create", "start", "assert_active"):
            for field, value in (("boot_id", "e" * 32), ("ready", False), ("enrolled", False)):
                with self.subTest(phase=phase, field=field):
                    f = Fixture()
                    f.add_member()
                    if phase != "create":
                        activity = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
                        if phase == "assert_active":
                            activity.start()
                    f.source.system["membership"]["peers"][0][field] = value
                    mutations = len([data for _, data in f.calls if data is not None])
                    with self.assertRaises(AssertionError):
                        if phase == "create":
                            f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
                        else:
                            getattr(activity, phase)()
                    self.assertEqual(len([data for _, data in f.calls if data is not None]), mutations)

    def test_foreign_member_or_session_has_no_cross_owner_activity_permission(self):
        for failure in ("proof", "bed", "session", "owner_object"):
            with self.subTest(failure=failure):
                f = Fixture()
                f.add_member()
                if failure == "proof":
                    f.bed._member_proofs.clear()
                elif failure == "bed":
                    f.member._testbed = SimpleNamespace()
                elif failure == "session":
                    f.member_player._session = object()
                else:
                    f.dut.last_scan.owner_duts[f.member.identity.device_id] = DUT("amity-dut-2")
                with self.assertRaises((ValueError, AssertionError)):
                    f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
                self.assertFalse(any(data is not None for _, data in f.calls))

    def test_other_owner_fresh_ui_cannot_replace_member_wire_discovery(self):
        for mutation in ({"transport": "virtual"}, {"wire_timestamp_ns": 1}):
            with self.subTest(mutation=mutation):
                f = Fixture()
                f.add_member()
                remote_pins = {c.gpio for c in (f.member_tv, f.member_avr, f.member_player)}
                f.dut.last_scan.records = tuple({**record, **mutation} if record["gpio"] in remote_pins else record
                                                for record in f.dut.last_scan.records)
                with self.assertRaisesRegex(AssertionError, "fresh physical"):
                    f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
                self.assertFalse(any(data is not None for _, data in f.calls))

    def test_member_reboot_while_start_settles_never_returns_an_authorized_boundary(self):
        f = self.fixture
        f.add_member()
        activity = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
        wait = f.dut.wait_for

        def reboot_after_settle(*args, **kwargs):
            state = wait(*args, **kwargs)
            f.source.system["membership"]["peers"][0]["boot_id"] = "e" * 32
            return state

        f.dut.wait_for = reboot_after_settle
        with self.assertRaisesRegex(AssertionError, "changed boot"):
            activity.start()


if __name__ == "__main__":
    unittest.main()
