"""Writes the engine golden traces by running the REAPER edition's SeqCore in ysfx.

Each scenario drives SeqCore through its command ring, as the Lua session does, and records
every block's transport input, every MIDI event, every transaction result, the playhead and
the final saved state. The C++ tests replay the same inputs into furrow::Engine and require
the same output, so SeqCore stays the reference for every V1 rule.

Build the ysfx host once with tools/jsfx_host/build.sh, then run from the repository root:

    python3 plugin/tests/golden/make_golden.py

Trace lines, one record per line:
    rate SRATE BLOCK            sample rate and nominal block size
    C OP A B C D V BEAT         one command staged for the next transaction
    T                           commit the staged commands as one transaction
    A CODE                      result of the last transaction
    I OP A B                    an immediate command (audition or panic)
    L WORD...                   load these state words, as a host state restore does
    B PLAYING TEMPO BEAT FRAMES one block of host transport
    E OFFSET BUS STATUS D1 D2   one MIDI event from the block above
    P PATTERN PENDING POS...    pattern, pending pattern and lane playheads after the block
    S WORD...                   the saved state words at the end
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "jsfx_host"))

import jsfx_host  # noqa: E402

SEQCORE = ROOT / "jsfx" / "furrow_seqcore.jsfx"
OUT = Path(__file__).resolve().parent

# Wire format 1, from jsfx/furrow_seqcore.jsfx.
SLOT_BASE, SLOT_SIZE = 1024, 131072
H_GENERATION, H_PATTERN, H_PENDING, H_SESSION = 2, 10, 11, 13
H_CMD_PROD, H_CMD_CONS, H_ACK_PROD, H_ACK_CONS = 14, 15, 16, 17
PLAYHEAD, RING, RING_SIZE, ENTRY, ACK, ACK_ENTRY = 256, 512, 4096, 10, 41472, 4

OP_COMMIT, SET_STEP, ADD_LANE, REMOVE_LANE, SET_LANE, SET_RACK = 1, 2, 3, 4, 5, 6
SET_LENGTH, AUDITION, PANIC, QUEUE_PATTERN, CLEAR_PATTERN = 7, 8, 9, 10, 11

# Step fields.
ON, VEL, REL, PITCH, FINE, PAN, MOD_A, MOD_B, SHIFT, GATE, RATCHET, CURVE, PROB = range(13)
# Lane fields.
L_PROB_KEY, L_ROW, L_NOTE, L_MUTE, L_SOLO, L_VOLUME, L_PAN, L_BEND, L_SWING = range(2, 11)
L_MOD_A, L_MOD_B = 13, 14
# Rack fields.
R_ID, R_SEED, R_PATTERN, R_MODE, R_SWING, R_TRUNCATE = range(6)

SESSION = 7


def num(x):
    """Writes a double so that strtod reads back the same bits."""
    return repr(float(x)) if x != int(x) else str(int(x))


class Oracle:
    def __init__(self, name, srate=44100.0, block=1024, lanes_shown=4):
        self.name = name
        self.srate = srate
        self.block_size = block
        self.lanes_shown = lanes_shown
        self.lines = ["# furrow golden trace: " + name,
                      "# Generated from jsfx/furrow_seqcore.jsfx by make_golden.py. Do not edit.",
                      "rate %s %d" % (num(srate), block)]
        self.gmem = jsfx_host.Gmem()
        # A fresh slot directory, because ysfx keeps named gmem for the whole process.
        self.gmem.write(64, [0.0] * 64)
        self.fx = jsfx_host.Effect(SEQCORE, srate, block)
        self.beat = 0.0
        self.tempo = 120.0
        self.staged = []
        self.seq = 0
        self.txn = 0
        self.ack_cons = 0
        self.last_p = None
        # The first block claims a slot and sends the startup all-notes-off.
        self.block(False)
        self.slot = int(self.fx.slider(1))
        self.base = SLOT_BASE + self.slot * SLOT_SIZE
        self.h_write(H_SESSION, SESSION)
        self.block(False)
        self.generation = int(self.h(H_GENERATION))
        self.prod = int(self.h(H_CMD_PROD))

    def h(self, index):
        return self.gmem.read(self.base + index, 1)[0]

    def h_write(self, index, value):
        self.gmem.write(self.base + index, [float(value)])

    # Commands -----------------------------------------------------------------------------

    def cmd(self, op, a=0, b=0, c=0, d=0, v=0, beat=0.0):
        wire = (a, b, c, d, v)
        if op == QUEUE_PATTERN:
            ticks = int(round(beat * 1024))
            assert ticks == beat * 1024, "queue beats must be multiples of 1/1024"
            wire = (a, ticks // 16777216, ticks % 16777216, 0, 0)
        self.staged.append((op, wire))
        self.lines.append("C %d %d %d %d %d %d %s" % (op, a, b, c, d, v, num(beat)))

    def step(self, pattern, lane, step, field, value):
        self.cmd(SET_STEP, pattern, lane, step, field, value)

    def add_lane(self, slot, note, lane_id=None, key=None, row=None):
        self.cmd(ADD_LANE, slot, lane_id or slot + 1, key if key is not None else slot + 1, note,
                 slot if row is None else row)

    def entry(self, txn, op, values):
        while self.prod - int(self.h(H_CMD_CONS)) >= RING_SIZE:
            self.block(False)
        self.seq += 1
        at = self.base + RING + (self.prod % RING_SIZE) * ENTRY
        self.gmem.write(at, [SESSION, self.generation, self.seq, txn, op] + [float(x) for x in values])
        self.prod += 1
        self.h_write(H_CMD_PROD, self.prod)

    def commit(self):
        """Sends the staged commands. SeqCore applies them at the start of the next block."""
        self.txn += 1
        for op, wire in self.staged:
            self.entry(self.txn, op, wire)
        self.entry(self.txn, OP_COMMIT, (len(self.staged), 0, 0, 0, 0))
        self.lines.append("T")
        self.staged = []
        self.pending_txn = self.txn

    def immediate(self, op, a=0, b=0):
        self.entry(0, op, (a, b, 0, 0, 0))
        self.lines.append("I %d %d %d" % (op, a, b))

    def read_acks(self):
        prod = int(self.h(H_ACK_PROD))
        while self.ack_cons < prod:
            at = self.base + ACK + (self.ack_cons % RING_SIZE) * ACK_ENTRY
            txn, _revision, code, _generation = self.gmem.read(at, ACK_ENTRY)
            assert int(txn) == self.pending_txn, "unexpected acknowledgement for %d" % txn
            self.lines.append("A %d" % int(code))
            self.ack_cons += 1
        self.h_write(H_ACK_CONS, self.ack_cons)

    def apply(self, playing=False, tempo=None):
        """Commits and runs blocks until the result is known. SeqCore reads 256 ring entries
        a block, so a long transaction takes more than one."""
        self.commit()
        acked = self.ack_cons
        self.block(playing, tempo)
        while self.ack_cons == acked:
            self.block(playing, tempo)

    # Transport ----------------------------------------------------------------------------

    def block(self, playing=True, tempo=None, frames=None, advance=None):
        tempo = self.tempo if tempo is None else tempo
        frames = frames or self.block_size
        self.fx.time(tempo, jsfx_host.PLAYING if playing else jsfx_host.STOPPED, self.beat, 0.0)
        self.lines.append("B %d %s %s %d" % (1 if playing else 0, num(tempo), num(self.beat), frames))
        for bus, offset, status, d1, d2 in self.fx.process(frames):
            self.lines.append("E %d %d %d %d %d" % (offset, bus, status, d1, d2))
        if getattr(self, "base", None) is not None:
            pos = self.gmem.read(self.base + PLAYHEAD, self.lanes_shown)
            line = "P %d %d %s" % (self.h(H_PATTERN), self.h(H_PENDING), " ".join("%d" % p for p in pos))
            # Only changes are written. The replay keeps the last one.
            if line != self.last_p:
                self.lines.append(line)
                self.last_p = line
            if getattr(self, "pending_txn", None) is not None:
                self.read_acks()
            # A state restore starts a new generation, and the Lua session follows it.
            self.generation = int(self.h(H_GENERATION))
        if playing:
            self.beat += advance if advance is not None else tempo / 60.0 * frames / self.srate

    def play(self, blocks, tempo=None, frames=None):
        if tempo is not None:
            self.tempo = tempo
        for _ in range(blocks):
            self.block(True, frames=frames)

    def play_to(self, beat):
        """Plays whole blocks until the playhead reaches the beat."""
        while self.beat < beat:
            self.block(True)

    def align_to(self, beat):
        """Plays one short block so the next block starts exactly on the beat."""
        frames = int(round((beat - self.beat) * 60.0 / self.tempo * self.srate))
        assert 0 < frames <= 8192
        self.block(True, frames=frames, advance=beat - self.beat)

    def stop(self, blocks=1):
        for _ in range(blocks):
            self.block(False)

    def seek(self, beat):
        self.beat = beat

    def ramp(self, tempos):
        """Plays one block per tempo, advancing as REAPER does for a linear ramp in time."""
        for i, tempo in enumerate(tempos):
            nxt = tempos[i + 1] if i + 1 < len(tempos) else tempo
            self.block(True, tempo, advance=(tempo + nxt) * self.block_size / (120.0 * self.srate))
        self.tempo = tempos[-1]

    # State --------------------------------------------------------------------------------

    def words(self):
        data = self.fx.save()
        values = struct.unpack("<%df" % (len(data) // 4), data)
        assert all(v == int(v) for v in values)
        return [int(v) for v in values]

    def load(self, words):
        self.lines.append("L " + " ".join(str(w) for w in words))
        self.fx.load(struct.pack("<%df" % len(words), *words))

    def finish(self):
        self.lines.append("S " + " ".join(str(w) for w in self.words()))
        (OUT / (self.name + ".txt")).write_text("\n".join(self.lines) + "\n")
        self.fx.close()
        self.gmem.close()


# Scenarios ---------------------------------------------------------------------------------

PROBE = [(36, [0, 4, 8, 12]), (38, [2, 6, 10, 14]), (42, [0, 2, 4, 6, 8, 10, 12, 14]), (46, [4, 12, 15])]


def probe_pattern(o):
    for slot, (note, steps) in enumerate(PROBE):
        o.add_lane(slot, note)
    for slot, (note, steps) in enumerate(PROBE):
        for s in steps:
            o.step(0, slot, s, ON, 1)
    o.apply()


def basic(srate, block, tempo):
    o = Oracle("basic-%d-%d-%d" % (srate, block, tempo), srate, block)
    probe_pattern(o)
    # Two passes of the 16-step pattern.
    o.play(int(8 * 60 / tempo * srate / block) + 2, tempo)
    o.stop()
    o.finish()


def polymeter():
    o = Oracle("polymeter", 48000, 512, 6)
    lengths = [16, 12, 5, 7, 1, 33]
    for slot, n in enumerate(lengths):
        o.add_lane(slot, 40 + slot)
    for slot, n in enumerate(lengths):
        o.cmd(SET_LENGTH, 0, slot, n)
        o.step(0, slot, 0, ON, 1)
        if n > 3:
            o.step(0, slot, n - 2, ON, 1)
    o.step(0, 5, 32, ON, 1)
    o.apply()
    o.play(260, 175)
    o.finish()


def step_fields():
    o = Oracle("step-fields", 44100, 256, 4)
    for slot in range(4):
        o.add_lane(slot, 60)
    o.cmd(SET_LANE, 0, L_VOLUME, 7500)
    o.cmd(SET_LANE, 0, L_PAN, -10000)
    o.cmd(SET_LANE, 1, L_PAN, 10000)
    o.cmd(SET_LANE, 1, L_BEND, 12)
    o.cmd(SET_LANE, 2, L_BEND, 0)
    o.cmd(SET_LANE, 3, L_MOD_A, 1)
    o.cmd(SET_LANE, 3, L_MOD_B, 11)
    for s in range(16):
        for slot in range(4):
            o.step(0, slot, s, ON, 1)
    for s, (vel, rel, pitch, fine, pan) in enumerate([
            (1, 0, -24, -100, -64), (127, 127, 24, 100, 63), (64, 30, 5, 50, 20), (100, 64, 0, 0, 0),
            (90, 100, 12, -37, -20), (2, 1, -12, 99, 5), (100, 64, 0, 0, 0), (55, 10, 7, 1, -1)]):
        for slot in range(3):
            o.step(0, slot, s, VEL, vel)
            o.step(0, slot, s, REL, rel)
            o.step(0, slot, s, FINE, fine)
            o.step(0, slot, s, PAN, pan)
        o.step(0, 0, s, PITCH, pitch)
        o.step(0, 1, s, PITCH, -pitch)
    # Lane 2 plays note 60 + pitch; out-of-range results are dropped.
    o.cmd(SET_LANE, 2, L_NOTE, 120)
    o.step(0, 2, 1, PITCH, 24)
    o.step(0, 2, 2, PITCH, 7)
    o.cmd(SET_LANE, 0, L_NOTE, 10)
    o.step(0, 0, 0, PITCH, -24)
    for s, (a, b) in enumerate([(0, 127), (64, 64), (100, 3), (64, 64), (127, 0)]):
        o.step(0, 3, s * 2, MOD_A, a)
        o.step(0, 3, s * 2, MOD_B, b)
    o.apply()
    o.play(200, 133)
    o.stop()
    o.finish()


def swing_shift_gate():
    o = Oracle("swing-shift-gate", 44100, 128, 6)
    for slot in range(6):
        o.add_lane(slot, 36 + slot)
    o.cmd(SET_RACK, R_SWING, 10000)
    o.cmd(SET_LANE, 0, L_SWING, 10000)
    o.cmd(SET_LANE, 1, L_SWING, 5000)
    o.cmd(SET_LANE, 2, L_SWING, 0)
    o.cmd(SET_LANE, 3, L_SWING, 7777)
    o.cmd(SET_LANE, 4, L_SWING, 10000)
    o.cmd(SET_LANE, 5, L_SWING, 10000)
    for s in range(16):
        for slot in range(4):
            o.step(0, slot, s, ON, 1)
        if s % 3 == 0:
            o.step(0, 4, s, ON, 1)
            o.step(0, 4, s, GATE, [500, 10000, 40000, 160000, 25000, 9999][s // 3])
        if s % 2 == 1:
            o.step(0, 5, s, ON, 1)
            o.step(0, 5, s, SHIFT, [0, 2500, 6666, 9900, 5000, 1, 7000, 3333][s // 2])
            o.step(0, 5, s, GATE, [5000, 10000, 3000, 500, 20000, 7000, 6000, 9000][s // 2])
        o.step(0, 3, s, SHIFT, (s * 613) % 9901)
        o.step(0, 3, s, GATE, 500 + (s * 7919) % 30000)
    o.apply()
    o.play(140, 128)
    # Half swing, then truncation.
    o.cmd(SET_RACK, R_SWING, 5000)
    o.commit()
    o.play(100)
    o.cmd(SET_RACK, R_TRUNCATE, 1)
    o.commit()
    o.play(140)
    o.cmd(SET_RACK, R_SWING, 10000)
    o.commit()
    o.play(140)
    o.stop()
    o.finish()


def ratchet():
    o = Oracle("ratchet", 48000, 64, 4)
    for slot in range(4):
        o.add_lane(slot, 50 + slot)
    for s in range(16):
        o.step(0, 0, s, ON, 1)
        o.step(0, 0, s, RATCHET, 1 + s % 8)
        if s % 4 == 0:
            o.step(0, 1, s, ON, 1)
            o.step(0, 1, s, RATCHET, [2, 3, 5, 8][s // 4])
            o.step(0, 1, s, GATE, [10000, 40000, 160000, 500][s // 4])
        if s % 2 == 0:
            o.step(0, 2, s, ON, 1)
            o.step(0, 2, s, RATCHET, 4)
            o.step(0, 2, s, SHIFT, 2500 * (s % 4))
            o.step(0, 2, s, GATE, 15000)
        o.step(0, 3, s, ON, 1 if s % 5 == 0 else 0)
        o.step(0, 3, s, RATCHET, 8)
        o.step(0, 3, s, GATE, 80000)
    o.cmd(SET_RACK, R_SWING, 8000)
    o.cmd(SET_LANE, 2, L_SWING, 10000)
    o.apply()
    o.play(500, 140)
    o.cmd(SET_RACK, R_TRUNCATE, 1)
    o.commit()
    o.play(500)
    o.stop()
    o.finish()


def probability():
    o = Oracle("probability", 44100, 512, 4)
    for slot in range(4):
        o.add_lane(slot, 70 + slot, key=[1, 99, 123456, 16777215][slot])
    for s in range(64):
        for slot in range(4):
            o.step(0, slot, s, ON, 1)
            o.step(0, slot, s, PROB, [50, 25, 75, 1 + (s * 37) % 99][slot])
    for slot in range(4):
        o.cmd(SET_LENGTH, 0, slot, 64)
    for s in range(16):
        o.step(1, 0, s, ON, 1)
        o.step(1, 0, s, PROB, 50)
    o.cmd(SET_RACK, R_SEED, 4242)
    o.apply()
    o.play(110, 150)
    o.cmd(SET_RACK, R_SEED, 16777215)
    o.commit()
    o.play(110)
    o.cmd(SET_RACK, R_SEED, 0)
    o.step(0, 2, 5, PROB, 0)
    o.step(0, 2, 6, PROB, 100)
    o.commit()
    o.play(110)
    o.stop()
    # The project step number takes the floor with a small allowance, so these hits still count
    # as steps 4, 5, 6 and on.
    o.seek(0.999999999999)
    o.cmd(QUEUE_PATTERN, 1, beat=0.0)
    o.commit()
    o.stop(2)
    o.play(60)
    o.stop()
    o.finish()


def live_edits():
    o = Oracle("live-edits", 44100, 1024, 5)
    for slot in range(5):
        o.add_lane(slot, 48 + slot)
    for s in range(16):
        for slot in range(5):
            o.step(0, slot, s, ON, 1)
            o.step(0, slot, s, GATE, 30000)
        o.step(0, 3, s, FINE, 40 - s * 5)
        o.step(0, 3, s, MOD_A, s * 8)
        o.step(0, 4, s, MOD_B, 127 - s * 8)
    o.cmd(SET_LANE, 3, L_BEND, 2)
    o.apply()
    o.play(12)
    o.cmd(SET_LANE, 0, L_MUTE, 1)
    o.commit()
    o.play(6)
    o.cmd(SET_LANE, 1, L_SOLO, 1)
    o.commit()
    o.play(6)
    o.cmd(SET_LANE, 0, L_SOLO, 1)
    o.commit()
    o.play(6)
    o.cmd(SET_LANE, 1, L_SOLO, 0)
    o.cmd(SET_LANE, 0, L_SOLO, 0)
    o.cmd(SET_LANE, 0, L_MUTE, 0)
    o.commit()
    o.play(6)
    o.cmd(SET_LANE, 2, L_NOTE, 90)
    o.commit()
    o.play(5)
    o.cmd(SET_LANE, 3, L_BEND, 24)
    o.cmd(SET_LANE, 3, L_MOD_A, 20)
    o.commit()
    o.play(5)
    o.cmd(SET_LANE, 4, L_MOD_B, 21)
    o.cmd(SET_LANE, 3, L_MUTE, 1)
    o.commit()
    o.play(5)
    o.cmd(SET_LENGTH, 0, 2, 3)
    o.step(0, 1, 7, ON, 0)
    o.step(0, 1, 8, VEL, 12)
    o.commit()
    o.play(5)
    o.cmd(REMOVE_LANE, 4)
    o.cmd(REMOVE_LANE, 1)
    o.commit()
    o.play(5)
    o.add_lane(1, 30)
    o.step(0, 1, 3, ON, 1)
    o.commit()
    o.play(8)
    o.stop()
    o.finish()


def pattern_switch():
    o = Oracle("pattern-switch", 44100, 333, 3)
    for slot in range(3):
        o.add_lane(slot, 60 + slot)
    for p, steps in enumerate([[0, 4, 8, 12], [1, 3, 9], [0, 1, 2, 3, 4, 5, 6, 7]]):
        for slot in range(3):
            for s in steps:
                o.step(p, slot, (s + slot) % 16, ON, 1)
                o.step(p, slot, (s + slot) % 16, GATE, 20000)
    o.cmd(SET_LENGTH, 1, 2, 6)
    o.apply()
    o.play_to(6.0)
    # A switch on a bar line, inside a block. The outgoing notes end at the boundary.
    o.cmd(QUEUE_PATTERN, 1, beat=8.0)
    o.commit()
    o.play_to(12.0)
    # A boundary already passed switches at once without replaying old steps.
    o.cmd(QUEUE_PATTERN, 2, beat=10.0)
    o.commit()
    o.play_to(13.0)
    # Queueing the current pattern cancels a pending switch.
    o.cmd(QUEUE_PATTERN, 0, beat=16.0)
    o.commit()
    o.play_to(14.0)
    o.cmd(QUEUE_PATTERN, 2, beat=16.0)
    o.commit()
    o.play_to(17.0)
    # A switch off the grid, inside a block.
    o.cmd(QUEUE_PATTERN, 1, beat=22.3125)
    o.commit()
    o.play_to(25.0)
    # A switch exactly at the start of a block.
    o.cmd(QUEUE_PATTERN, 0, beat=28.0)
    o.commit()
    o.play_to(27.9)
    o.align_to(28.0)
    o.play_to(30.0)
    # Setting the pattern directly releases at once and keeps the phase.
    o.cmd(SET_RACK, R_PATTERN, 2)
    o.commit()
    o.play_to(32.0)
    # Stopping with a switch queued applies it at the stop, from the cursor.
    o.cmd(QUEUE_PATTERN, 1, beat=100.0)
    o.commit()
    o.play(3)
    o.stop(2)
    o.play_to(35.0)
    # A stopped switch is immediate and starts the pattern at the cursor.
    o.stop()
    o.seek(3.0)
    o.cmd(QUEUE_PATTERN, 2, beat=0.0)
    o.commit()
    o.stop(2)
    o.play_to(6.0)
    o.cmd(CLEAR_PATTERN, 2)
    o.commit()
    o.play_to(8.0)
    o.stop()
    # A switch where rounding puts the step a hair before the boundary: the allowance makes the
    # switch land first, so the outgoing pattern does not play there.
    o.seek(0.999999999999)
    o.cmd(QUEUE_PATTERN, 0, beat=0.0)
    o.commit()
    o.stop(2)
    o.play_to(8.5)
    o.cmd(QUEUE_PATTERN, 1, beat=9.0)
    o.commit()
    o.play_to(10.0)
    o.stop()
    o.finish()


def transport():
    o = Oracle("transport", 44100, 1024, 4)
    probe_pattern(o)
    o.cmd(SET_LENGTH, 0, 3, 11)
    o.commit()
    o.play(20)
    o.seek(1.0)
    o.play(10)
    # Loop wrap.
    for _ in range(4):
        o.seek(2.5)
        o.play(9)
    o.stop(3)
    o.seek(1.0000000000001)
    o.play(6)
    # Seeks of a block and a half, forward and back, are seeks too.
    step = 120.0 / 60.0 * 1024 / 44100
    o.seek(o.beat + 1.5 * step)
    o.play(4)
    o.seek(o.beat - 1.5 * step)
    o.play(4)
    o.stop()
    # Pre-roll from before the origin.
    o.seek(-1.3)
    o.play(30)
    o.stop()
    # Restart where it stopped.
    o.play(10)
    # Small and odd blocks.
    for frames in [1, 7, 64, 100, 513, 1, 2048, 4096, 3]:
        o.block(True, frames=frames)
    o.play(20, frames=97)
    o.stop()
    o.finish()


def tempo():
    o = Oracle("tempo", 48000, 480, 4)
    probe_pattern(o)
    o.play(30, 120)
    o.ramp([120.0 + i * 1.5 for i in range(80)])
    o.ramp([238.5 - i * 2.25 for i in range(80)])
    o.play(20, 60)
    o.play(20, 200)
    o.play(20, 97.25)
    # A tempo change that does not fit a ramp is treated as a step change.
    o.block(True, 150, advance=0.25)
    o.play(20, 150)
    o.stop()
    o.finish()


def audition_panic():
    o = Oracle("audition-panic", 44100, 256, 3)
    for slot in range(3):
        o.add_lane(slot, 60 + slot)
    o.cmd(SET_LANE, 1, L_VOLUME, 3000)
    o.cmd(SET_LANE, 1, L_PAN, 2500)
    o.step(0, 0, 0, ON, 1)
    o.step(0, 0, 0, GATE, 160000)
    o.apply()
    o.immediate(AUDITION, 1, 99)
    o.stop(25)
    o.immediate(AUDITION, 2, 1)
    o.stop(3)
    o.immediate(AUDITION, 2, 127)
    o.stop(20)
    # Ignored: inactive lane, bad velocity.
    o.immediate(AUDITION, 9, 100)
    o.immediate(AUDITION, 0, 0)
    o.stop(2)
    o.play(30)
    # Ignored while playing.
    o.immediate(AUDITION, 1, 100)
    o.play(3)
    o.immediate(PANIC)
    o.play(30)
    o.stop()
    o.immediate(PANIC)
    o.stop()
    o.finish()


def lanes_64():
    o = Oracle("lanes-64", 44100, 1024, 64)
    for slot in range(64):
        o.add_lane(slot, slot + 20, row=63 - slot)
    for slot in range(64):
        o.cmd(SET_LENGTH, 0, slot, 1 + slot % 16)
        o.step(0, slot, slot % (1 + slot % 16), ON, 1)
    o.cmd(SET_LANE, 63, L_SOLO, 1)
    o.cmd(SET_LANE, 17, L_SOLO, 1)
    o.apply()
    o.play(40)
    o.cmd(SET_LANE, 63, L_SOLO, 0)
    o.cmd(SET_LANE, 17, L_SOLO, 0)
    o.commit()
    o.play(40)
    o.stop()
    o.finish()


def invalid_commands():
    o = Oracle("invalid-commands", 44100, 1024, 2)
    o.add_lane(0, 60)
    o.apply()
    bad = [
        (SET_STEP, 0, 1, 0, ON, 1), (SET_STEP, 256, 0, 0, ON, 1), (SET_STEP, 0, 0, 512, ON, 1),
        (SET_STEP, 0, 0, 0, 16, 1), (SET_STEP, 0, 0, 0, VEL, 0), (SET_STEP, 0, 0, 0, VEL, 128),
        (SET_STEP, 0, 0, 0, GATE, 499), (SET_STEP, 0, 0, 0, RATCHET, 9), (SET_STEP, 0, 0, 0, PITCH, -25),
        (SET_STEP, 0, -1, 0, ON, 1), (ADD_LANE, 0, 5, 5, 60, 0), (ADD_LANE, 64, 5, 5, 60, 0),
        (ADD_LANE, 1, 0, 5, 60, 0), (ADD_LANE, 1, 5, 5, 128, 0), (ADD_LANE, 1, 5, 5, 60, 64),
        (REMOVE_LANE, 1), (SET_LANE, 1, L_NOTE, 60), (SET_LANE, 0, 1, 5), (SET_LANE, 0, 15, 0),
        (SET_LANE, 0, L_NOTE, 128), (SET_LANE, 0, L_SWING, 10001), (SET_LANE, 0, 11, 2),
        (SET_LANE, 0, 12, 1), (SET_RACK, 6, 0), (SET_RACK, R_ID, 0), (SET_RACK, R_PATTERN, 256),
        (SET_RACK, R_MODE, 2), (SET_RACK, R_SWING, 10001), (SET_LENGTH, 0, 0, 0), (SET_LENGTH, 0, 0, 513),
        (SET_LENGTH, 0, 1, 16), (CLEAR_PATTERN, 256), (12, 0, 0, 0, 0, 0), (AUDITION, 0, 100),
    ]
    for c in bad:
        # Each bad command follows a good one, so the whole transaction must be refused.
        o.step(0, 0, 3, ON, 1)
        o.cmd(*c)
        o.apply()
    # A lane added earlier in the same transaction can be configured by a later command.
    o.add_lane(1, 61)
    o.cmd(SET_LANE, 1, L_NOTE, 62)
    o.step(0, 1, 0, ON, 1)
    o.cmd(REMOVE_LANE, 1)
    o.add_lane(1, 63)
    o.apply()
    o.cmd(SET_RACK, R_MODE, 1)
    o.cmd(SET_RACK, R_ID, 99)
    o.apply()
    o.cmd(SET_RACK, R_MODE, 0)
    o.apply()
    o.play(20)
    o.stop()
    o.finish()


def state_load():
    o = Oracle("state-load", 44100, 1024, 4)
    probe_pattern(o)
    o.cmd(SET_RACK, R_SEED, 77)
    o.cmd(SET_RACK, R_SWING, 3000)
    o.cmd(SET_LANE, 1, L_SWING, 10000)
    o.cmd(SET_LENGTH, 3, 2, 9)
    o.step(5, 3, 500, PITCH, -3)
    o.cmd(QUEUE_PATTERN, 0, beat=0.0)
    o.apply()
    o.play(10)
    saved = o.words()
    # A restore while playing releases held notes and keeps playing the restored state.
    o.load(saved)
    o.play(10)
    # A restore with a held pitch saved in it.
    held = list(saved)
    o.stop()
    # Unreadable states leave the engine silent, refuse edits and are written back unchanged.
    for bad in [held[:-1], [4617586, 2] + held[2:], held[:12] + [65] + held[13:],
                held + [0], [1, 2, 3], held[:5] + [10001] + held[6:]]:
        o.load(bad)
        o.play(5)
        o.step(0, 0, 1, ON, 1)
        o.apply(True)
        o.play(3)
        o.stop()
    o.load([])
    o.play(5)
    o.load(saved)
    o.play(20)
    o.stop()
    o.finish()


SCENARIOS = [
    lambda: basic(44100, 1024, 120), lambda: basic(48000, 256, 200), lambda: basic(96000, 64, 180),
    lambda: basic(44100, 4096, 97), polymeter, step_fields, swing_shift_gate, ratchet, probability,
    live_edits, pattern_switch, transport, tempo, audition_panic, lanes_64, invalid_commands, state_load,
]

if __name__ == "__main__":
    if not jsfx_host.available():
        sys.exit("run tools/jsfx_host/build.sh first")
    for scenario in SCENARIOS:
        scenario()
    print("wrote %d traces to %s" % (len(SCENARIOS), OUT))
