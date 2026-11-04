"""Activity editing evidence must preserve the running session and saved rows."""

from copy import deepcopy
from contextlib import contextmanager
from pathlib import Path
from types import SimpleNamespace
import sys
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.activity import ActivityConflict
from amity_harness import AvrComponent, CecSession, DUT, Frame, Hooks, PlaybackComponent, TvComponent
from amity_harness.activity_edits import ActiveActivityEdits, ActivityEditObservation
from amity_harness.dut import AmityHTTPError
from test_activity import Fixture
from test_activity_handoff import IdleWire


class EditingFixture(Fixture):
    def __init__(self, *, remote=False):
        self.accept_active = self.admit_rejected = self.wrong_reason = False
        self.after_save = self.after_rejection = None
        self.activity_generation = 9
        super().__init__()
        if remote:
            self.add_member()
        self.other = self.activity("Inactive numeric input", source=None, switch_device=self.avr, switch_input=4)
        self.active = (self.activity("Current playback", display=self.member_tv, audio=self.member_avr,
                                     source=self.member_player) if remote else self.activity("Current playback"))
        self.active.start()

    def sync_full(self):
        super().sync_full()
        for row in self.full["activities"]:
            row.update(zone_id=self.full["primary_zone_id"], generated_label=row["label"], label_override=True)
        self.full["activity_order"] = [row["activity_id"] for row in self.full["activities"]]
        self.full["zones"] = [{"zone_id": self.full["primary_zone_id"],
                               "activity_order": list(self.full["activity_order"])}]
        self.full["activities"].sort(key=lambda row: row["activity_id"])

    def state(self):
        state = super().state()
        state["activity_generation"] = self.activity_generation
        return state

    def request(self, path, data=None, **kwargs):
        editing = data is not None and path == "/api/activity-editor" and self.current is not None
        if editing:
            before = next(row for row in self.rows if row["activity_id"] == self.current)
            proposed = [row for row in data["activities"] if row.get("activity_id") == self.current]
            if proposed != [before] and not self.accept_active:
                self.calls.append((path, deepcopy(data)))
                if self.admit_rejected:
                    self.operations[data["request_id"]] = {"operation_id": data["request_id"], "stage": "rejected"}
                if self.after_rejection:
                    self.after_rejection(self)
                raise AmityHTTPError(409, "Stop this activity before editing or deleting it.",
                                     code="stale_revision" if self.wrong_reason else "active_activity")
        response = super().request(path, data, **kwargs)
        if editing and self.after_save:
            self.after_save(self)
        return response


