"""A pending UI selection preserves its admission boundary until explicitly awaited."""

from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.activity import ActivityConflict
from amity_harness.discovery import _fresh_discovery
from amity_harness.dut import AmityHTTPError
from test_activity import Fixture, fresh_rescan


class PendingFixture(Fixture):
    """Keep the accepted selection in flight until its operation is awaited."""

    def __init__(self, *, member=False):
        super().__init__()
        if member:
            self.add_member()
        scan = self.dut.last_scan
        scan._testbed, scan._group = self.bed, self.source.group
        scan.owner_duts[self.dut.identity.device_id] = self.dut
        scan.owner_boots[self.dut.identity.device_id] = self.source.boot
        self.in_flight = self.start_request = None
        self.waited = []
        self.operation_fault = None
        self.dut.wait_operation = self.wait_operation

    def state(self):
        state = super().state()
        state["activity_settling"] |= self.in_flight is not None
        return state

    def request(self, path, data=None, **kwargs):
        previous = self.current
        result = super().request(path, data, **kwargs)
        if path == "/api/activity-editor/activity" and data is not None:
            if data["activity_id"] is not None:
                self.current, self.in_flight = previous, data["activity_id"]
                self.start_request = data["request_id"]
                result = {**result, "stage": "accepted"}
                self.operations[self.start_request] = deepcopy(result)
            else:
                if self.start_request is not None:
                    self.operations[self.start_request]["stage"] = "cancelled"
                self.in_flight = None
        return result

    def wait_operation(self, result, **kwargs):
        self.waited.append(deepcopy(result))
        operation = deepcopy(self.operations[result["operation_id"]])
        if result["operation_id"] == self.start_request:
            if self.operation_fault is not None:
                return {**operation, **self.operation_fault}
            self.current, self.in_flight = self.in_flight, None
            operation["stage"] = "complete"
            self.operations[result["operation_id"]] = deepcopy(operation)
        return operation

    def selection_posts(self):
        return [data for path, data in self.calls
                if path == "/api/activity-editor/activity" and data is not None]


