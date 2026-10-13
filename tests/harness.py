"""Runs SeqCore in ysfx with the real Lua session module driving it through named gmem."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "jsfx_host"))

import jsfx_host  # noqa: E402

try:
    from lupa import lua54
except ImportError:
    lua54 = None

SEQCORE = ROOT / "jsfx" / "furrow_seqcore.jsfx"
LUA = ROOT / "lua"


def missing():
    if lua54 is None:
        return "lupa is not installed"
    if not jsfx_host.available():
        return "run tools/jsfx_host/build.sh first"
    return None


class Mirror:
    """A copy of the directory and one slot, refreshed after each engine block.

    The engine runs only inside Rig.block, so a refresh after every block is exact."""

    def __init__(self, gmem, slot_base, slot_size):
        self.gmem = gmem
        self.slot_base = slot_base
        self.slot_size = slot_size
        self.directory = []
        self.slot = []
        self.queued = {}
        self.refresh()

    def refresh(self):
        self.directory = self.gmem.read(0, 128)
        self.slot = self.gmem.read(self.slot_base, self.slot_size)

    def read(self, index):
        index = int(index)
        if index < 128:
            return self.directory[index]
        offset = index - self.slot_base
        if 0 <= offset < self.slot_size:
            return self.slot[offset]
        return self.gmem.read(index, 1)[0]

    def write(self, index, value):
        index = int(index)
        self.queued[index] = value
        if index < 128:
            self.directory[index] = value
        elif 0 <= index - self.slot_base < self.slot_size:
            self.slot[index - self.slot_base] = value

    def flush(self):
        """Sends queued writes in index order, in runs, before the engine's next block."""
        run_start, run = None, []
        for index in sorted(self.queued):
            if run and index == run_start + len(run):
                run.append(self.queued[index])
                continue
            if run:
                self.gmem.write(run_start, run)
            run_start, run = index, [self.queued[index]]
        if run:
            self.gmem.write(run_start, run)
        self.queued = {}


def pair(value):
    """Lua returns one value or several. This always gives two."""
    if isinstance(value, tuple):
        return (value + (None, None))[:2]
    return value, None


class LuaObject:
    """Calls Lua methods with the object as self, and reads and writes its fields."""

    def __init__(self, lua, table):
        object.__setattr__(self, "_table", table)
        object.__setattr__(self, "_call", lua.eval("function(o, m, ...) return o[m](o, ...) end"))
        object.__setattr__(self, "_lua", lua)

    def __getattr__(self, name):
        value = self._table[name]
        if self._lua.eval("type")(value) == "function":
            return lambda *args: self._call(self._table, name, *args)
        return value

    def __setattr__(self, name, value):
        self._table[name] = value


class Rig:
    def __init__(self, srate=44100.0, block=1024):
        self.gmem = jsfx_host.Gmem()
        # A fresh directory per rig, because ysfx keeps named gmem for the whole process.
        self.gmem.write(64, [0.0] * 64)
        self.engine = jsfx_host.Effect(SEQCORE, srate, block)
        self.srate = srate
        self.block_size = block
        self.sample = 0
        self.beat = 0.0
        self.events = []
        self.block(playing=False)
        self.slot = int(self.engine.slider(1))
        self.wire = None
        self.lua = lua54.LuaRuntime(unpack_returned_tuples=True, encoding="latin-1")
        self.lua.execute('package.path = "%s/?.lua;" .. package.path' % LUA.as_posix())
        self.wire = self.lua.eval('(require("furrow.wire"))')
        self.mirror = Mirror(self.gmem, self.wire.SLOT_BASE + self.slot * self.wire.SLOT_SIZE, self.wire.SLOT_SIZE)
        mem = self.lua.table_from({"read": self.mirror.read, "write": self.mirror.write})
        session_class = self.lua.eval('(require("furrow.session"))')
        self.session = LuaObject(self.lua, session_class.new(mem, self.slot))

    def close(self):
        self.engine.close()
        self.gmem.close()

    def block(self, playing=True, tempo=120.0, frames=None):
        frames = frames or self.block_size
        if getattr(self, "wire", None) is not None:
            self.mirror.flush()
        self.engine.time(tempo, jsfx_host.PLAYING if playing else jsfx_host.STOPPED, self.beat, self.sample / self.srate)
        for bus, offset, status, data1, data2 in self.engine.process(frames):
            self.events.append((self.sample + offset, bus, status, data1, data2))
        if playing:
            self.beat += tempo / 60.0 * frames / self.srate
            self.sample += frames
        if getattr(self, "wire", None) is not None:
            self.mirror.refresh()

    def run(self, blocks, playing=True, tempo=120.0):
        for _ in range(blocks):
            self.block(playing, tempo)

    def seek(self, beat, sample=None):
        self.beat = beat
        self.sample = int(round(beat * 60.0 / 120.0 * self.srate)) if sample is None else sample

    def attach(self, session_id=101):
        ok, err = pair(self.session.attach(session_id))
        assert ok, err
        self.mirror.refresh()

    def send(self, commands):
        table = self.lua.table_from([self.lua.table_from(c) for c in commands])
        return pair(self.session.send(table))[0]

    def results(self):
        results, note = pair(self.session.poll())
        out = []
        for i in range(1, len(results) + 1):
            r = results[i]
            out.append({"txn": r.txn, "code": r.code, "revision": r.revision, "generation": r.generation})
        return out, note

    def apply(self, commands, blocks=2):
        txn = self.send(commands)
        assert txn, "ring full"
        self.run(blocks, playing=False)
        results, _ = self.results()
        codes = [r["code"] for r in results if r["txn"] == txn]
        return codes[0] if codes else None

    def snapshot(self, blocks=8):
        self.run(blocks, playing=False)
        return self.session.read_snapshot()

    def notes(self, status_high=0x90):
        return [e for e in self.events if e[2] & 0xF0 == status_high and (status_high != 0x90 or e[4] > 0)]
