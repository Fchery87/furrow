"""The item gate and SeqCore's mode handoff, run together in ysfx. ysfx is not REAPER.

The invariant is that Live notes and committed-item notes never sound in the same block, in
either processing order. REAPER runs still decide how the two tracks really interleave."""

import random
import unittest

from harness import ROOT, Rig, jsfx_host, missing

SKIP = missing()
GATE = ROOT / "jsfx" / "furrow_itemgate.jsfx"
OP_ADD_LANE, OP_SET_STEP, OP_SET_RACK = 3, 2, 6
R_ID, R_MODE = 0, 3
RACK = 777
ITEM_NOTE = 72


@unittest.skipIf(SKIP, SKIP)
class GateTest(unittest.TestCase):
    def setUp(self):
        self.rig = Rig()
        self.rig.attach()
        self.gate = jsfx_host.Effect(GATE)
        self.gate.set_slider(0, RACK)
        self.passed = []
        self.sample = 0

    def tearDown(self):
        self.gate.close()
        self.rig.close()

    def configure(self, steps=range(16)):
        commands = [[OP_ADD_LANE, 0, 1000, 7, 60, 0], [OP_SET_RACK, R_ID, RACK]]
        commands += [[OP_SET_STEP, 0, 0, s, 0, 1] for s in steps]
        self.rig.apply(commands)

    def set_mode(self, mode):
        self.rig.send([[OP_SET_RACK, R_MODE, mode]])

    def block(self, playing=True, gate_first=True, item_notes=True, engine=True):
        """One block of both devices. Returns (gate events, engine note-ons)."""
        def run_gate():
            self.gate.time(120, 1, 0)
            if item_notes:
                self.gate.send_midi(0x90, ITEM_NOTE, 100, 0)
                self.gate.send_midi(0x80, ITEM_NOTE, 0, 300)
            return self.gate.process(1024)

        def run_engine():
            if not engine:
                return []
            before = len(self.rig.events)
            self.rig.block(playing=playing)
            return [e for e in self.rig.events[before:] if e[2] & 0xF0 == 0x90 and e[4] > 0]

        if gate_first:
            out = run_gate()
            ons = run_engine()
        else:
            ons = run_engine()
            out = run_gate()
        return out, ons

    @staticmethod
    def note_ons(events):
        return [e for e in events if e[2] & 0xF0 == 0x90 and e[4] > 0]

    def test_a_gate_with_no_matching_rack_drops_everything(self):
        self.gate.set_slider(0, 4242)
        self.configure()
        self.set_mode(1)
        for _ in range(6):
            out, _ = self.block(playing=False)
            self.assertEqual(out, [])

    def test_live_mode_keeps_the_gate_shut(self):
        self.configure()
        for _ in range(6):
            out, _ = self.block(playing=False)
            self.assertEqual(out, [])

    def test_items_mode_opens_the_gate_and_passes_notes_and_releases(self):
        self.configure()
        self.set_mode(1)
        outs = [self.block(playing=False)[0] for _ in range(6)]
        self.assertEqual(outs[0], [])
        passed = [e for out in outs for e in out]
        self.assertEqual([(e[2], e[3]) for e in passed][-2:], [(0x90, ITEM_NOTE), (0x80, ITEM_NOTE)])
        self.assertEqual(len(self.note_ons(outs[-1])), 1)

    def test_closing_the_gate_releases_a_note_it_let_through(self):
        self.configure()
        self.set_mode(1)
        for _ in range(4):
            self.block(playing=False)
        self.gate.send_midi(0x90, 80, 100, 0)
        self.gate.time(120, 1, 0)
        self.gate.process(1024)
        self.set_mode(0)
        released = []
        for _ in range(4):
            out, _ = self.block(playing=False, item_notes=False)
            released += [(e[2], e[3]) for e in out]
        self.assertIn((0x80, 80), released)
        self.assertEqual(self.note_ons(self.block(playing=False)[0]), [])

    def test_closing_the_gate_lifts_a_sustain_pedal_the_items_left_down(self):
        self.configure()
        self.set_mode(1)
        for _ in range(4):
            self.block(playing=False)
        self.gate.send_midi(0xB0, 64, 127, 0)
        self.gate.time(120, 1, 0)
        self.gate.process(1024)
        self.set_mode(0)
        pedal = []
        for _ in range(4):
            out, _ = self.block(playing=False, item_notes=False)
            pedal += [(e[2], e[3], e[4]) for e in out if e[2] == 0xB0 and e[3] == 64]
        self.assertEqual(pedal, [(0xB0, 64, 0)])

    def test_a_stalled_engine_closes_the_gate(self):
        self.configure()
        self.set_mode(1)
        for _ in range(6):
            self.block(playing=False)
        opened = self.note_ons(self.block(playing=False, engine=False)[0])
        self.assertEqual(len(opened), 1)
        outs = [self.block(playing=False, engine=False)[0] for _ in range(80)]
        self.assertEqual(self.note_ons(outs[-1]), [])

    def test_live_resumes_only_after_the_gate_has_closed(self):
        self.configure()
        self.set_mode(1)
        for _ in range(6):
            self.block()
        self.set_mode(0)
        live_blocks = []
        for i in range(8):
            out, ons = self.block(item_notes=True)
            live_blocks.append((len(self.note_ons(out)), len(ons)))
        for gate_ons, engine_ons in live_blocks:
            self.assertFalse(gate_ons and engine_ons, live_blocks)

    def test_the_two_sources_never_share_a_block_in_either_order_under_random_switching(self):
        for gate_first in (True, False):
            self.tearDown()
            self.setUp()
            self.configure()
            self.rig.seek(0.0)
            rng = random.Random(7)
            overlaps = []
            engine_total = gate_total = 0
            for n in range(400):
                if n % 7 == 0:
                    self.set_mode(rng.randint(0, 1))
                out, ons = self.block(gate_first=gate_first)
                g = len(self.note_ons(out))
                engine_total += len(ons)
                gate_total += g
                if g and ons:
                    overlaps.append(n)
            self.assertEqual(overlaps, [], gate_first)
            self.assertGreater(engine_total, 20)
            self.assertGreater(gate_total, 20)


if __name__ == "__main__":
    unittest.main()
