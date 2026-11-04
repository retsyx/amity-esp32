"""Policy checks cannot reuse older logs or silently skip lost capture bytes."""

from dataclasses import replace
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from amity_harness.diagnostics import LogCapture
from amity_harness.tx_policy import failed_key_attempts, fresh_log_delta


class FreshLogTests(unittest.TestCase):
    def capture(self, data, *, started=1, finished=2):
        return LogCapture(data, "a" * 32, "b" * 32, "b" * 32, "b" * 32, started, finished)

    def test_exact_append_and_ring_wrap_return_only_new_lines(self):
        before = self.capture(b"old\nretained\n")
        for data in (b"old\nretained\nfresh\n", b"retained\nfresh\n", b"ld\nretained\nfresh\n"):
            with self.subTest(data=data):
                after = self.capture(data, started=3, finished=4)
                self.assertEqual(fresh_log_delta(before, after), "fresh\n")
        self.assertEqual(fresh_log_delta(before, self.capture(before.data, started=3, finished=4)), "")
        self.assertEqual(fresh_log_delta(self.capture(b""), self.capture(b"new\n", started=3, finished=4)), "new\n")

    def test_missing_overlap_changed_boot_identity_or_capture_order_fails(self):
        before = self.capture(b"old\nretained\n")
        after = self.capture(b"retained\nfresh\n", started=3, finished=4)
        invalid = (replace(after, data=b"fresh\n"), replace(after, device_id="c" * 32),
                   replace(after, boot_before="c" * 32), replace(after, boot_after="c" * 32),
                   replace(after, started_ns=1), replace(after, data=b"retained\nfresh"))
        for changed in invalid:
            with self.subTest(changed=changed):
                with self.assertRaises(AssertionError):
                    fresh_log_delta(before, changed)

    def policy(self, entries, *, physical=4, evidence=None):
        before = self.capture(b"retained\n")
        lines = b"retained\n" + b"".join(
            (f"W (17) cec: TX failed (GPIO 2, {len(raw)} bytes: {raw.hex(' ')}, "
             f"result Nack, attempts {attempts}, started true)\n").encode()
            for raw, attempts in entries)
        return failed_key_attempts(before, self.capture(lines, started=3, finished=4),
                                   gpio=2, header=4, physical_attempts=physical, evidence=evidence)

    def test_count_mismatch_retains_transmission_evidence_before_failing(self):
        evidence = {}
        with self.assertRaisesRegex(AssertionError, "physical NACK count"):
            self.policy(((bytes([4, 0x44, 4]), 1), (bytes([4, 0x45]), 3)), physical=3, evidence=evidence)
        self.assertEqual(evidence["failed_transmissions"], [
            {"frame": "04:44:04", "attempts": 1}, {"frame": "04:45", "attempts": 3}])

    def test_failed_press_is_one_attempt_separate_from_release_and_matches_wire(self):
        press, release = bytes([4, 0x44, 4]), bytes([4, 0x45])
        self.assertEqual(self.policy(((press, 1), (release, 3))), [
            {"frame": "04:44:04", "attempts": 1}, {"frame": "04:45", "attempts": 3}])
        self.assertEqual(len(self.policy(((press, 1),), physical=1)), 1)
        # Both have four indistinguishable physical headers; the first is the
        # exact regression that a combined one-to-four bound concealed.
        for entries, physical in ((((press, 2), (release, 2)), 4),
                                  (((press, 1), (press, 1), (release, 2)), 4),
                                  (((press, 1), (release, 4)), 5),
                                  (((release, 3), (press, 1)), 4),
                                  (((press, 1), (release, 3)), 3),
                                  (((release, 3),), 3)):
            with self.subTest(entries=entries, physical=physical), self.assertRaises(AssertionError):
                self.policy(entries, physical=physical)


if __name__ == "__main__":
    unittest.main()
