#!/usr/bin/env python3
"""Literal checks for the schedule. These do not call REAPER."""

import math
import tempfile
import unittest
from pathlib import Path

import diff_log
import make_tempo_project
import schedule


ROOT = Path(__file__).resolve().parent


class ScheduleTest(unittest.TestCase):
    @staticmethod
    def event(sample, sequence, receiver, lane, bus, channel, note, velocity, kind):
        return {
            "sample": sample,
            "sequence": sequence,
            "receiver": receiver,
            "lane": lane,
            "bus": bus,
            "channel": channel,
            "note": note,
            "velocity": velocity,
            "kind": kind,
        }

    @staticmethod
    def expected(sample, order, receiver, lane, bus, channel, note, velocity, kind):
        return {
            "sample": sample,
            "order": order,
            "receiver": receiver,
            "lane": lane,
            "bus": bus,
            "channel": channel,
            "note": note,
            "velocity": velocity,
            "kind": kind,
        }

    def test_diff_rejects_wrong_lane_delivery(self):
        expected = [self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on")]
        captured = [self.event(100, 0, 0, 1, 0, 0, 36, 100, "on")]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("lane 0" in problem and "lane 1" in problem for problem in problems), problems)

    def test_diff_rejects_wrong_receiver_routing(self):
        expected = [self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on")]
        captured = [self.event(100, 0, 1, 0, 0, 0, 36, 100, "on")]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("receiver 0" in problem and "receiver 1" in problem for problem in problems), problems)

    def test_diff_rejects_wrong_destination_bus(self):
        expected = [self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on")]
        captured = [self.event(100, 0, 0, 0, 1, 0, 36, 100, "on")]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("bus 0" in problem and "bus 1" in problem for problem in problems), problems)

    def test_diff_rejects_release_after_simultaneous_note_on(self):
        expected = [
            self.expected(100, 0, 0, 0, 0, 0, 36, 0, "off"),
            self.expected(100, 2, 0, 0, 0, 0, 36, 100, "on"),
        ]
        captured = [
            self.event(100, 0, 0, 0, 0, 0, 36, 100, "on"),
            self.event(100, 1, 0, 0, 0, 0, 36, 0, "off"),
        ]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("order" in problem.lower() for problem in problems), problems)

    def test_diff_enforces_controller_between_release_and_note_on(self):
        expected = [
            self.expected(100, 0, 0, 0, 0, 0, 36, 0, "off"),
            self.expected(100, 1, 0, 0, 0, 0, 0, 64, "cc"),
            self.expected(100, 2, 0, 0, 0, 0, 36, 100, "on"),
        ]
        captured = [
            self.event(100, 0, 0, 0, 0, 0, 0, 64, "cc"),
            self.event(100, 1, 0, 0, 0, 0, 36, 0, "off"),
            self.event(100, 2, 0, 0, 0, 0, 36, 100, "on"),
        ]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("order" in problem.lower() and "cc" in problem and "off" in problem for problem in problems), problems)

    def test_diff_rejects_missing_note_release(self):
        expected = [
            self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on"),
            self.expected(200, 0, 0, 0, 0, 0, 36, 0, "off"),
        ]
        captured = [self.event(100, 0, 0, 0, 0, 0, 36, 100, "on")]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("missing" in problem and "off" in problem for problem in problems), problems)

    def test_diff_rejects_unexpected_event(self):
        expected = [self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on")]
        captured = [
            self.event(100, 0, 0, 0, 0, 0, 36, 100, "on"),
            self.event(150, 1, 0, 0, 0, 0, 38, 100, "on"),
        ]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("extra" in problem and "note 38" in problem for problem in problems), problems)

    def test_diff_rejects_event_outside_tolerance(self):
        expected = [self.expected(100, 1, 0, 0, 0, 0, 36, 100, "on")]
        captured = [self.event(102, 0, 0, 0, 0, 0, 36, 100, "on")]
        problems = diff_log.compare(expected, captured, 1)
        self.assertTrue(any("timing" in problem.lower() and "100" in problem and "102" in problem for problem in problems), problems)

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

    def test_ramp_ends_where_reaper_reached_140(self):
        # The REAPER 7.82 capture logged 100 + 20 BPM per second, reaching 140 at 2.0 s.
        got = schedule.seconds_at(4.0, [(0.0, 100.0), (4.0, 140.0)])
        self.assertAlmostEqual(got, 2.0, places=12)

    def test_partial_ramp_solves_the_time_linear_integral(self):
        # Beats at time t are (100 t + 10 t^2) / 60. Beat 2 solves t^2 + 10 t - 12 = 0.
        got = schedule.seconds_at(2.0, [(0.0, 100.0), (4.0, 140.0)])
        self.assertAlmostEqual(got, (-10.0 + math.sqrt(148.0)) / 2.0, places=12)

    def test_case_file_keeps_the_order_of_a_tempo_step(self):
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory) / "step.txt"
            case.write_text("sample_rate_hz 44100\norigin_beat 0\npoints\n0 120\n0.75 120\n0.75 60\n")
            points = schedule.read_case(case)["points"]
        self.assertEqual(points, [(0.0, 120.0), (0.75, 120.0), (0.75, 60.0)])
        # 0.75 beats at 120 BPM is 0.375 s, then 0.25 beats at 60 BPM adds 0.25 s.
        self.assertAlmostEqual(schedule.seconds_at(1.0, points), 0.625, places=12)

    def test_two_tempo_points_at_one_beat(self):
        # The zero-length span takes no time. 60 BPM then holds, so beat 1 is 1 second.
        points = [(0.0, 120.0), (0.0, 60.0)]
        self.assertEqual(schedule.seconds_at(0.0, points), 0.0)
        self.assertAlmostEqual(schedule.seconds_at(1.0, points), 1.0, places=9)

    def test_pattern_counts_and_first_pair(self):
        lines = schedule.schedule(
            ROOT / "pattern-4x16.txt",
            ROOT / "cases" / "constant-120.txt",
        )
        # Lanes enable 4, 4, 8 and 3 steps. Each step is one note-on and one note-off.
        self.assertEqual(len(lines), 38)
        self.assertEqual(lines[0], "0 1 0 0 0 0 36 100 on")
        self.assertEqual(lines[1], "0 1 2 2 0 2 42 100 on")
        self.assertEqual(lines[2], "2756 0 0 0 0 0 36 0 off")

    def test_generated_files_match_a_fresh_run(self):
        for name in ("constant-120", "ramp-44100-1024", "ramp-edge-44100-1024", "step-on-beat-44100-1024", "step-off-grid-44100-1024"):
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

    def test_legacy_capture_is_rejected_as_unverifiable(self):
        results = ROOT / "results" / "reaper-7.82-linux"
        expected = diff_log.load(ROOT / "expected" / "constant-120.txt")
        legacy = diff_log.load(results / "constant-120-events.txt")
        self.assertEqual(
            diff_log.compare(expected, legacy, 1),
            ["unverifiable: legacy capture discarded observed sequence and receiver identity"],
        )

    def test_tempo_project_writes_a_ramp_and_a_step(self):
        source = "\r\n".join([
            "  TEMPO 120 4 4 0",
            "  <TEMPOENVEX",
            "    ACT 0 -1",
            "    PT 9.000000000000 90.0000000000 1",
            "  >",
            "",
        ])
        points = [(0.0, 100.0), (1.0, 100.0), (3.0, 200.0), (4.0, 200.0), (4.0, 60.0)]
        self.assertEqual(
            make_tempo_project.rewrite(source, points).split("\r\n"),
            [
                "  TEMPO 100 4 4 0",
                "  <TEMPOENVEX",
                "    ACT 1 -1",
                "    PT 0.000000000000 100.0000000000 1",
                "    PT 0.600000000000 100.0000000000 0",
                "    PT 1.400000000000 200.0000000000 1",
                "    PT 1.700000000000 60.0000000000 1",
                "  >",
                "",
            ],
        )

    def test_legacy_tempo_edge_captures_are_unverifiable(self):
        results = ROOT / "results" / "reaper-7.82-linux"

        def problems(name, tolerance):
            expected = diff_log.load(ROOT / "expected" / f"{name}.txt")
            return diff_log.compare(expected, diff_log.load(results / f"{name}-events.txt"), tolerance)

        for name in ("ramp-edge-44100-1024", "step-on-beat-44100-1024", "step-off-grid-44100-1024"):
            self.assertEqual(
                problems(name, 1),
                ["unverifiable: legacy capture discarded observed sequence and receiver identity"],
            )

    def test_diff_rejects_an_extra_note(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            expect = base / "expect.txt"
            seen = base / "seen.txt"
            expect.write_text("# sample order receiver lane bus channel note velocity kind\n0 1 0 0 0 0 36 100 on\n")
            seen.write_text("# sample sequence receiver lane bus channel note velocity kind\n0 0 0 0 0 0 36 100 on\n10 1 1 1 0 1 38 100 on\n")
            problems = diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1)
            self.assertEqual(
                problems,
                ["extra event: observed sample 10 on lane 1 receiver 1 bus 0 ch 1 note 38 velocity 100"],
            )

    def test_diff_accepts_one_sample_and_rejects_two(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            expect = base / "expect.txt"
            seen = base / "seen.txt"
            expect.write_text("# sample order receiver lane bus channel note velocity kind\n100 1 0 0 0 0 36 100 on\n")
            seen.write_text("# sample sequence receiver lane bus channel note velocity kind\n101 0 0 0 0 0 36 100 on\n")
            self.assertEqual(diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1), [])
            seen.write_text("# sample sequence receiver lane bus channel note velocity kind\n102 0 0 0 0 0 36 100 on\n")
            problems = diff_log.compare(diff_log.load(expect), diff_log.load(seen), 1)
            self.assertEqual(len(problems), 1)
            self.assertIn("expected sample 100", problems[0])
            self.assertIn("observed sample 102", problems[0])


if __name__ == "__main__":
    unittest.main()
