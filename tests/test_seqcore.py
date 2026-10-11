"""SeqCore and the Lua session, run together in ysfx. ysfx is not REAPER.

A pass shows the EEL2 code and the protocol agree. REAPER runs still decide host behaviour."""

import re
import unittest

from harness import LUA, SEQCORE, Rig, missing

SKIP = missing()

OP_COMMIT, OP_SET_STEP, OP_ADD_LANE, OP_REMOVE_LANE, OP_SET_LANE, OP_SET_RACK, OP_SET_LENGTH = 1, 2, 3, 4, 5, 6, 7
OP_AUDITION = 8
OK, STALE, INVALID, CAPACITY, ABORTED, LOAD_ERROR = 1, 2, 3, 4, 5, 6
L_MUTE, L_SOLO, L_VOLUME, L_PAN = 5, 6, 7, 8
F_ON, F_VEL, F_GATE = 0, 1, 9
SAMPLES_PER_STEP = 5512.5  # a sixteenth at 120 BPM and 44.1 kHz


def add_lane(slot, lane_id=None, note=60, row=None):
    return [OP_ADD_LANE, slot, lane_id or 1000 + slot, 7 + slot, note, slot if row is None else row]


def step_on(slot, step, value=1, pattern=0):
    return [OP_SET_STEP, pattern, slot, step, F_ON, value]


@unittest.skipIf(SKIP, SKIP)
class WireTest(unittest.TestCase):
    def test_lua_constants_match_the_engine(self):
        engine = dict(re.findall(r"^([A-Z][A-Z0-9_]*) = (-?\d+);", SEQCORE.read_text(), re.M))
        lua = dict(re.findall(r"^  ([A-Z][A-Z0-9_]*) = (-?\d+),", (LUA / "furrow" / "wire.lua").read_text(), re.M))
        shared = sorted(set(engine) & set(lua))
        self.assertGreater(len(shared), 90)
        self.assertEqual({k: lua[k] for k in shared}, {k: engine[k] for k in shared})
        self.assertEqual(lua["LANE_LENGTH"], "15")
        self.assertEqual(engine["LANE_FIELDS"], "15")

    def test_no_two_engine_names_differ_only_by_case(self):
        # EEL2 ignores case, so a local pt and the table PT are one variable.
        code = re.sub(r"//.*", "", SEQCORE.read_text().split("@init", 1)[1])
        spellings = {}
        for name in re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", code):
            spellings.setdefault(name.lower(), set()).add(name)
        self.assertEqual({k: v for k, v in spellings.items() if len(v) > 1}, {})


