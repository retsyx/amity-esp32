"""A later verified sibling survives removal without inheriting another owner."""

from copy import deepcopy
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.membership import MembershipConflict
from amity_harness.removal import MembershipContinuity
from test_removal_cascade import CascadeFixture


class ReplacementTests(unittest.TestCase):
    def setUp(self):
        sleeper = patch("amity_harness.membership.time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def later_member(self, fixture):
        identity = fixture.main.wiring.identities["amity-dut-3"].device_id
        other = SimpleNamespace(main=fixture.main, session=fixture.bed.session, device_id=identity,
                                name="amity-dut-3", member_boot="9" * 32, _created=True, _closed=False)
        fixture.bed._member_proofs[identity] = other
        fixture.system["membership"]["peers"].append(fixture.peer(identity, other.name, other.member_boot))
        fixture.system["named_devices"].append({"octopus_id": identity, "hostname": other.name})
        fixture.config["octopuses"].append(fixture.owner(identity))
        fixture.advance()
        return other

    def test_remove_earlier_member_preserves_exact_later_verified_sibling(self):
        fixture = CascadeFixture()
        other = self.later_member(fixture)
        with self.assertRaises(MembershipConflict):
            fixture.member.review_removal(*fixture.resources)
        before = deepcopy(fixture.system["membership"]["peers"][-1])
        fixture.member.review_removal(*fixture.resources, preserve=(other,)).confirm()
        self.assertTrue(fixture.member._closed)
        self.assertEqual(fixture.system["membership"]["peers"][-1], before)
        self.assertEqual(fixture.posts("remove")[0]["octopus_id"], fixture.member.device_id)

    def test_preservation_cannot_admit_unverified_changed_or_unlisted_sibling(self):
        for fault in ("unverified", "closed", "foreign_session", "boot", "name", "identity", "extra"):
            with self.subTest(fault=fault):
                fixture = CascadeFixture()
                other = self.later_member(fixture)
                if fault == "unverified":
                    fixture.bed._member_proofs.pop(other.device_id)
                elif fault == "closed":
                    other._closed = True
                elif fault == "foreign_session":
                    other.session = object()
                elif fault == "extra":
                    fixture.system["membership"]["peers"].append(fixture.peer("e" * 32, "extra", "8" * 32))
                else:
                    key = {"boot": "boot_id", "name": "hostname", "identity": "octopus_id"}[fault]
                    fixture.system["membership"]["peers"][-1][key] = "e" * 32 if key != "hostname" else "changed"
                with self.assertRaises(MembershipConflict):
                    fixture.member.review_removal(*fixture.resources, preserve=(other,))
                self.assertEqual(fixture.posts("remove"), [])

    def continuity_fixture(self):
        fixture = CascadeFixture()
        row = fixture.config["activities"][-1]
        row.update(source="4" * 32, display="5" * 32, audio="6" * 32)
        activity = fixture.resources[-1]
        activity._created_row = fixture.editor_row(row)
        return fixture, MembershipContinuity(fixture.member, activity)

    def test_continuity_accepts_owner_reordering_and_new_sibling_but_keeps_saved_ids(self):
        fixture, continuity = self.continuity_fixture()
        other = self.later_member(fixture)
        fixture.config["octopuses"].reverse()
        fixture.system["membership"]["peers"].reverse()
        existing = SimpleNamespace(device_id=fixture.existing_id)
        removed = SimpleNamespace(device_id="e" * 32)
        continuity.assert_preserved(fixture.member, existing, other, removed=(removed,))
        proof = fixture.bed.control_observations[-1]
        self.assertTrue(proof["preserved"])
        self.assertEqual(proof["survivor"], fixture.candidate.identity.device_id)
        self.assertEqual(len(proof["saved_sha256"]), 64)
        self.assertEqual(proof["members"], sorted((fixture.member.device_id, fixture.existing_id, other.device_id)))
        self.assertNotIn("PRIVATE_", json.dumps(proof))

    def test_changed_owner_boot_equipment_activity_or_removed_observation_fails(self):
        mutations = {
            "boot": lambda f: f.system["membership"]["peers"][-1].update(boot_id="e" * 32),
            "identity": lambda f: f.system["membership"]["peers"][-1].update(octopus_id="e" * 32),
            "equipment_owner": lambda f: f.config["components"][-1]["qualified_names"][0].update(octopus_id="e" * 32),
            "equipment_id": lambda f: f.config["components"][-1].update(component_id="e" * 32),
            "activity_id": lambda f: f.config["activities"][-1].update(activity_id="e" * 32),
            "activity_selection": lambda f: f.config["activities"][-1].update(source="3" * 32),
            "qualified_observation": lambda f: f.system.update(observations=[{"qualified_name": {"octopus_id": "e" * 32}}]),
            "runtime_observation": lambda f: f.system.update(observations=[{"runtime_address": {"octopus_id": "e" * 32}}]),
            "saved_name": lambda f: f.system["named_devices"].append({"octopus_id": "e" * 32, "hostname": "removed"}),
        }
        for fault, mutation in mutations.items():
            with self.subTest(fault=fault):
                fixture, continuity = self.continuity_fixture()
                mutation(fixture)
                with self.assertRaises((AssertionError, MembershipConflict)):
                    continuity.assert_preserved(fixture.member, SimpleNamespace(device_id=fixture.existing_id),
                                                removed=(SimpleNamespace(device_id="e" * 32),))
                self.assertEqual(fixture.posts("remove"), [])


if __name__ == "__main__":
    unittest.main()
