"""Replacement evidence must reject changed saved intent before rebinding fixtures."""

from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness import AvrComponent, DUT, PlaybackComponent, PowerState, TvComponent
from amity_harness.membership import AddedAmity, MembershipConflict
from amity_harness.replacement import ReplacementReview, replacement_proposal
from test_removal_cascade import CascadeFixture
from unit_tests.virtual_time import use_virtual_time


class SavedActivity:
    def refresh_scan(self, scan):
        for component in self.components.values():
            if component is not None:
                scan.assert_on(component, component.endpoint)
        self._action_scan = scan


class PhysicalScan:
    """A unit-test boundary for the separately tested physical scan assertions."""

    def __init__(self, components):
        self.expected = tuple(components)
        self.locations = []

    def assert_on(self, component, endpoint):
        if component not in self.expected or component.endpoint != endpoint:
            raise AssertionError("Missing fresh physical discovery on the replacement connection")
        self.locations.append(component)


class ReplacementFixture(CascadeFixture):
    def __init__(self):
        self.apply_mutation = None
        self.lost_apply_reply = False
        self.replacement = None
        super().__init__()
        self.member.member_boot, self.member.timeout = self.member_boot, 10
        self.new_dut = DUT("amity-dut-4")
        self.new_dut._testbed = self.bed
        self.bed.duts = (*self.bed.duts, self.new_dut)
        self.bed.members = (self.candidate, self.new_dut)
        self.replacement = AddedAmity(self.main, self.new_dut, timeout=10)
        self.replacement._created = True
        self.replacement.boot, self.replacement.group = self.boot, self.group
        self.replacement.member_boot = "9" * 32
        self.bed._member_proofs = {member.device_id: member
                                   for member in (self.member, self.replacement)}
        peer = self.peer(self.replacement.device_id, self.replacement.name, self.replacement.member_boot)
        peer.update(supported_tentacles=4, configured_tentacles_mask=15)
        self.system["membership"]["peers"].append(peer)
        self.system["named_devices"].append({"octopus_id": self.replacement.device_id,
                                               "hostname": self.replacement.name})
        self.config["octopuses"].append({**self.owner(self.replacement.device_id), "supported_tentacles": 4})
        self.old = self.components(self.candidate, 0, PowerState.OFF)
        self.new = self.components(self.new_dut, 1, PowerState.STANDBY)
        self.pairs = tuple(zip(self.old, self.new))
        self.system["observations"] = [self.observation(component) for component in self.new]
        self.bed.components = self.bed.session.components = (*self.old, *self.new)
        for component in self.bed.components:
            component._session = self.bed.session
        self.resources = [self.activity(resource) for resource in self.resources]
        self.bed._activities = self.resources
        self.main.last_scan = PhysicalScan(self.new)
        self.action_scan = PhysicalScan(self.new)
        self.main.scan_hdmi = lambda: self.action_scan
        self.advance()

    def observation(self, component, *, resolved=True, tentacle=None):
        name = {"consortium_id": self.group, "octopus_id": component.dut.identity.device_id,
                "tentacle_id": component.tentacle if tentacle is None else tentacle,
                "side": component.direction, "osd_name": component.name}
        return {"title": component.name, "resolved": resolved, "stale": not resolved, "qualified_name": name}

    @staticmethod
    def components(dut, adapter, power):
        connector = dut.tentacle(adapter)
        tv = TvComponent(connector.front, "Remote display", power=power)
        avr = AvrComponent(connector.back, "Remote AVR", power=power)
        player = PlaybackComponent(avr, "Remote source", port=1, power=power)
        return tv, avr, player

    def activity(self, resource):
        activity = SavedActivity()
        activity.__dict__.update(resource.__dict__)
        tv, avr, player = self.old
        choices = {"component:" + identity * 32: component
                   for identity, component in (("4", player), ("5", tv), ("6", avr))}
        row = resource._created_row
        activity.components = {role: choices.get(row.get(role)) for role in ("source", "display", "audio")}
        activity.components["switch"] = choices.get((row.get("switch") or {}).get("device"))
        activity.scan = activity._action_scan = None
        return activity

    def expected(self, payload):
        result = deepcopy(self.config)
        result["octopuses"] = [row for row in result["octopuses"]
                               if row["octopus_id"] != payload["previous_octopus_id"]]
        # Requests name the consortium; saved names leave it to the configuration.
        for mapping in payload["equipment"]:
            component = next(row for row in result["components"]
                             if row["component_id"] == mapping["component_id"])
            if {"consortium_id": result["consortium_id"], **component["qualified_name"]} == mapping["previous"]:
                component["qualified_name"] = {key: value for key, value in mapping["replacement"].items()
                                               if key != "consortium_id"}
        return result

    def request(self, path, data=None, **kwargs):
        if self.replacement is not None and data is None and path == "/api/devices/" + self.replacement.device_id + "/settings":
            self.calls.append((path, None))
            return {"status": "ready", "error": "", "settings": {
                "boot_id": self.replacement.member_boot, "settings_revision": "1",
                "adapters": [{"tentacle_id": number, "configured": True,
                              "default_front_physical_address": "1.0.0.0"} for number in range(4)]}}
        if data is None or path != "/api/replacements/apply":
            return super().request(path, data, **kwargs)
        self.calls.append((path, deepcopy(data)))
        self.config = self.expected(data)
        self.system["membership"]["peers"] = [row for row in self.system["membership"]["peers"]
                                                  if row["octopus_id"] != self.member.device_id]
        self.system["membership"]["membership_revision"] = str(int(self.system["membership"]["membership_revision"]) + 1)
        self.system["named_devices"] = [row for row in self.system["named_devices"]
                                         if row["octopus_id"] != self.member.device_id]
        self.settings_purged = True
        self.advance()
        if self.apply_mutation:
            self.apply_mutation(self.config)
        operation = self.operation(data["request_id"], "applied", reset="scheduled")
        self.operations[data["request_id"]] = operation
        if self.lost_apply_reply:
            raise OSError("Admitted replacement response lost")
        return deepcopy(operation)

    def review(self, pairs=None):
        return ReplacementReview(self.member, self.replacement, self.resources,
                                 self.pairs if pairs is None else pairs)

    def posts(self, action):
        return [data for path, data in self.calls if data is not None and path == "/api/replacements/" + action]


