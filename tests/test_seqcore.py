"""SeqCore and the Lua session, run together in ysfx. ysfx is not REAPER.

A pass shows the EEL2 code and the protocol agree. REAPER runs still decide host behaviour."""

import re
import unittest

from harness import LUA, SEQCORE, Rig, missing

SKIP = missing()

OP_COMMIT, OP_SET_STEP, OP_ADD_LANE, OP_REMOVE_LANE, OP_SET_LANE, OP_SET_RACK, OP_SET_LENGTH = 1, 2, 3, 4, 5, 6, 7
OP_AUDITION = 8
OP_QUEUE_PATTERN, OP_CLEAR_PATTERN = 10, 11
OK, STALE, INVALID, CAPACITY, ABORTED, LOAD_ERROR = 1, 2, 3, 4, 5, 6
L_MUTE, L_SOLO, L_VOLUME, L_PAN = 5, 6, 7, 8
F_ON, F_VEL, F_REL, F_FINE, F_PAN, F_MOD_A, F_MOD_B, F_SHIFT, F_GATE, F_RATCHET, F_PROB = 0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 12
L_MOD_A_CC, L_MOD_B_CC = 74, 71
R_SEED, R_SWING, R_TRUNCATE = 1, 4, 5
L_SWING = 10
SAMPLES_PER_STEP = 5512.5  # a sixteenth at 120 BPM and 44.1 kHz


def add_lane(slot, lane_id=None, note=60, row=None):
    return [OP_ADD_LANE, slot, lane_id or 1000 + slot, 7 + slot, note, slot if row is None else row]


def step_on(slot, step, value=1, pattern=0):
    return [OP_SET_STEP, pattern, slot, step, F_ON, value]


PROB_M, PROB_A, PROB_B, PROB_C = 67108859, 40503817, 60251237, 9301841
PROB_VECTORS = [
    ((0, 0, 0, 0), 59528724), ((0, 0, 0, 1), 30014588), ((1, 0, 0, 0), 10390278),
    ((0, 1, 0, 0), 63793128), ((0, 0, 1, 0), 27661453), ((12345, 7, 999999, 123456), 58092903),
    ((16777215, 255, 16777215, 67108858), 58439087),
]


def prob_roll(seed, pattern_key, lane_key, n):
    """The reference hash. SeqCore and lua/furrow/prob.lua must match it bit for bit."""
    h = 0
    for x in (seed, pattern_key, lane_key, n % PROB_M):
        h = (h + x + 1) % PROB_M
        h = (h * PROB_A) % PROB_M
        h = (h * h + PROB_C) % PROB_M
        h = (h * PROB_B) % PROB_M
    return (h * h + PROB_C) % PROB_M