@unittest.skipIf(SKIP, SKIP)
class SeqCoreTest(unittest.TestCase):
    def setUp(self):
        self.rig = Rig()
        self.rig.attach()

    def tearDown(self):
        self.rig.close()

    def test_startup_sends_all_notes_off_on_every_route(self):
        cc123 = [(e[1], e[2] & 0x0F) for e in self.rig.events if e[2] & 0xF0 == 0xB0 and e[3] == 123]
        self.assertEqual(cc123, [(s // 16, s % 16) for s in range(64)])

    def test_two_engines_claim_different_slots(self):
        from harness import jsfx_host
        other = jsfx_host.Effect(SEQCORE)
        try:
            other.time(120, 0, 0)
            other.process()
            self.assertEqual((self.rig.slot, other.slider(1)), (0, 1.0))
        finally:
            other.close()

    def test_a_transaction_applies_whole_and_the_snapshot_shows_it(self):
        code = self.rig.apply([add_lane(0), add_lane(3, row=1), step_on(0, 0), step_on(3, 5),
                               [OP_SET_STEP, 0, 3, 5, F_VEL, 64]])
        self.assertEqual(code, OK)
        self.rig.session.request(self.rig.lua.table_from({"pattern": 0, "l0": 0, "nl": 4, "s0": 0, "ns": 16}))
        snap = self.rig.snapshot()
        self.assertEqual(sorted(snap.lanes.keys()), [0, 3])
        self.assertEqual((snap.lanes[3].row, snap.lanes[3].id, snap.lanes[3].length), (1, 1003, 16))
        self.assertEqual([snap.steps[0][s][F_ON] for s in range(4)], [1, 0, 0, 0])
        self.assertEqual((snap.steps[3][5][F_ON], snap.steps[3][5][F_VEL]), (1, 64))
        self.assertEqual(snap.rack.value_count, 3)

    def test_an_invalid_command_leaves_no_partial_state(self):
        self.assertEqual(self.rig.apply([add_lane(0)]), OK)
        code = self.rig.apply([step_on(0, 1), [OP_SET_STEP, 0, 0, 2, F_VEL, 500]])
        self.assertEqual(code, INVALID)
        code = self.rig.apply([step_on(0, 1), step_on(5, 1)])
        self.assertEqual(code, INVALID)
        self.assertEqual(self.rig.engine.var("nd_count"), 0)
        self.assertEqual(self.rig.engine.var("revision"), 1)

    def test_lanes_play_on_the_sixteenth_grid_with_controllers_first(self):
        self.rig.apply([add_lane(0), add_lane(17, note=38), step_on(0, 0), step_on(0, 4),
                        step_on(17, 2), [OP_SET_LANE, 17, L_PAN, -10000], [OP_SET_LANE, 17, L_VOLUME, 10000]])
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(25)
        ons = [(e[0], e[1], e[2], e[3]) for e in self.rig.notes()]
        self.assertEqual(ons, [(0, 0, 0x90, 60), (11025, 1, 0x91, 38), (22050, 0, 0x90, 60)])
        lane17 = [e[2:] for e in self.rig.events if e[1] == 1 and e[0] == 11025]
        self.assertEqual(lane17, [(0xB1, 7, 127), (0xB1, 10, 0), (0x91, 38, 100)])
        offs = [(e[0], e[3]) for e in self.rig.events if e[2] & 0xF0 == 0x80 and e[0] > 0]
        # Gate 0.5 of a step is 2756.25 samples, rounded to the nearest sample.
        self.assertEqual(offs, [(2756, 60), (11025 + 2756, 38), (22050 + 2756, 60)])

    def test_lane_length_and_loop_repeat(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 2), [OP_SET_LENGTH, 0, 0, 3]])
        self.rig.seek(0.0)
        self.rig.events.clear()
        self.rig.run(87)
        starts = [round(e[0] / SAMPLES_PER_STEP) for e in self.rig.notes()]
        self.assertEqual(starts, [0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15])

    def test_mute_and_solo(self):
        self.rig.apply([add_lane(0), add_lane(1, note=61), add_lane(2, note=62)]
                       + [step_on(s, k) for s in range(3) for k in range(16)])
        self.rig.apply([[OP_SET_LANE, 0, L_MUTE, 1], [OP_SET_LANE, 2, L_SOLO, 1], [OP_SET_LANE, 0, L_SOLO, 1]])
        self.rig.seek(0.0)
        self.rig.events.clear()
        self.rig.run(4)
        self.assertEqual(sorted({e[3] for e in self.rig.notes()}), [62])

    def test_muting_a_held_lane_releases_it_at_once(self):
        self.rig.apply([add_lane(0), step_on(0, 0), [OP_SET_STEP, 0, 0, 0, F_GATE, 160000]])
        self.rig.seek(0.0)
        self.rig.run(2)
        self.rig.events.clear()
        self.rig.send([[OP_SET_LANE, 0, L_MUTE, 1]])
        self.rig.run(1)
        self.assertEqual([e[2:4] for e in self.rig.events], [(0x80, 60)])

    def test_stop_releases_held_notes(self):
        self.rig.apply([add_lane(0), step_on(0, 0), [OP_SET_STEP, 0, 0, 0, F_GATE, 160000]])
        self.rig.seek(0.0)
        self.rig.run(2)
        self.rig.events.clear()
        self.rig.block(playing=False)
        self.assertEqual([e[2:4] for e in self.rig.events], [(0x80, 60)])

    def test_audition_plays_and_releases_after_100_ms(self):
        self.rig.apply([add_lane(0)])
        self.rig.events.clear()
        self.rig.session.immediate(OP_AUDITION, 0, 90)
        for _ in range(6):
            self.rig.block(playing=False)
            self.rig.sample += self.rig.block_size
        kinds = [(e[0], e[2], e[3]) for e in self.rig.events if e[3] == 60]
        self.assertEqual(kinds, [(0, 0x90, 60), (4410, 0x80, 60)])

    def test_an_interrupted_transaction_is_aborted(self):
        self.rig.apply([add_lane(0)])
        session = self.rig.session
        session.txn = session.txn + 1
        session.write_entry(session.txn, OP_SET_STEP, self.rig.lua.table_from([0, 0, 0, F_ON, 1]))
        session.set_h(self.rig.wire.H_CMD_PROD, session.prod)
        session.pending[session.txn] = self.rig.lua.table_from({"generation": session.generation})
        session.pending_count = session.pending_count + 1
        txn = self.rig.send([step_on(0, 3)])
        self.rig.run(2, playing=False)
        results, _ = self.rig.results()
        self.assertEqual([(r["txn"], r["code"]) for r in results], [(txn - 1, ABORTED), (txn, OK)])
        self.assertEqual(self.rig.engine.var("nd_count"), 1)

    def test_a_full_ring_refuses_more_commands_and_drops_none(self):
        self.rig.apply([add_lane(0)])
        accepted = 0
        while self.rig.send([step_on(0, accepted % 16, (accepted // 16 + 1) % 2)]):
            accepted += 1
        self.assertEqual(accepted, 2048)
        self.assertFalse(self.rig.send([step_on(0, 0)]))
        self.rig.run(20, playing=False)
        results, _ = self.rig.results()
        self.assertEqual(len(results), 2048)
        self.assertTrue(all(r["code"] == OK for r in results))
        self.assertEqual(self.rig.engine.var("revision"), 2049)

    def test_exhausting_the_page_pool_fails_with_capacity_and_keeps_state(self):
        self.rig.apply([add_lane(s) for s in range(64)])
        total = 0
        for pattern in range(8):
            commands = [[OP_SET_STEP, pattern, s, 16 * page, F_VEL, 90] for s in range(64) for page in range(32)]
            for start in range(0, len(commands), 1024):
                self.assertEqual(self.rig.apply(commands[start:start + 1024], blocks=6), OK)
                total += 1024
        self.assertEqual(self.rig.engine.var("free_top"), 0)
        before = self.rig.engine.var("nd_count")
        self.assertEqual(self.rig.apply([[OP_SET_STEP, 9, 0, 0, F_VEL, 90]]), CAPACITY)
        self.assertEqual(self.rig.engine.var("nd_count"), before)
        self.assertEqual(self.rig.apply([[OP_SET_STEP, 0, 0, 0, F_VEL, 100]]), OK)
        self.assertEqual(self.rig.engine.var("free_top"), 1)
        self.assertEqual(self.rig.apply([[OP_SET_STEP, 9, 0, 0, F_VEL, 90]]), OK)

    def test_state_round_trips_and_undo_replaces_it(self):
        self.rig.apply([add_lane(2, lane_id=777), step_on(2, 7), [OP_SET_STEP, 0, 2, 7, F_VEL, 33],
                        [OP_SET_LENGTH, 0, 2, 12], [OP_SET_LANE, 2, L_PAN, -2500], [OP_SET_RACK, 1, 4242]])
        saved = self.rig.engine.save()
        self.rig.apply([step_on(2, 8), [OP_SET_LANE, 2, L_MUTE, 1]])
        generation = self.rig.session.generation
        self.assertTrue(self.rig.engine.load(saved))
        self.rig.run(1, playing=False)
        results, note = self.rig.results()
        self.assertEqual(note, "replaced")
        self.assertEqual(self.rig.session.generation, generation + 1)
        self.rig.session.request(self.rig.lua.table_from({"pattern": 0, "l0": 0, "nl": 3, "s0": 0, "ns": 16}))
        snap = self.rig.snapshot()
        lane = snap.lanes[2]
        self.assertEqual((lane.id, lane.length, lane.pan, lane.mute), (777, 12, -2500, False))
        self.assertEqual((snap.steps[2][7][F_ON], snap.steps[2][7][F_VEL], snap.steps[2][8][F_ON]), (1, 33, 0))
        self.assertEqual(snap.rack.seed, 4242)
        self.assertEqual(self.rig.engine.save(), saved)

    def test_a_command_from_before_an_undo_is_stale(self):
        self.rig.apply([add_lane(0)])
        saved = self.rig.engine.save()
        txn = self.rig.send([step_on(0, 1)])
        self.rig.engine.load(saved)
        self.rig.run(2, playing=False)
        results, _ = self.rig.results()
        self.assertEqual([(r["txn"], r["code"]) for r in results], [(txn, STALE)])
        self.assertEqual(self.rig.engine.var("nd_count"), 0)

    def test_an_unreadable_state_is_kept_and_written_back(self):
        self.rig.apply([add_lane(0), step_on(0, 1)])
        good = self.rig.engine.save()
        bad = good[:-4]
        self.rig.engine.load(bad)
        self.rig.run(1, playing=False)
        self.assertEqual(self.rig.engine.var("load_error"), 2)
        self.assertEqual(self.rig.engine.save(), bad)
        self.rig.results()
        self.assertEqual(self.rig.apply([step_on(0, 2)]), LOAD_ERROR)
        self.rig.engine.load(good)
        self.rig.run(1, playing=False)
        self.assertEqual(self.rig.engine.var("load_error"), 0)

    def test_reload_releases_the_held_pitch(self):
        self.rig.apply([add_lane(0), step_on(0, 0), [OP_SET_STEP, 0, 0, 0, F_GATE, 160000]])
        self.rig.seek(0.0)
        self.rig.run(2)
        saved = self.rig.engine.save()
        # REAPER builds a new instance when an FX comes back online or a project reopens.
        from harness import jsfx_host
        reloaded = jsfx_host.Effect(SEQCORE)
        try:
            reloaded.load(saved)
            reloaded.time(120, 0, 0)
            events = reloaded.process()
        finally:
            reloaded.close()
        self.assertEqual([e[2:4] for e in events[:3]], [(0x80, 60), (0xB0, 123), (0xB1, 123)])
        self.assertEqual(len(events), 65)


if __name__ == "__main__":
    unittest.main()
