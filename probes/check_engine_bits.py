#!/usr/bin/env python3
"""Fail if the probe engine's step bits drift from the pattern file.

The engine stores one zero or one per step, lane by lane, sixteen steps each.
This reads those assignments out of the JSFX text. It does not load REAPER.
"""

import re
import unittest
from pathlib import Path

import schedule


ROOT = Path(__file__).resolve().parents[0]
ENGINE = ROOT.parent / "jsfx" / "furrow_probe_engine.jsfx"


def engine_steps(text):
    steps = [0] * 64
    for index, value in re.findall(r"step_on\[(\d+)\] = (\d+);", text):
        steps[int(index)] = int(value)
    return steps


class EngineBitsTest(unittest.TestCase):
    def test_engine_bits_match_the_pattern_file(self):
        header, lanes = schedule.read_pattern(ROOT / "pattern-4x16.txt")
        text = ENGINE.read_text()
        stored = engine_steps(text)
        for lane in lanes:
            for index, enabled in enumerate(lane["steps"]):
                self.assertEqual(
                    stored[lane["lane"] * 16 + index],
                    enabled,
                    f"lane {lane['lane']} step {index}",
                )
            self.assertIn(f"note_of[{lane['lane']}] = {header['notes'][lane['lane']]};", text)
        self.assertEqual(text.count("@block"), 1)
        self.assertEqual((ROOT.parent / "jsfx" / "furrow_probe_receiver.jsfx").read_text().count("@block"), 1)


if __name__ == "__main__":
    unittest.main()