@unittest.skipIf(SKIP, SKIP)
class WireTest(unittest.TestCase):
    def test_lua_constants_match_the_engine(self):
        engine = dict(re.findall(r"^([A-Z][A-Z0-9_]*) = (-?\d+);", SEQCORE.read_text(), re.M))
        lua = dict(re.findall(r"^  ([A-Z][A-Z0-9_]*) = (-?\d+),", (LUA / "furrow" / "wire.lua").read_text(), re.M))
        shared = sorted(set(engine) & set(lua))
        self.assertGreater(len(shared), 90)
        self.assertEqual({k: lua[k] for k in shared}, {k: engine[k] for k in shared})
        defaults = re.search(r"^table16\(DEF, ([^)]*)\);", SEQCORE.read_text(), re.M).group(1)
        wire_defaults = re.search(r"STEP_DEFAULTS = \{ \[0\] = ([^}]*) \}", (LUA / "furrow" / "wire.lua").read_text())
        self.assertEqual([int(x) for x in wire_defaults.group(1).split(",")], [int(x) for x in defaults.split(",")])
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

    def note_ons(self, bus=0, channel=0):
        return [e[0] for e in self.rig.events if e[1] == bus and e[2] == 0x90 + channel and e[4] > 0]

    def note_offs(self, bus=0, channel=0):
        return [e[0] for e in self.rig.events if e[1] == bus and e[2] == 0x80 + channel and e[0] > 0]

    def play(self, blocks):
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(blocks)

    def assert_samples(self, got, want):
        self.assertEqual(len(got), len(want), (got, want))
        for g, w in zip(got, want):
            self.assertLessEqual(abs(g - w), 1, (got, want))

    def test_swing_delays_every_second_step_by_a_third_of_a_step_times_both_amounts(self):
        lanes = [add_lane(0), add_lane(1, note=61), add_lane(2, note=62)]
        steps = [step_on(slot, 1) for slot in range(3)]
        tune = [[OP_SET_LANE, 1, L_SWING, 0], [OP_SET_LANE, 2, L_SWING, 5000], [OP_SET_RACK, R_SWING, 10000]]
        self.rig.apply(lanes + steps + tune)
        self.play(10)
        third = SAMPLES_PER_STEP / 3
        self.assert_samples(self.note_ons(0, 0), [SAMPLES_PER_STEP + third])
        self.assert_samples(self.note_ons(0, 1), [SAMPLES_PER_STEP])
        self.assert_samples(self.note_ons(0, 2), [SAMPLES_PER_STEP + third / 2])

    def test_global_swing_zero_leaves_steps_on_the_grid_and_even_steps_never_swing(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), step_on(0, 2), [OP_SET_RACK, R_SWING, 0]])
        self.play(14)
        self.assert_samples(self.note_ons(), [0, SAMPLES_PER_STEP, 2 * SAMPLES_PER_STEP])
        self.rig.apply([[OP_SET_RACK, R_SWING, 10000]])
        self.play(14)
        third = SAMPLES_PER_STEP / 3
        self.assert_samples(self.note_ons(), [0, SAMPLES_PER_STEP + third, 2 * SAMPLES_PER_STEP])

    def test_truncation_ends_a_swung_note_at_the_next_step_boundary(self):
        self.rig.apply([add_lane(0), step_on(0, 1), [OP_SET_STEP, 0, 0, 1, F_GATE, 10000],
                        [OP_SET_RACK, R_SWING, 10000]])
        self.play(14)
        third = SAMPLES_PER_STEP / 3
        self.assert_samples(self.note_offs(), [SAMPLES_PER_STEP + third + SAMPLES_PER_STEP])
        self.rig.apply([[OP_SET_RACK, R_TRUNCATE, 1]])
        self.play(14)
        self.assert_samples(self.note_offs(), [2 * SAMPLES_PER_STEP])

    def test_shift_delays_a_step_by_its_fraction_of_a_step_after_swing(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1),
                        [OP_SET_STEP, 0, 0, 0, F_SHIFT, 5000], [OP_SET_STEP, 0, 0, 1, F_SHIFT, 3000],
                        [OP_SET_RACK, R_SWING, 10000]])
        self.play(14)
        self.assert_samples(self.note_ons(), [SAMPLES_PER_STEP / 2, SAMPLES_PER_STEP * (1 + 1 / 3 + 0.3)])
        self.assert_samples(self.note_offs()[:1], [SAMPLES_PER_STEP])

    def test_a_truncated_step_pushed_to_its_boundary_by_swing_and_shift_is_dropped(self):
        self.rig.apply([add_lane(0), step_on(0, 1), [OP_SET_STEP, 0, 0, 1, F_SHIFT, 7000],
                        [OP_SET_RACK, R_SWING, 10000]])
        self.play(14)
        self.assert_samples(self.note_ons(), [SAMPLES_PER_STEP * (1 + 1 / 3 + 0.7)])
        self.rig.apply([[OP_SET_RACK, R_TRUNCATE, 1]])
        self.play(14)
        self.assertEqual(self.note_ons(), [])

    def test_ratchets_split_the_gate_into_equal_hits_each_released_by_its_interval(self):
        self.rig.apply([add_lane(0), step_on(0, 0), [OP_SET_STEP, 0, 0, 0, F_RATCHET, 4],
                        [OP_SET_STEP, 0, 0, 0, F_GATE, 10000]])
        self.play(14)
        quarter = SAMPLES_PER_STEP / 4
        self.assert_samples(self.note_ons(), [0, quarter, 2 * quarter, 3 * quarter])
        self.assert_samples(self.note_offs(), [quarter, 2 * quarter, 3 * quarter, 4 * quarter])
        self.rig.apply([[OP_SET_STEP, 0, 0, 0, F_GATE, 5000]])
        self.play(14)
        eighth = SAMPLES_PER_STEP / 8
        self.assert_samples(self.note_ons(), [0, eighth, 2 * eighth, 3 * eighth])
        self.assert_samples(self.note_offs(), [eighth, 2 * eighth, 3 * eighth, 4 * eighth])

    def test_a_later_playing_step_cancels_the_ratchets_still_waiting(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), [OP_SET_STEP, 0, 0, 0, F_RATCHET, 4],
                        [OP_SET_STEP, 0, 0, 0, F_GATE, 10000], [OP_SET_STEP, 0, 0, 0, F_SHIFT, 9000]])
        self.play(14)
        self.assert_samples(self.note_ons(), [0.9 * SAMPLES_PER_STEP, SAMPLES_PER_STEP])
        self.rig.apply([[OP_SET_STEP, 0, 0, 1, F_ON, 0]])
        self.play(14)
        self.assert_samples(self.note_ons(), [0.9 * SAMPLES_PER_STEP, 1.15 * SAMPLES_PER_STEP,
                                              1.4 * SAMPLES_PER_STEP, 1.65 * SAMPLES_PER_STEP])

    def test_ratchets_play_the_same_across_small_blocks(self):
        self.rig.apply([add_lane(0), step_on(0, 0), [OP_SET_STEP, 0, 0, 0, F_RATCHET, 8],
                        [OP_SET_STEP, 0, 0, 0, F_GATE, 10000], [OP_SET_STEP, 0, 0, 0, F_SHIFT, 2500]])
        self.rig.events.clear()
        self.rig.seek(0.0)
        for _ in range(130):
            self.rig.block(frames=64)
        eighth = SAMPLES_PER_STEP / 8
        self.assert_samples(self.note_ons(), [SAMPLES_PER_STEP * 0.25 + k * eighth for k in range(8)])

    def controller_events(self):
        return [(e[2] & 0xF0, e[3], e[4]) for e in self.rig.events
                if e[2] & 0xF0 in (0xB0, 0xE0) and e[3] not in (7, 10, 123)]

    def test_a_plain_pattern_sends_no_bend_or_modulation(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1)])
        self.play(14)
        self.assertEqual(self.controller_events(), [])
        self.assertEqual([e[4] for e in self.rig.notes(0x80)], [64, 64])

    def test_release_velocity_rides_the_note_off(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), [OP_SET_STEP, 0, 0, 0, F_REL, 100]])
        self.play(14)
        self.assertEqual([e[4] for e in self.rig.notes(0x80)], [100, 64])

    def test_step_pan_offsets_the_lane_pan_and_clamps(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), [OP_SET_STEP, 0, 0, 0, F_PAN, 10],
                        [OP_SET_STEP, 0, 0, 1, F_PAN, -64]])
        self.play(14)
        pans = [e[4] for e in self.rig.events if e[2] == 0xB0 and e[3] == 10]
        self.assertEqual(pans, [74, 0])

    def test_fine_pitch_sends_bend_only_when_it_changes_and_resets_on_stop(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), step_on(0, 2), step_on(0, 3),
                        [OP_SET_STEP, 0, 0, 0, F_FINE, 50], [OP_SET_STEP, 0, 0, 1, F_FINE, 50],
                        [OP_SET_STEP, 0, 0, 2, F_FINE, -100]])
        self.play(20)
        # 50 cents of a 2 semitone range is a quarter of the range: 8192 + 2048 = 10240.
        # -100 cents is 8192 - 4096 = 4096. The step after it returns to centre, 8192.
        bends = [(e[3], e[4]) for e in self.rig.events if e[2] == 0xE0]
        self.assertEqual(bends, [(0, 80), (0, 32), (0, 64)])
        self.rig.apply([[OP_SET_STEP, 0, 0, 3, F_FINE, 100]])
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(20)
        self.rig.events.clear()
        self.rig.block(playing=False)
        self.assertEqual([(e[3], e[4]) for e in self.rig.events if e[2] == 0xE0], [(0, 64)])

    def test_mod_a_and_b_send_the_lane_controllers_when_a_step_moves_them(self):
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 1), step_on(0, 2),
                        [OP_SET_STEP, 0, 0, 0, F_MOD_A, 100], [OP_SET_STEP, 0, 0, 1, F_MOD_B, 20]])
        self.play(14)
        sent = [(e[3], e[4]) for e in self.rig.events if e[2] == 0xB0 and e[3] in (74, 71)]
        self.assertEqual(sent, [(74, 100), (74, 64), (71, 20), (71, 64)])

    def test_a_swung_hit_that_crosses_a_block_edge_plays_once_in_the_next_block(self):
        self.rig.apply([add_lane(0), step_on(0, 1), [OP_SET_RACK, R_SWING, 10000]])
        # The hit lands at 7,350 samples. Blocks of 64 frames put several edges before it.
        self.rig.events.clear()
        self.rig.seek(0.0)
        for _ in range(130):
            self.rig.block(frames=64)
        self.assert_samples(self.note_ons(), [SAMPLES_PER_STEP + SAMPLES_PER_STEP / 3])

    def test_the_hash_matches_its_vectors_in_python_and_in_the_lua_port(self):
        prob = self.rig.lua.eval('(require("furrow.prob"))')
        for args, want in PROB_VECTORS:
            self.assertEqual(prob_roll(*args), want)
            self.assertEqual(prob.roll(*args), want)

    def test_probability_plays_the_steps_the_reference_hash_picks_and_repeats(self):
        every = [step_on(0, n) for n in range(16)] + [step_on(1, n) for n in range(16)] + \
                [step_on(2, n) for n in range(16)]
        half = [[OP_SET_STEP, 0, 0, n, F_PROB, 50] for n in range(16)]
        never = [[OP_SET_STEP, 0, 1, n, F_PROB, 0] for n in range(16)]
        self.rig.apply([add_lane(0), add_lane(1, note=61), add_lane(2, note=62)] + every + half + never +
                       [[OP_SET_RACK, R_SEED, 12345]], blocks=4)
        blocks = 350
        self.play(blocks)
        limit = blocks * 1024
        steps = lambda: [round(x / SAMPLES_PER_STEP) for x in self.note_ons(0, 0)]
        first = steps()
        want = [n for n in range(int(limit / SAMPLES_PER_STEP) + 1)
                if n * SAMPLES_PER_STEP < limit and prob_roll(12345, 0, 7, n) * 100 < 50 * PROB_M]
        self.assertEqual(first, want)
        self.assertGreater(len(first), 15)
        self.assertLess(len(first), 50)
        self.assertEqual(self.note_ons(0, 1), [])
        self.assertEqual(len(self.note_ons(0, 2)), len([n for n in range(int(limit / SAMPLES_PER_STEP) + 1)
                                                         if n * SAMPLES_PER_STEP < limit]))
        self.play(blocks)
        self.assertEqual(steps(), first)
        self.rig.apply([[OP_SET_RACK, R_SEED, 999]], blocks=4)
        self.play(blocks)
        rerolled = steps()
        self.assertNotEqual(rerolled, first)
        self.assertEqual(rerolled, [n for n in range(int(limit / SAMPLES_PER_STEP) + 1)
                                     if n * SAMPLES_PER_STEP < limit and prob_roll(999, 0, 7, n) * 100 < 50 * PROB_M])

    @staticmethod
    def queue(pattern, beat):
        ticks = round(beat * 1024)
        return [OP_QUEUE_PATTERN, pattern, ticks // 16777216, ticks % 16777216]

    def two_patterns(self):
        # Pattern 0 plays step 0 only. Pattern 1 plays steps 0 and 2.
        self.rig.apply([add_lane(0), step_on(0, 0), step_on(0, 0, pattern=1), step_on(0, 2, pattern=1)])

    def queue_while_playing(self, pattern, beat, blocks=0):
        """Sends a queue command during playback. A stopped engine applies a switch at once."""
        self.rig.send([self.queue(pattern, beat)])
        self.rig.run(blocks)

    def test_a_queued_switch_lands_on_its_boundary_inside_a_block(self):
        self.two_patterns()
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.queue_while_playing(1, 4.0, blocks=1)
        self.assertEqual(self.rig.engine.var("pend_pattern"), 1)
        self.rig.run(209)
        beat = 22050
        # The old pattern plays step 0 at beat 0. The new one starts at beat 4, which is 88,200
        # samples and falls inside a 1,024 frame block, then adds step 2 half a beat later.
        self.assert_samples(self.note_ons(), [0, 4 * beat, 4.5 * beat, 8 * beat, 8.5 * beat])
        self.assertEqual((self.rig.engine.var("pattern"), self.rig.engine.var("pend_pattern")), (1, -1))
        self.assertEqual(self.rig.engine.var("origin"), 4.0)

    def test_a_switch_releases_the_outgoing_note_at_the_boundary(self):
        self.rig.apply([add_lane(0), [OP_SET_STEP, 0, 0, 0, F_GATE, 160000], step_on(0, 0),
                        [OP_SET_STEP, 1, 0, 0, F_ON, 1]])
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.queue_while_playing(1, 1.0, blocks=60)
        # The old note has a 16 step gate and would run on. The switch ends it on the boundary, and
        # the new pattern's own note ends half a step after it starts, at the default gate.
        self.assert_samples(self.note_offs(), [22050, 22050 + SAMPLES_PER_STEP / 2])
        self.assert_samples(self.note_ons()[:2], [0, 22050])

    def test_a_boundary_already_passed_switches_without_replaying_old_steps(self):
        self.rig.apply([add_lane(0)] + [step_on(0, n, pattern=1) for n in range(16)])
        self.rig.seek(3.0)
        self.rig.events.clear()
        self.queue_while_playing(1, 1.0, blocks=8)
        ons = self.note_ons()
        self.assertEqual(len(ons), len(set(ons)))
        self.assertTrue(all(t >= 3.0 * 22050 - 1 for t in ons), ons)
        self.assertEqual(self.rig.engine.var("pattern"), 1)

    def test_queueing_the_current_pattern_cancels_a_pending_switch(self):
        self.two_patterns()
        self.rig.seek(0.0)
        self.queue_while_playing(1, 40.0, blocks=2)
        self.assertEqual(self.rig.engine.var("pend_pattern"), 1)
        self.queue_while_playing(0, 0.0, blocks=2)
        self.assertEqual(self.rig.engine.var("pend_pattern"), -1)
        self.rig.run(210)
        self.assertEqual(self.rig.engine.var("pattern"), 0)

    def test_a_stopped_switch_is_immediate_and_starts_the_pattern_at_the_cursor(self):
        self.two_patterns()
        self.rig.seek(6.25)
        self.assertEqual(self.rig.apply([self.queue(1, 99.0)]), OK)
        self.assertEqual((self.rig.engine.var("pattern"), self.rig.engine.var("pend_pattern")), (1, -1))
        self.assertEqual(self.rig.engine.var("origin"), 6.25)
        self.rig.events.clear()
        self.rig.run(20)
        start = 6.25 * 22050
        self.assert_samples(self.note_ons()[:2], [start, start + 0.5 * 22050])

    def test_stopping_with_a_queued_switch_applies_it_at_the_stop(self):
        self.two_patterns()
        self.rig.seek(0.0)
        self.queue_while_playing(1, 400.0, blocks=4)
        self.assertEqual(self.rig.engine.var("pend_pattern"), 1)
        self.rig.run(2, playing=False)
        self.assertEqual((self.rig.engine.var("pattern"), self.rig.engine.var("pend_pattern")), (1, -1))

    def test_clearing_a_pattern_frees_its_pages_lengths_and_values_and_keeps_others(self):
        self.rig.apply([add_lane(0), add_lane(1)])
        free = self.rig.engine.var("free_top")
        self.rig.apply([[OP_SET_STEP, 3, 0, 20, F_VEL, 90], step_on(1, 5, pattern=3), step_on(0, 1, pattern=0),
                        [OP_SET_LENGTH, 3, 0, 40]])
        self.assertEqual(self.rig.engine.var("nd_count"), 3)
        self.assertEqual(self.rig.engine.var("len_count"), 1)
        self.assertEqual(self.rig.apply([[OP_CLEAR_PATTERN, 3]]), OK)
        self.assertEqual(self.rig.engine.var("nd_count"), 1)
        self.assertEqual(self.rig.engine.var("len_count"), 0)
        self.assertEqual(self.rig.engine.var("free_top"), free - 1)
        self.assertEqual(self.rig.apply([[OP_CLEAR_PATTERN, 256]]), INVALID)

    def test_the_pending_pattern_reaches_the_header(self):
        self.two_patterns()
        self.rig.seek(0.0)
        self.queue_while_playing(1, 400.0, blocks=4)
        self.assertEqual(self.rig.session.h(self.rig.wire.H_PENDING_PATTERN), 1)


if __name__ == "__main__":
    unittest.main()