def corruptions():
    return {
        "switch input": lambda config: config["activities"][-1].update(switch_input=9),
        "activity identity": lambda config: config["activities"][-1].update(activity_id="e" * 32),
        "equipment identity": lambda config: config["components"][-1].update(component_id="e" * 32),
        "role selection": lambda config: config["activities"][1].update(source="3" * 32),
        "activity order": lambda config: config["activity_order"].reverse(),
        "home slot": lambda config: config["external_slots"][-1].update(alias="Changed shortcut"),
    }


class ReplacementTests(unittest.TestCase):
    def setUp(self):
        self.clock = use_virtual_time(self)

    def test_exact_saved_intent_survives_and_cleanup_tracks_only_remapped_equipment(self):
        fixture = ReplacementFixture()
        before = deepcopy(fixture.config)
        allocated = [set(activity._allocated_equipment) for activity in fixture.resources]
        planned, affected, enabled = replacement_proposal(fixture.config, fixture.system, fixture.member.device_id,
                                                          fixture.replacement.device_id)
        review = fixture.review()
        self.assertEqual(fixture.config, before)
        self.assertEqual(fixture.posts("apply"), [])
        review.apply().assert_retained()
        # The body is exactly the UI's own plan, behind its request identity.
        self.assertTrue(enabled)
        self.assertEqual(set(affected), {activity.activity_id for activity in fixture.resources})
        self.assertEqual(fixture.posts("apply"), [{"request_id": review.request_id, **planned}])
        self.assertEqual(list(fixture.posts("apply")[0])[0], "request_id")
        self.assertEqual({(row["previous"]["tentacle_id"], row["previous"]["side"],
                           row["replacement"]["tentacle_id"], row["replacement"]["side"])
                          for row in planned["ports"]}, {(0, "front", 1, "front"), (0, "back", 1, "back")})
        for key in ("activities", "activity_order", "external_slots"):
            self.assertEqual(fixture.config[key], before[key])
        posted = fixture.posts("apply")[0]
        for mapping in posted["ports"] + posted["equipment"]:
            self.assertEqual((mapping["previous"]["consortium_id"], mapping["replacement"]["consortium_id"]),
                             (fixture.group, fixture.group))
        moved = [row for row in fixture.config["components"]
                 if row["component_id"] in {mapping["component_id"] for mapping in posted["equipment"]}]
        self.assertEqual(len(moved), len(posted["equipment"]))
        for row in moved:
            self.assertEqual(set(row["qualified_name"]), {"octopus_id", "tentacle_id", "side", "osd_name"})
            self.assertEqual(row["qualified_name"]["octopus_id"], fixture.replacement.device_id)
        self.assertTrue(fixture.member._closed)
        self.assertFalse(fixture.replacement._closed)
        self.assertIn(fixture.member.device_id, fixture.replacement._removed_owners())
        self.assertEqual(set(fixture.bed._member_proofs), {fixture.replacement.device_id})
        for activity, identities in zip(fixture.resources, allocated):
            self.assertEqual(activity._allocated_equipment, identities)
            self.assertIs(activity._action_scan, fixture.action_scan)
            for component in activity.components.values():
                if component is not None:
                    self.assertIn(component, fixture.new)
            for identity, record in activity._new_equipment.items():
                self.assertEqual(record, next(row for row in fixture.config["components"]
                                             if row["component_id"] == identity))
        self.assertEqual(len(fixture.posts("apply")), 1)
        self.assertFalse(any(data is not None and path in ("/api/config", "/api/activity-editor")
                             for path, data in fixture.calls))
        with self.assertRaises(RuntimeError):
            review.apply()
        self.assertEqual(len(fixture.posts("apply")), 1)

    def test_changed_applied_saved_intent_cannot_rebind_activity_evidence(self):
        for name, mutation in corruptions().items():
            with self.subTest(fault=name):
                fixture = ReplacementFixture()
                original = [dict(activity.components) for activity in fixture.resources]
                fixture.apply_mutation = mutation
                review = fixture.review()
                with self.assertRaises(AssertionError):
                    review.apply()
                self.assertEqual([activity.components for activity in fixture.resources], original)

    def test_ui_plan_must_match_the_physical_moves_before_any_write(self):
        def unseen(f):
            f.system["observations"] = f.system["observations"][1:]

        def other_adapter(f):
            f.system["observations"][1] = f.observation(f.new[1], tentacle=2)

        def unresolved(f):
            f.system["observations"][0] = f.observation(f.new[0], resolved=False)

        def foreign_owner(f):
            f.system["observations"] = [f.observation(component) for component in f.old]

        def not_ready(f):
            f.system["membership"]["peers"][-1].update(ready=False)

        for fault, effect, error in (("unseen display", unseen, AssertionError),
                                     ("moved receiver", other_adapter, AssertionError),
                                     ("unresolved display", unresolved, AssertionError),
                                     ("old owner only", foreign_owner, AssertionError),
                                     ("replacement not ready", not_ready, MembershipConflict)):
            with self.subTest(fault=fault):
                fixture = ReplacementFixture()
                effect(fixture)
                with self.assertRaises(error):
                    fixture.review()
                self.assertEqual(fixture.posts("apply"), [])

    def test_unseen_equipment_follows_the_equipment_that_shared_its_connector(self):
        fixture = ReplacementFixture()
        fixture.system["observations"][2] = fixture.observation(fixture.new[2], resolved=False)
        review = fixture.review()
        source = next(row for row in review.payload["equipment"] if row["previous"]["osd_name"] == "Remote source")
        self.assertEqual((source["replacement"]["tentacle_id"], source["replacement"]["side"]), (1, "back"))
        review.apply().assert_retained()
        self.assertEqual(len(fixture.posts("apply")), 1)

    def test_plan_changed_after_review_is_not_submitted(self):
        fixture = ReplacementFixture()
        review = fixture.review()
        fixture.system["observations"][0] = fixture.observation(fixture.new[0], tentacle=2)
        with self.assertRaises((AssertionError, MembershipConflict)):
            review.apply()
        self.assertEqual(fixture.posts("apply"), [])
        self.assertFalse(fixture.member._closed)

    def test_applied_replacement_that_fails_verification_is_recorded_for_cleanup(self):
        fixture = ReplacementFixture()
        fixture.apply_mutation = corruptions()["home slot"]
        review = fixture.review()
        with self.assertRaisesRegex(AssertionError, "exact saved activity configuration"):
            review.apply()
        self.assertTrue(fixture.member._closed)
        self.assertEqual(set(fixture.bed._member_proofs), {fixture.replacement.device_id})
        # Every member's membership checks no longer expect the old member.
        self.assertIn(fixture.member.device_id, fixture.replacement._removed_owners())
        for activity in fixture.resources:
            for identity, record in activity._new_equipment.items():
                self.assertEqual(record, next(row for row in review.expected["components"]
                                             if row["component_id"] == identity))

    def test_changed_configuration_or_membership_after_review_is_not_submitted(self):
        for effect in (lambda f: f.config["external_slots"][-1].update(hidden=False),
                       lambda f: f.system["membership"].update(membership_revision="999")):
            fixture = ReplacementFixture()
            review = fixture.review()
            effect(fixture)
            with self.assertRaises(MembershipConflict):
                review.apply()
            self.assertEqual(fixture.posts("apply"), [])

    def test_incomplete_duplicate_or_unowned_mapping_is_rejected_without_a_write(self):
        for fault in ("missing", "duplicate", "unowned", "unverified", "wrong old boot", "not isolated"):
            with self.subTest(fault=fault):
                fixture = ReplacementFixture()
                pairs = fixture.pairs
                if fault == "missing":
                    pairs = pairs[:-1]
                elif fault == "duplicate":
                    pairs = (*pairs, pairs[0])
                elif fault == "unowned":
                    for activity in fixture.resources:
                        activity._new_equipment.clear()
                elif fault == "unverified":
                    fixture.bed._member_proofs.pop(fixture.replacement.device_id)
                elif fault == "wrong old boot":
                    fixture.member.member_boot = "e" * 32
                else:
                    fixture.old[0]._power = PowerState.STANDBY
                with self.assertRaises((AssertionError, MembershipConflict)):
                    fixture.review(pairs)
                self.assertEqual(fixture.posts("apply"), [])

    def test_lost_apply_reply_reconciles_one_request_and_later_config_drift_fails(self):
        fixture = ReplacementFixture()
        fixture.lost_apply_reply = True
        review = fixture.review().apply()
        self.assertEqual(len(fixture.posts("apply")), 1)
        self.assertIn(("/api/operations/" + review.request_id, None), fixture.calls)
        fixture.config["activities"][-1]["switch_input"] = 8
        with self.assertRaises(AssertionError):
            review.assert_retained()

    def test_missing_fresh_discovery_cannot_adopt_new_activity_recipients(self):
        fixture = ReplacementFixture()
        original = [dict(activity.components) for activity in fixture.resources]
        review = fixture.review()
        fixture.action_scan.expected = ()
        with self.assertRaisesRegex(AssertionError, "Missing fresh physical discovery"):
            review.apply()
        self.assertEqual([activity.components for activity in fixture.resources], original)
        self.assertFalse(review._applied)

    def test_later_missing_discovery_preserves_every_activity_fixture(self):
        fixture = ReplacementFixture()
        original = [dict(activity.components) for activity in fixture.resources]
        equipment = [deepcopy(activity._new_equipment) for activity in fixture.resources]
        scans = [activity.scan for activity in fixture.resources]
        review = fixture.review()
        # The first activity uses only the source, so it passes discovery;
        # the later display activity must fail before any fixture is rebound.
        fixture.action_scan.expected = fixture.new[1:]
        with self.assertRaisesRegex(AssertionError, "Missing fresh physical discovery"):
            review.apply()
        self.assertIn(fixture.new[2], fixture.action_scan.locations)
        self.assertEqual([activity.components for activity in fixture.resources], original)
        self.assertEqual([activity.scan for activity in fixture.resources], scans)
        self.assertFalse(review._applied)
        # The verified apply already moved the saved records; cleanup prunes
        # by those, while physical recipients stay unbound.
        records = {row["component_id"]: row for row in review.expected["components"]}
        self.assertEqual([activity._new_equipment for activity in fixture.resources],
                         [{identity: records[identity] for identity in before} for before in equipment])


if __name__ == "__main__":
    unittest.main()
