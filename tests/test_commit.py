"""The commit renderer against SeqCore in ysfx. Both must produce the same MIDI for the same
pattern and origin. ysfx is not REAPER, so this checks the rules and not the host."""

import random
import unittest

from harness import Rig, missing

SKIP = missing()
SAMPLES_PER_BEAT = 22050.0  # 120 BPM at 44.1 kHz
OP_SET_STEP, OP_ADD_LANE, OP_SET_LANE, OP_SET_RACK, OP_SET_LENGTH = 2, 3, 5, 6, 7
R_SEED, R_SWING, R_TRUNCATE = 1, 4, 5
L_SWING = 10
STEPS = 40
DEFAULTS = [0, 100, 64, 0, 0, 0, 64, 64, 0, 5000, 1, 0, 100, 0, 0, 0]


def random_pattern(seed, lengths):
    rng = random.Random(seed)
    rows = {}
    for slot, length in lengths.items():
        for step in range(length):
            if rng.random() < 0.7:
                row = list(DEFAULTS)
                row[0] = 1
                row[1] = rng.randint(1, 127)
                row[2] = rng.choice([64, 64, 0, 100])
                row[3] = rng.choice([0, 0, 0, 2, -3])
                row[4] = rng.choice([0, 0, 50, -30])
                row[5] = rng.choice([0, 0, 10, -20])
                row[6] = rng.choice([64, 64, 100])
                row[7] = rng.choice([64, 64, 20])
                row[8] = rng.choice([0, 0, 2500, 6000])
                row[9] = rng.choice([2500, 5000, 10000, 15000])
                row[10] = rng.choice([1, 1, 2, 3, 4])
                row[12] = rng.choice([100, 100, 60])
                rows[(slot, step)] = row
    return rows


