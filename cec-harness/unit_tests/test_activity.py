"""Temporary activity ownership uses existing UI transactions and safe cleanup."""

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.activity import Activity, ActivityCleanupError, ActivityConflict
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
        self.current, self.pending_stop, self.state_revision, self.polls = None, 0, 1, 0
        self.reject_create = self.lose_create_reply = self.conflict_cleanup = self.change_boot_on_action = self.reject_start = False
        self.dut.get_configuration, self.dut.get_state = self.editor, self.state
        self.dut.request, self.dut.wait_for = self.request, self.wait_for
        self.dut.wait_operation = lambda result, **kwargs: self.operations[result["operation_id"]]
        self.dut.get_system = lambda: deepcopy(source.system)
        self.sync_full()

    def health(self):
        if not self.healthy:
            raise RuntimeError("injected CEC responder failure")

    def sync_full(self):
        self.full["activities"] = [{"activity_id": row["activity_id"], "label": row["name"],
                                     **{role: row.get(role, "").removeprefix("component:") or None
                                        for role in ("display", "source", "audio")}}
                                    for row in self.rows]

    def editor(self):
        choices = [{"value": self.selectors[component.name], "roles": roles}
                   for component, roles in ((self.tv, ["display", "audio"]), (self.avr, ["audio", "switch"]),
                                            (self.player, ["source"]))]
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
                     activity_settling=bool(self.pending_stop))
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
            if any(row.get(role) == native for row in rows for role in ("display", "source", "audio")):
                identity = f"{index + 11:032x}"
                if not any(record["component_id"] == identity for record in self.full["components"]):
                    self.full["components"].append({"component_id": identity, "label": component.name,
                                                     "qualified_names": [deepcopy(self.source.system["observations"][index]["qualified_name"])]})
                self.selectors[component.name] = "component:" + identity
                for row in rows:
                    for role in ("display", "source", "audio"):
                        if row.get(role) == native:
                            row[role] = self.selectors[component.name]

    def request(self, path, data=None, **kwargs):
        self.calls.append((path, deepcopy(data)))
        if data is None:
            if path == "/api/v2/config":
                return deepcopy(self.full)
            if path.startswith("/api/v2/operations/"):
                identity = path.rsplit("/", 1)[1]
                if identity not in self.operations:
                    raise AmityHTTPError(404, "Unknown operation")
                return deepcopy(self.operations[identity])
            raise AssertionError(path)
        if data["expected_config_revision"] != self.full["config_revision"]:
            raise AmityHTTPError(409, "stale revision")
        if path == "/api/v2/activity-editor":
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
                row.setdefault("activity_id", "a" * 32)
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
        if path == "/api/v2/activity-editor/activity":
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
        if path == "/api/v2/config":
            self.full = deepcopy(data["configuration"])
            self.bump()
            result = {"operation_id": data["request_id"], "stage": "applied"}
            self.operations[data["request_id"]] = result
            return result
        raise AssertionError(path)

    def activity(self):
        return self.dut.create_activity("Temporary test", display=self.tv, source=self.player, audio=self.avr)


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
        self.assertTrue(any(path.startswith("/api/v2/operations/") for path, _ in f.calls))

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
        self.assertFalse(any(path.startswith("/api/v2/operations/") for path, _ in f.calls))

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


if __name__ == "__main__":
    unittest.main()