class PendingActivityTests(unittest.TestCase):
    def test_submit_does_not_wait_or_claim_in_flight_activity_current(self):
        f = PendingFixture()
        activity = f.activity()
        f.waited.clear()
        initial = f.state()
        polls = f.polls

        with activity.start_pending() as pending:
            action = pending.action()
            self.assertEqual(f.waited, [])
            self.assertEqual(f.polls, polls)
            self.assertEqual(len(f.selection_posts()), 1)
            self.assertEqual(action.operation["stage"], "accepted")
            self.assertEqual(action.state, initial)
            self.assertEqual(action.state["current_activity"], -1)
            self.assertEqual(f.state()["current_activity"], -1)
            self.assertTrue(f.state()["activity_settling"])
            with self.assertRaisesRegex(AssertionError, "not the current"):
                activity.assert_active()

            boundary = (action.started_ns, action.request_id, action.activity_id)
            initiators = dict(action.initiators)
            completed = pending.wait()
            self.assertIs(completed, action)
            self.assertEqual((action.started_ns, action.request_id, action.activity_id), boundary)
            self.assertEqual(action.initiators, initiators)
            self.assertEqual(len(f.waited), 1)
            self.assertEqual(f.waited[0]["operation_id"], action.request_id)
            self.assertEqual(action.operation["stage"], "complete")
            self.assertEqual(action.state["current_activity"], 1)
            self.assertFalse(action.state["activity_settling"])
            self.assertIs(activity.assert_active(), activity)
            polls = f.polls
            self.assertIs(pending.wait(), action)
            self.assertEqual(f.polls, polls)
            self.assertEqual(len(f.waited), 1)
            self.assertEqual(len(f.selection_posts()), 1)
        activity.close()

    def test_partial_current_scan_preserves_selected_owner_and_original_boundary(self):
        for member in (False, True):
            with self.subTest(member=member):
                f = PendingFixture(member=member)
                player = f.member_player if member else f.player
                selections = ({"display": f.member_tv, "audio": f.member_avr,
                               "source": player} if member else {})
                activity = f.activity(**selections)
                creation = activity.scan
                scan = fresh_rescan(f)
                scan.records = tuple(record for record in scan.records
                                     if not (record["component"] == player.name
                                             and record["opcode"] == 0x47))
                self.assertFalse(_fresh_discovery(scan.records, player, scan.initiators[player.gpio], scan.started))
                scan.status = {**scan.status, "devices": [], "discovering": True}
                state = f.dut.get_state

                def missing():
                    current = state()
                    current["devices"] = [row for row in current["devices"]
                                          if row["name"] != f.selectors[player.name]]
                    return current

                f.dut.get_state = missing
                saved = deepcopy(f.full)
                f.waited.clear()
                with activity.start_pending(scan=scan) as pending:
                    action = pending.action()
                    self.assertIs(action.scan, scan)
                    self.assertIs(activity.scan, creation)
                    self.assertIs(activity._action_scan, creation)
                    self.assertEqual(action.owner_boots, scan.owner_boots)
                    self.assertIs(action.owner_duts[player.dut.identity.device_id], player.dut)
                    self.assertEqual(action.initiators, scan.initiators)
                    self.assertEqual(f.full, saved)
                    self.assertEqual(f.waited, [])
                    self.assertIs(pending.wait(), action)
                self.assertEqual(len(f.selection_posts()), 1)
                activity.close()

    def test_changed_current_scan_or_owner_is_rejected_before_submission(self):
        faults = ("not_current", "main", "bed", "session", "main_boot", "group",
                  "owner_object", "member_boot", "missing_component", "component_session",
                  "initiator", "missing_initiator", "not_started")
        for fault in faults:
            with self.subTest(fault=fault):
                f = PendingFixture(member=True)
                activity = f.activity(display=f.member_tv, source=f.member_player, audio=f.member_avr)
                scan = fresh_rescan(f)
                if fault == "not_current":
                    f.dut.last_scan = activity.scan
                elif fault == "main":
                    scan.dut = f.member
                elif fault == "bed":
                    scan._testbed = object()
                elif fault == "session":
                    scan._session = object()
                elif fault == "main_boot":
                    scan._boot = "e" * 32
                elif fault == "group":
                    scan._group = "e" * 32
                elif fault == "owner_object":
                    scan.owner_duts[f.member.identity.device_id] = object()
                elif fault == "member_boot":
                    scan.owner_boots[f.member.identity.device_id] = "e" * 32
                elif fault == "missing_component":
                    scan.expected = tuple(component for component in scan.expected if component is not f.member_player)
                elif fault == "component_session":
                    f.member_player._session = object()
                elif fault == "initiator":
                    scan.initiators[f.member_player.gpio] = 14
                elif fault == "missing_initiator":
                    scan.initiators.pop(f.member_player.gpio)
                else:
                    scan.started = 0
                with self.assertRaises(AssertionError):
                    activity.start_pending(scan=scan)
                self.assertEqual(f.selection_posts(), [])

    def test_local_pending_scan_rejects_stale_testbed(self):
        for fault in ("closed", "not_entered", "bed_session", "dut_bed"):
            with self.subTest(fault=fault):
                f = PendingFixture()
                activity = f.activity()
                scan = fresh_rescan(f)
                if fault == "closed":
                    f.bed._closed = True
                elif fault == "not_entered":
                    f.bed._entered = False
                elif fault == "bed_session":
                    f.bed.session = object()
                else:
                    f.dut._testbed = object()
                with self.assertRaises(AssertionError):
                    activity.start_pending(scan=scan)
                self.assertEqual(f.selection_posts(), [])

    def test_changed_saved_selection_is_rejected_before_submission(self):
        f = PendingFixture()
        activity = f.activity()
        f.rows[-1]["source"] = ""
        f.sync_full()
        with self.assertRaisesRegex(AssertionError, "equipment changed"):
            activity.start_pending()
        self.assertEqual(f.selection_posts(), [])

    def test_wait_rejects_changed_configuration_without_resubmitting(self):
        f = PendingFixture()
        activity = f.activity()
        pending = activity.start_pending()
        f.bump()
        with self.assertRaises(ActivityConflict):
            pending.wait()
        self.assertEqual(len(f.selection_posts()), 1)
        activity.close()

    def test_wait_requires_original_successful_operation_without_replay(self):
        for fault in ({"operation_id": "e" * 32}, {"stage": "failed"}, {"partial": True}):
            with self.subTest(fault=fault):
                f = PendingFixture()
                activity = f.activity()
                pending = activity.start_pending()
                f.operation_fault = fault
                with self.assertRaises(AssertionError):
                    pending.wait()
                self.assertEqual(len(f.selection_posts()), 1)
                f.operation_fault = None
                activity.close()

    def test_rejected_submission_is_not_replayed_or_followed_by_standby(self):
        f = PendingFixture()
        activity = f.activity()
        f.waited.clear()
        f.reject_start = True
        with self.assertRaises(AmityHTTPError):
            activity.start_pending()
        self.assertEqual(len(f.selection_posts()), 1)
        self.assertEqual(f.waited, [])
        activity.close()
        self.assertEqual(len(f.selection_posts()), 1)
        self.assertEqual([row["name"] for row in f.rows], ["Existing activity"])

    def test_lost_admission_reply_is_not_replayed_and_cleanup_cancels_original(self):
        f = PendingFixture()
        activity = f.activity()
        request = f.dut.request

        def lose_reply(path, data=None, **kwargs):
            result = request(path, data, **kwargs)
            if path == "/api/activity-editor/activity" and data is not None and data["activity_id"] is not None:
                raise OSError("accepted selection reply lost")
            return result

        f.dut.request = lose_reply
        with self.assertRaisesRegex(OSError, "reply lost"):
            activity.start_pending()
        self.assertEqual(len(f.selection_posts()), 1)
        original = f.start_request
        activity.close()
        self.assertEqual([post["activity_id"] for post in f.selection_posts()], [activity.activity_id, None])
        self.assertEqual(f.operations[original]["stage"], "cancelled")
        self.assertEqual([row["name"] for row in f.rows], ["Existing activity"])

    def test_unawaited_context_reports_error_and_activity_cleanup_cancels_pending_selection(self):
        f = PendingFixture()
        activity = f.activity()
        f.waited.clear()
        with self.assertRaisesRegex(RuntimeError, "not awaited"):
            with activity.start_pending():
                pass
        self.assertIsNotNone(f.in_flight)
        activity.close()
        self.assertIsNone(f.in_flight)
        self.assertEqual([row["name"] for row in f.rows], ["Existing activity"])
        self.assertEqual([post["activity_id"] for post in f.selection_posts()], [activity.activity_id, None])
        self.assertTrue(all(operation["operation_id"] != f.start_request for operation in f.waited))
        cancellation = next(index for index, (path, data) in enumerate(f.calls)
                            if path == "/api/activity-editor/activity" and data["activity_id"] is None)
        removal = next(index for index, (path, data) in enumerate(f.calls)
                       if index > cancellation and path == "/api/activity-editor" and data is not None)
        self.assertLess(cancellation, removal)

    def test_pending_context_does_not_mask_primary_exception(self):
        f = PendingFixture()
        activity = f.activity()
        with self.assertRaisesRegex(ValueError, "physical evidence failed"):
            with activity.start_pending():
                raise ValueError("physical evidence failed")
        activity.close()
        self.assertEqual([post["activity_id"] for post in f.selection_posts()], [activity.activity_id, None])


if __name__ == "__main__":
    unittest.main()