@unittest.skipIf(SKIP, SKIP)
class CommitTest(unittest.TestCase):
    def setUp(self):
        self.rig = Rig()
        self.rig.attach()
        self.lua = self.rig.lua
        self.commit = self.lua.eval('(require("furrow.commit"))')

    def tearDown(self):
        self.rig.close()

    def build(self, rows, lengths, swing, truncate, lane_swing=(10000, 6000), seed=4242):
        commands = [[OP_ADD_LANE, 0, 1000, 7, 60, 0], [OP_ADD_LANE, 1, 1001, 8, 64, 1],
                    [OP_SET_RACK, R_SEED, seed], [OP_SET_RACK, R_SWING, swing], [OP_SET_RACK, R_TRUNCATE, truncate],
                    [OP_SET_LANE, 0, L_SWING, lane_swing[0]], [OP_SET_LANE, 1, L_SWING, lane_swing[1]],
                    [OP_SET_LANE, 0, 7, 8000], [OP_SET_LANE, 1, 8, -3000], [OP_SET_LANE, 1, 9, 12],
                    [OP_SET_LANE, 1, 13, 21]]
        for slot, length in lengths.items():
            commands.append([OP_SET_LENGTH, 0, slot, length])
        for (slot, step), row in rows.items():
            for field, value in enumerate(row):
                if value != DEFAULTS[field]:
                    commands.append([OP_SET_STEP, 0, slot, step, field, value])
        for first in range(0, len(commands), 150):
            self.assertEqual(self.rig.apply(commands[first:first + 150]), 1)

    def snapshot_input(self, lengths, origin=0.0):
        self.rig.session.request(self.lua.table_from({"pattern": 0, "l0": 0, "nl": 2, "s0": 0, "ns": 16}))
        snap = self.rig.snapshot()
        lanes = []
        for slot, length in lengths.items():
            lane = snap.lanes[slot]
            rows = {}
            for page in range(0, length, 16):
                self.rig.session.request(self.lua.table_from(
                    {"pattern": 0, "l0": 0, "nl": 2, "s0": page, "ns": min(16, length - page)}))
                window = self.rig.snapshot()
                for step in range(page, min(page + 16, length)):
                    rows[step] = window.steps[slot][step]
            lanes.append(self.lua.table_from({
                "slot": slot, "note": lane.note, "volume": lane.volume, "pan": lane.pan, "bend": lane.bend,
                "mod_a": lane.mod_a, "mod_b": lane.mod_b, "swing": lane.swing, "prob_key": lane.prob_key,
                "length": lane.length, "rows": self.lua.table_from(rows)}))
        rack = snap.rack
        return self.lua.table_from({
            "origin": origin, "steps": STEPS, "seed": rack.seed, "swing": rack.swing, "truncate": rack.truncate,
            "pattern_key": 0, "lanes": self.lua.table_from(lanes)})

    def play_once(self):
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(222)
        self.played = list(self.rig.events)

    def live_events(self, slot):
        want = []
        for sample, bus, status, d1, d2 in self.played:
            if bus == slot // 16 and status & 0x0F == slot % 16 and not (status & 0xF0 == 0xB0 and d1 == 123):
                want.append((sample, status & 0xF0, d1, d2))
        return want

    def rendered_events(self, rendered, slot):
        events = rendered[slot]
        return [(e.beat * SAMPLES_PER_BEAT, e.status, e.data1, e.data2) for e in events.values()]

    def assert_same(self, rendered, slot):
        end = STEPS * 0.25 * SAMPLES_PER_BEAT
        live = [e for e in self.live_events(slot) if e[0] < end - 0.5]
        mine = [e for e in self.rendered_events(rendered, slot) if e[0] < end - 0.5]
        self.assertGreater(len(live), 40)
        self.assertEqual([e[1:] for e in mine], [e[1:] for e in live], slot)
        for a, b in zip(mine, live):
            self.assertLessEqual(abs(a[0] - b[0]), 1.0, (a, b))

    def check(self, seed, swing, truncate, lengths=None):
        lengths = lengths or {0: 16, 1: 6}
        self.build(random_pattern(seed, lengths), lengths, swing, truncate)
        rendered = self.commit.render(self.snapshot_input(lengths))
        self.play_once()
        for slot in lengths:
            self.assert_same(rendered, slot)

    def test_straight_pattern_matches_the_engine(self):
        self.check(1, 0, 0)

    def test_swing_and_truncation_match_the_engine(self):
        self.check(2, 6000, 1)

    def test_swing_without_truncation_matches_the_engine(self):
        self.check(3, 10000, 0)

    def test_a_pattern_longer_than_one_page_matches_the_engine(self):
        self.check(4, 3000, 0, {0: 24, 1: 7})


    def test_hits_past_the_end_are_left_out_and_a_held_note_is_released_at_the_end(self):
        row = list(DEFAULTS)
        row[0], row[8], row[9], row[10] = 1, 5000, 15000, 2
        empty = self.lua.table_from(dict(enumerate(DEFAULTS)))
        full = {i: (self.lua.table_from(dict(enumerate(row))) if i == 3 else empty) for i in range(16)}
        lane = self.lua.table_from({"slot": 0, "note": 60, "volume": 5000, "pan": 0, "bend": 2, "mod_a": 74,
                                    "mod_b": 71, "swing": 10000, "prob_key": 0, "length": 16,
                                    "rows": self.lua.table_from(full)})
        data = self.lua.table_from({"origin": 0.0, "steps": 4, "seed": 0, "swing": 0, "truncate": 0,
                                    "pattern_key": 0, "lanes": self.lua.table_from([lane])})
        events = self.commit.render(data)[0]
        notes = [(e.beat, e.status) for e in events.values() if e.status in (0x80, 0x90)]
        self.assertEqual(notes, [(0.875, 0x90), (1.0, 0x80)])


    def test_the_snapshot_text_round_trips_and_the_fingerprint_tracks_every_byte(self):
        lengths = {0: 16, 1: 6}
        self.build(random_pattern(5, lengths), lengths, 3000, 1)
        data = self.snapshot_input(lengths)
        for lane in data.lanes.values():
            lane.id = 1000 + lane.slot
        text = self.commit.encode(data)
        decoded = self.commit.decode(text)
        self.assertEqual(self.commit.encode(decoded), text)
        self.assertEqual((decoded.seed, decoded.swing, decoded.truncate, decoded.steps), (4242, 3000, 1, STEPS))
        self.assertEqual(decoded.lanes[2].length, 6)
        bad, message = (lambda r: r if isinstance(r, tuple) else (r, None))(self.commit.decode("nope"))
        self.assertIsNone(bad)
        fp = self.commit.fingerprint
        self.assertEqual(len(fp(text)), 16)
        self.assertNotEqual(fp(text), fp(text[:-2] + "x\n"))
        self.assertEqual(fp(text), fp(text))


if __name__ == "__main__":
    unittest.main()