class ActivityEditsTests(unittest.TestCase):
    def test_rejects_current_edit_delete_then_saves_inactive_input_without_reselection(self):
        fixture = EditingFixture()
        before = deepcopy(fixture.full)
        previous_index = fixture.state()["current_activity"]
        state_revision, generation = fixture.state_revision, fixture.activity_generation
        edits = ActiveActivityEdits(fixture.active)
        edits.assert_active_rejected().assert_active_rejected(delete=True)
        self.assertEqual(fixture.full, before)
        edits.edit_other(fixture.other, switch_input=3)
        self.assertGreater(previous_index, 0)
        self.assertEqual(fixture.state()["current_activity"], 0)
        self.assertEqual(fixture.current, fixture.active.activity_id)
        self.assertEqual((fixture.state_revision, fixture.activity_generation), (state_revision, generation))
        fixture.active.assert_active()
        self.assertEqual(fixture.other.switch_input, 3)
        self.assertEqual(fixture.other._created_row["switch"]["input"], 3)
        self.assertEqual([data["activity_id"] for path, data in fixture.calls
                          if path == "/api/activity-editor/activity"], [fixture.active.activity_id])
        # The accepted row remains usable by ordinary start/cleanup helpers.
        fixture.active.standby()
        fixture.other.start()
        fixture.other.assert_active()
        fixture.other.standby()
        fixture.other.close()
        fixture.active.close()
        self.assertEqual([row["name"] for row in fixture.rows], ["Existing activity"])

    def test_local_edit_preserves_verified_remote_activity_and_its_receivers(self):
        fixture = EditingFixture(remote=True)
        recipients = dict(fixture.active.components)
        scan = fixture.dut.last_scan
        ActiveActivityEdits(fixture.active).assert_active_rejected().assert_active_rejected(delete=True).edit_other(
            fixture.other, switch_input=3)
        fixture.active.assert_active()
        self.assertEqual(fixture.active.components, recipients)
        self.assertIs(fixture.dut.last_scan, scan)
        self.assertTrue(all(component is None or component.dut is fixture.member
                            for component in fixture.active.components.values()))
        self.assertEqual(fixture.state()["current_activity"], 0)
        self.assertEqual(fixture.other.switch_input, 3)

    def test_canonical_activity_records_stay_sorted_when_presentation_order_changes(self):
        fixture = EditingFixture()
        canonical = sorted(row["activity_id"] for row in fixture.full["activities"])
        self.assertLess(fixture.other.activity_id, fixture.active.activity_id)
        self.assertEqual([row["activity_id"] for row in fixture.full["activities"]], canonical)
        ActiveActivityEdits(fixture.active).edit_other(fixture.other, switch_input=3)
        self.assertEqual([row["activity_id"] for row in fixture.full["activities"]], canonical)
        self.assertEqual(fixture.full["activity_order"][0], fixture.active.activity_id)
        self.assertEqual(fixture.full["zones"][0]["activity_order"], fixture.full["activity_order"])
        self.assertNotEqual(canonical, fixture.full["activity_order"])
        self.assertEqual(next(row for row in fixture.full["activities"]
                              if row["activity_id"] == fixture.other.activity_id)["switch_input"], 3)

    def test_accepted_active_edit_wrong_reason_or_ledger_admission_cannot_pass(self):
        for fault in ("accept_active", "wrong_reason", "admit_rejected"):
            for delete in (False, True):
                with self.subTest(fault=fault, delete=delete):
                    fixture = EditingFixture()
                    setattr(fixture, fault, True)
                    with self.assertRaises(AssertionError):
                        ActiveActivityEdits(fixture.active).assert_active_rejected(delete=delete)

    def test_rejection_must_leave_saved_configuration_unchanged(self):
        fixture = EditingFixture()

        def mutation(current):
            current.rows[0]["name"] = "Unexpected other edit"
            current.sync_full()
            current.bump()

        fixture.after_rejection = mutation
        with self.assertRaisesRegex(AssertionError, "rejected active edit changed"):
            ActiveActivityEdits(fixture.active).assert_active_rejected()

    def test_inactive_save_cannot_change_selection_revision_generation_or_cached_devices(self):
        faults = {
            "selection": lambda f: setattr(f, "current", f.other.activity_id),
            "state revision": lambda f: setattr(f, "state_revision", f.state_revision + 1),
            "activity generation": lambda f: setattr(f, "activity_generation", f.activity_generation + 1),
            "discovery generation": lambda f: f.source.result.status.update(discovery_generation=99),
            "cached identity": lambda f: f.source.rows[0].update(physical_address=0x3000),
            "switch input": lambda f: f.full["activities"][-1].update(switch_input=8),
            "equipment": lambda f: f.full["components"][0].update(label="Unexpected equipment change"),
            "home slot": lambda f: f.full["external_slots"][0].update(alias="Unexpected shortcut change"),
            "activity order": lambda f: f.full["activity_order"].reverse(),
            "zone order": lambda f: f.full["zones"][0]["activity_order"].reverse(),
        }
        for name, mutation in faults.items():
            with self.subTest(fault=name):
                fixture = EditingFixture()
                old_row = deepcopy(fixture.other._created_row)
                fixture.after_save = mutation
                with self.assertRaises((AssertionError, ActivityConflict)):
                    ActiveActivityEdits(fixture.active).edit_other(fixture.other, switch_input=3)
                self.assertEqual(fixture.other._created_row, old_row)
                self.assertEqual(fixture.other.switch_input, 4)

    def test_edit_requires_exact_current_and_inactive_fixture_rows(self):
        for fault in ("standby", "changed current", "changed other", "same activity"):
            with self.subTest(fault=fault):
                fixture = EditingFixture()
                if fault == "standby":
                    fixture.current = None
                elif fault == "changed current":
                    next(row for row in fixture.rows if row["activity_id"] == fixture.active.activity_id)["name"] = "Changed"
                    fixture.sync_full()
                elif fault == "changed other":
                    next(row for row in fixture.rows if row["activity_id"] == fixture.other.activity_id)["switch"]["input"] = 9
                    fixture.sync_full()
                with self.assertRaises(ActivityConflict):
                    edits = ActiveActivityEdits(fixture.active)
                    edits.edit_other(fixture.active if fault == "same activity" else fixture.other, switch_input=3)


