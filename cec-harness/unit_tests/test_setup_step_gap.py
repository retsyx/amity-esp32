"""Setup-step gaps use only fresh Amity wire frames inside setup."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from functional_tests import activity as scenarios


class SetupStepGapTests(unittest.TestCase):
    @staticmethod
    def row(stamp, *, gpio=23, source=0, direction="rx", transport="wire"):
        return {"direction": direction, "transport": transport, "gpio": gpio,
                "source": source, "wire_timestamp_ns": stamp}

    def test_largest_gap_uses_only_fresh_amity_wire_frames_inside_setup(self):
        records = [self.row(5), self.row(100), self.row(250, gpio=24), self.row(400),
                   self.row(300, source=4), self.row(320, transport="virtual"),
                   self.row(330, direction="tx"), self.row(340, gpio=99), self.row(900)]
        frames, gap = scenarios.setup_step_gap(records, {23: 0, 24: 0}, {23, 24}, since=10, through=400)
        self.assertEqual((frames, gap), (3, 150))

    def test_a_reply_wait_between_steps_is_the_reported_gap(self):
        records = [self.row(100), self.row(1_600_000_100), self.row(1_600_050_000)]
        frames, gap = scenarios.setup_step_gap(records, {23: 0}, {23}, since=0, through=2_000_000_000)
        self.assertEqual((frames, gap), (3, 1_600_000_000))
        self.assertEqual(scenarios.setup_step_gap([], {23: 0}, {23}, since=0, through=1), (0, 0))


if __name__ == "__main__":
    unittest.main()
