#!/usr/bin/env python3
"""Literal checks for the schedule. These do not call REAPER."""

import math
import tempfile
import unittest
from pathlib import Path

import diff_log
import schedule


ROOT = Path(__file__).resolve().parent


class ScheduleTest(unittest.TestCase):
    def test_half_sample_tie_moves_to_even(self):
        self.assertEqual(schedule.round_half_even(0.5), 0)
        self.assertEqual(schedule.round_half_even(1.5), 2)
        self.assertEqual(schedule.round_half_even(2.5), 2)
        self.assertEqual(schedule.round_half_even(1.4), 1)
        self.assertEqual(schedule.round_half_even(1.6), 2)

    def test_constant_120_bpm_step_and_half_gate(self):
        # 120 BPM is 2 beats per second. A 1/16 step is 0.25 beats, so 0.125 s.
        # At 44100 Hz that is exactly 5512.5 samples, which ties to 5512.
        # The gate is half a step, so the first note-off ties at 2756.5 and rounds to 2756.
        step = schedule.seconds_at(0.25, [(0.0, 120.0)])
        gate = schedule.seconds_at(0.125, [(0.0, 120.0)])
        self.assertAlmostEqual(step, 0.125, places=9)
        self.assertEqual(schedule.round_half_even(step * 44100), 5512)
        self.assertEqual(schedule.round_half_even(gate * 44100), 2756)

    def test_linear_ramp_matches_the_integral(self):
        # 100 BPM to 140 BPM across 4 beats. At beat 4 the integral is
        # 60/10 * ln(1.4) seconds.
        got = schedule.seconds_at(4.0, [(0.0, 100.0), (4.0, 140.0)])
        self.assertAlmostEqual(got, 6.0 * math.log(1.4), places=9)

    def test_partial_ramp_uses_the_tempo_at_that_beat(self):
        # Halfway through the 100 to 140 ramp the tempo is 120, so the integral is 6 * ln(1.2).
        got = schedule.seconds_at(2.0, [(0.0, 100.0), (4.0, 140.0)])
        self.assertAlmostEqual(got, 6.0 * math.log(1.2), places=9)

    def test_pattern_counts_and_first_pair(self):
        lines = schedule.schedule(
            ROOT / "pattern-4x16.txt",
            ROOT / "cases" / "constant-120.txt",
        )
        # Lanes enable 4, 4, 8 and 3 steps. Each step is one note-on and one note-off.
        self.assertEqual(len(lines), 38)
        self.assertEqual(lines[0], "0 1 0 0 0 0 36 100 on")
        self.assertEqual(lines[1], "0 1 2 0 0 2 42 100 on")
        self.assertEqual(lines[2], "2756 0 0 0 0 0 36 0 off")

    def test_generated_files_match_a_fresh_run(self):
        for name in ("constant-120", "ramp-44100-1024"):
            fresh = schedule.schedule(
                ROOT / "pattern-4x16.txt",
                ROOT / "cases" / f"{name}.txt",
            )
            stored = [
                line
                for line in (ROOT / "expected" / f"{name}.txt").read_text().splitlines()
                if line and not line.startswith("#")
            ]
            self.assertEqual(stored, fresh)

    def test_diff_rejects_an_extra_note(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            expect = base / "expect.txt"
            seen = base / "seen.txt"
            expect.write_text("0 1 0 0 0 0 36 100 on\n")
            seen.write_text("0 1 0 0 0 0 36 100 on\n10 1 1 0 0 1 38 100 on\n")
            problems = diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1)
            self.assertEqual(problems, ["extra 10 on note 38 ch 1"])

    def test_diff_accepts_one_sample_and_rejects_two(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            expect = base / "expect.txt"
            seen = base / "seen.txt"
            expect.write_text("100 1 0 0 0 0 36 100 on\n")
            seen.write_text("101 1 0 0 0 0 36 100 on\n")
            self.assertEqual(diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1), [])
            seen.write_text("102 1 0 0 0 0 36 100 on\n")
            problems = diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1)
            self.assertEqual(len(problems), 2)


if __name__ == "__main__":
    unittest.main()