class ActivityEditWireTests(unittest.TestCase):
    @contextmanager
    def fixture(self):
        dut = DUT("amity-dut")
        connector = dut.tentacle(0)
        tv = TvComponent(connector.front, "Edit TV")
        avr = AvrComponent(connector.back, "Edit AVR")
        player = PlaybackComponent(avr, "Edit Player", port=1)
        components = (tv, avr, player)
        with CecSession(components, backend_factory=IdleWire) as session:
            initiators = {tv.gpio: 4, avr.gpio: 0}
            dut.last_scan = SimpleNamespace(initiators=initiators, _session=session)
            for component in components:
                component._power_timeout = .001
            with ActivityEditObservation(*components, keys=(0x00, 0x42)) as observation:
                yield observation, components, session, initiators

    def receive(self, fixture, component, opcode, operands=b"", *, transport="wire", source=None, timestamp=None):
        observation, components, session, initiators = fixture
        frame = Frame(initiators[component.gpio] if source is None else source,
                      15 if opcode in (0x80, 0x81, 0x82, 0x86) else component.address.logical,
                      opcode, operands, timestamp_ns=time.monotonic_ns() if timestamp is None else timestamp)
        component.receive(frame, transport=transport)
        session._wire_idle_since_ns = {gpio: time.monotonic_ns() + 10**12 for gpio in initiators}

    def test_explicit_keys_can_arrive_before_preservation_coverage_completes(self):
        with self.fixture() as fixture:
            observation, (_, avr, player), _, _ = fixture
            for component, key in ((player, 0x00), (avr, 0x42)):
                self.receive(fixture, component, 0x44, bytes([key]))
                self.receive(fixture, component, 0x45)
            self.assertIs(observation.assert_preserved(), observation)
            self.assertEqual(avr.routing_assertions[-1]["kind"], "wire_activity_edit_preserved")
            self.assertTrue(avr.routing_assertions[-1]["idle_receive_since_ns"])

    def test_power_routing_input_selection_and_unrequested_keys_fail_from_original_frames(self):
        faults = ((0x36, b""), (0x04, b""), (0x44, b"\x6d"),
                  (0x86, b"\x11\x00"), (0x44, b"\x69\x03"),
                  (0x44, b"\x04"), (0x45, b"\x00"))
        for opcode, operands in faults:
            for rewrite in (False, True):
                with self.subTest(opcode=opcode, operands=operands, rewrite=rewrite):
                    with self.assertRaisesRegex(AssertionError, "power, routing or an unrequested key"):
                        with self.fixture() as fixture:
                            avr = fixture[1][1]
                            avr.hooks = Hooks(before_rx=lambda component, frame:
                                              Frame(frame.source, frame.destination, 0x8f) if rewrite else None)
                            self.receive(fixture, avr, opcode, operands)
                            fixture[0].assert_preserved()

    def test_virtual_wrong_sender_and_old_frames_do_not_imply_new_dut_side_effects(self):
        with self.fixture() as fixture:
            observation, (_, avr, _), _, _ = fixture
            self.receive(fixture, avr, 0x36, transport="virtual")
            self.receive(fixture, avr, 0x36, source=8)
            self.receive(fixture, avr, 0x36, timestamp=observation._started - 1)
            observation.assert_preserved()

    def test_missing_physical_timestamp_fails(self):
        with self.assertRaisesRegex(AssertionError, "without a wire timestamp"):
            with self.fixture() as fixture:
                self.receive(fixture, fixture[1][1], 0x44, b"\x00", timestamp=0)
                fixture[0].assert_preserved()


if __name__ == "__main__":
    unittest.main()
