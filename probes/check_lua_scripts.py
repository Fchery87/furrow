#!/usr/bin/env python3
"""Run the probe Lua scripts against a stub REAPER API.

Needs the lupa package for an embedded Lua 5.4. The stub records what the
scripts ask REAPER to do. It does not prove REAPER honours those calls.
"""

import struct
import tempfile
import unittest
from pathlib import Path

import diff_log

try:
    from lupa import lua54
except ImportError:
    lua54 = None


ROOT = Path(__file__).resolve().parent
LUA = ROOT.parent / "lua"


class StubReaper:
    def __init__(self, missing_fx=(), gmem=None, project=""):
        self.tracks = []
        self.missing_fx = set(missing_fx)
        self.gmem = gmem or {}
        self.project = project
        self.messages = []
        self.send_info = {}
        self.params = {}

    def table(self, lua):
        names = [
            "CountTracks", "GetTrack", "InsertTrackAtIndex", "DeleteTrack",
            "GetSetMediaTrackInfo_String", "SetMediaTrackInfo_Value",
            "TrackFX_AddByName", "TrackFX_SetParam", "CreateTrackSend",
            "SetTrackSendInfo_Value", "ShowMessageBox", "ShowConsoleMsg",
            "gmem_attach", "gmem_read", "EnumProjects", "GetResourcePath",
        ]
        api = lua.table_from({name: getattr(self, name) for name in names})
        for name in ("Undo_BeginBlock", "Undo_EndBlock", "PreventUIRefresh", "TrackList_AdjustWindows"):
            api[name] = lambda *args: None
        return api

    def CountTracks(self, project):
        return len(self.tracks)

    def GetTrack(self, project, index):
        return self.tracks[int(index)]

    def InsertTrackAtIndex(self, index, defaults):
        self.tracks.insert(int(index), {"name": "", "fx": [], "sends": 0, "info": {}})

    def DeleteTrack(self, track):
        self.tracks.remove(track)

    def GetSetMediaTrackInfo_String(self, track, key, value, set_new):
        if set_new:
            track["name"] = value
        return True, track["name"]

    def SetMediaTrackInfo_Value(self, track, key, value):
        track["info"][key] = value

    def TrackFX_AddByName(self, track, name, record, instantiate):
        if name in self.missing_fx:
            return -1
        track["fx"].append(name)
        return len(track["fx"]) - 1

    def TrackFX_SetParam(self, track, fx, param, value):
        self.params[(track["name"], int(fx), int(param))] = value

    def CreateTrackSend(self, source, dest):
        source["sends"] += 1
        return source["sends"] - 1

    def SetTrackSendInfo_Value(self, track, category, send, key, value):
        self.send_info[(int(send), key)] = value

    def ShowMessageBox(self, text, title, kind):
        self.messages.append(text)

    def ShowConsoleMsg(self, text):
        self.messages.append(text)

    def gmem_attach(self, name):
        self.attached = name

    def gmem_read(self, index):
        return self.gmem.get(int(index), 0)

    def EnumProjects(self, index, buffer):
        return None, self.project

    def GetResourcePath(self):
        return str(Path(self.project).parent)


def run(stub, script):
    lua = lua54.LuaRuntime(unpack_returned_tuples=True)
    lua.globals().reaper = stub.table(lua)
    lua.execute((LUA / script).read_text())


class StubUndoReaper(StubReaper):
    """An engine that applies mailbox commands once per tick, and a snapshot undo.

    fx_tracking "always" rereads the engine state for every snapshot. "params" keeps the
    state REAPER last read and rereads it only when the State revision parameter changes,
    which is the behavior the REAPER 7.82 run pointed to. An FX-only undo point is then
    created only when that state differs from the last snapshot.
    """

    def __init__(self, project, steps, engine_runs=True, restores_fx=True, fx_tracking="always"):
        super().__init__(project=project)
        for name in ("Furrow Probe Folder", "Furrow Probe Engine", "Furrow Probe Lane 1"):
            self.InsertTrackAtIndex(len(self.tracks), True)
            self.tracks[-1]["name"] = name
        self.steps = list(steps)
        self.cached_steps = list(steps)
        self.revision = 0
        self.fx_tracking = fx_tracking
        self.engine_runs = engine_runs
        self.restores_fx = restores_fx
        self.gmem = {0: 1}
        self.last_seq = 0
        self.pending_seq = 0
        self.apply_at = 0
        self.clock = 0
        self.deferred = None
        self.history = [("", self.snapshot())]
        self.cursor = 0

    def table(self, lua):
        api = super().table(lua)
        for name in ("TrackFX_GetParam", "TrackFX_SetParam", "GetTrackGUID", "gmem_write", "defer", "time_precise", "Undo_OnStateChangeEx2", "Undo_BeginBlock2",
                     "Undo_EndBlock2", "Undo_DoUndo2", "Undo_DoRedo2", "Undo_CanUndo2", "OnPlayButton",
                     "OnStopButton"):
            api[name] = getattr(self, name)
        return api

    def fx_state(self):
        if self.fx_tracking == "always":
            self.cached_steps = list(self.steps)
        return (tuple(self.cached_steps), self.revision)

    def snapshot(self):
        return {"fx": self.fx_state(), "tracks": [dict(track) for track in self.tracks]}

    def restore(self, state):
        if self.restores_fx and state["fx"] != self.fx_state():
            self.steps = list(state["fx"][0])
            self.cached_steps = list(self.steps)
            self.revision = state["fx"][1]
            self.gmem[7] = self.gmem.get(7, 0) + 1
            self.gmem[8] = self.gmem.get(8, 0) + 1
        self.tracks[:] = [dict(track) for track in state["tracks"]]

    def bump_revision(self, value):
        self.revision = value
        self.cached_steps = list(self.steps)

    def TrackFX_GetParam(self, track, fx, param):
        return float(self.revision), 0.0, 16777215.0

    def TrackFX_SetParam(self, track, fx, param, value):
        if track["name"] == "Furrow Probe Engine" and int(param) == 1:
            self.bump_revision(int(value))

    def engine_block(self):
        if not self.engine_runs:
            return
        self.gmem[1] = self.gmem.get(1, 0) + 1
        seq = self.gmem.get(2, 0)
        if seq != self.last_seq:
            if seq != self.pending_seq:
                self.pending_seq = seq
                self.apply_at = self.gmem[1] + self.gmem.get(5, 0)
            if self.gmem[1] >= self.apply_at:
                index = int(self.gmem.get(4, 0))
                self.steps[index] = 1 - self.steps[index]
                if self.gmem.get(9, 0) == 1:
                    self.bump_revision(self.revision + 1)
                self.last_seq = seq
                self.gmem[6] = seq
                self.gmem[7] = self.gmem.get(7, 0) + 1
        for index, value in enumerate(self.steps):
            self.gmem[100 + index] = value

    def drive(self, limit=200000):
        while self.deferred is not None and limit > 0:
            callback, self.deferred = self.deferred, None
            self.engine_block()
            self.clock += 1
            callback()
            limit -= 1

    def InsertTrackAtIndex(self, index, defaults):
        super().InsertTrackAtIndex(index, defaults)
        self.guids = getattr(self, "guids", 0) + 1
        self.tracks[int(index)]["guid"] = "{%08d}" % self.guids

    def GetTrackGUID(self, track):
        return track["guid"]

    def gmem_write(self, index, value):
        self.gmem[int(index)] = value

    def defer(self, callback):
        self.deferred = callback

    def time_precise(self):
        return self.clock * 0.001

    def push(self, description):
        del self.history[self.cursor + 1:]
        self.history.append((description, self.snapshot()))
        self.cursor += 1

    def Undo_OnStateChangeEx2(self, project, description, states, track):
        if int(states) == 2 and self.snapshot()["fx"] == self.history[self.cursor][1]["fx"]:
            return
        self.push(description)

    def Undo_BeginBlock2(self, project):
        pass

    def Undo_EndBlock2(self, project, description, flags):
        self.push(description)

    def Undo_DoUndo2(self, project):
        if self.cursor == 0:
            return 0
        self.cursor -= 1
        self.restore(self.history[self.cursor][1])
        return 1

    def Undo_DoRedo2(self, project):
        if self.cursor + 1 == len(self.history):
            return 0
        self.cursor += 1
        self.restore(self.history[self.cursor][1])
        return 1

    def Undo_CanUndo2(self, project):
        return self.history[self.cursor][0] if self.cursor > 0 else None

    def OnPlayButton(self):
        pass

    def OnStopButton(self):
        pass


@unittest.skipIf(lua54 is None, "lupa is not installed")
class LayoutTest(unittest.TestCase):
    def test_each_send_filters_its_own_source_channel(self):
        stub = StubReaper()
        run(stub, "furrow_probe_layout.lua")
        flags = [stub.send_info[(send, "I_MIDIFLAGS")] for send in range(4)]
        self.assertEqual(flags, [4210721, 4210754, 4210787, 4210820])

    def test_each_receiver_gets_its_own_slot(self):
        stub = StubReaper()
        run(stub, "furrow_probe_layout.lua")
        slots = [stub.params[(f"Furrow Probe Lane {lane}", 0, 0)] for lane in (1, 2, 3, 4)]
        self.assertEqual(slots, [0, 1, 2, 3])

    def test_second_run_refuses(self):
        stub = StubReaper()
        run(stub, "furrow_probe_layout.lua")
        run(stub, "furrow_probe_layout.lua")
        self.assertEqual(len(stub.tracks), 6)
        self.assertIn("already in this project", stub.messages[-1])

    def test_missing_jsfx_leaves_no_tracks(self):
        stub = StubReaper(missing_fx={"JS:furrow_probe_receiver.jsfx"})
        run(stub, "furrow_probe_layout.lua")
        self.assertEqual(stub.tracks, [])
        self.assertIn("could not load JS:furrow_probe_receiver.jsfx", stub.messages[-1])


@unittest.skipIf(lua54 is None, "lupa is not installed")
class DumpTest(unittest.TestCase):
    def test_dump_of_the_expected_events_diffs_clean(self):
        expected_path = ROOT / "expected" / "constant-120.txt"
        expected = diff_log.load(expected_path)
        gmem = {0: 3, 2: 2, 1000: 0, 1001: 120.0, 1002: 0.0, 1003: 1024, 1004: 120.0, 1005: 0.0464399093, 3: 2, 4: 0, 200000: 120.0, 200001: 120.0}
        counts = [0, 0, 0, 0]
        for row in reversed(expected):
            slot = int(row["channel"])
            at = 2000000 + slot * 500000 + counts[slot] * 9
            values = [row["sample"], row["order"], slot, row["src_bus"], row["dst_bus"],
                      row["channel"], row["note"], row["velocity"], 1 if row["kind"] == "on" else 0]
            for offset, value in enumerate(values):
                gmem[at + offset] = float(value)
            counts[slot] += 1
        for slot, count in enumerate(counts):
            gmem[10 + slot] = count
        with tempfile.TemporaryDirectory() as directory:
            stub = StubReaper(gmem=gmem, project=str(Path(directory) / "probe.rpp"))
            run(stub, "furrow_probe_dump.lua")
            events = Path(directory) / "furrow-capture-events.txt"
            self.assertEqual(diff_log.compare(expected, diff_log.load(events), 0), [])
            dumped = [line for line in events.read_text().splitlines() if not line.startswith("#")]
            stored = [line for line in expected_path.read_text().splitlines() if line and not line.startswith("#")]
            self.assertEqual(dumped, stored)
            self.assertEqual(
                (Path(directory) / "furrow-capture-tempo-sample.txt").read_text().splitlines()[1:],
                ["0 120.0000000000", "1 120.0000000000"],
            )
            self.assertEqual(
                (Path(directory) / "furrow-capture-tempo-block.txt").read_text().splitlines()[1:],
                ["0 120.0000000000 0.000000000000", "1024 120.0000000000 0.046439909300"],
            )

    def test_wrong_layout_version_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            stub = StubReaper(gmem={0: 2}, project=str(Path(directory) / "probe.rpp"))
            run(stub, "furrow_probe_dump.lua")
            self.assertEqual(list(Path(directory).iterdir()), [])
            self.assertIn("layout version 2", stub.messages[-1])


@unittest.skipIf(lua54 is None, "lupa is not installed")
class UndoTest(unittest.TestCase):
    def run_undo(self, **options):
        steps = [int(value) for value in "1000100010001000" "0010001000100010" + "0" * 32]
        with tempfile.TemporaryDirectory() as directory:
            stub = StubUndoReaper(str(Path(directory) / "undo.rpp"), steps, **options)
            run(stub, "furrow_probe_undo.lua")
            stub.drive()
            results = (Path(directory) / "furrow-undo-results.txt").read_text().splitlines()
        return stub, results

    def test_an_undo_that_restores_engine_state_passes_every_check(self):
        stub, results = self.run_undo()
        self.assertEqual([line for line in results if line.startswith("FAIL")], [])
        self.assertEqual(results[-1], "0 failures")
        self.assertEqual([track["name"] for track in stub.tracks],
                         ["Furrow Probe Folder", "Furrow Probe Engine", "Furrow Probe Lane 1"])

    def test_cached_engine_state_fails_only_the_unmarked_gesture(self):
        stub, results = self.run_undo(fx_tracking="params")
        failed = [line.split("  ")[0] for line in results if line.startswith("FAIL")]
        self.assertEqual(failed, ["FAIL stopped A-none undo point", "FAIL playing A-none undo point"])

    def test_an_undo_that_skips_engine_state_fails_the_step_checks(self):
        stub, results = self.run_undo(restores_fx=False)
        failed = [line.split("  ")[0] for line in results if line.startswith("FAIL")]
        self.assertIn("FAIL stopped A-lua undo restores step", failed)
        self.assertIn("FAIL stopped C-engine one undo restores step and track", failed)
        self.assertNotIn("FAIL stopped B-lua first undo keeps the rename", failed)

    def test_an_engine_that_never_runs_stops_at_the_first_wait(self):
        stub, results = self.run_undo(engine_runs=False)
        self.assertIn("FAIL engine processes while stopped", results)
        self.assertIn("FAIL engine processes while playing", results)


class StubMemoryReaper:
    """Project tabs whose memory JSFX instances run once per tick.

    shared_gmem gives every tab one named gmem. Otherwise each tab has its own, and Lua
    sees the first tab's. background_runs keeps a tab that is not active processing.
    """

    SIZE = 8388608

    def __init__(self, directory, shared_gmem=True, background_runs=True):
        self.directory = directory
        self.shared_gmem = shared_gmem
        self.background_runs = background_runs
        self.projects = [{"name": "first", "tracks": [], "gmem": {}}]
        self.active = self.projects[0]
        self.instances = []
        self.clock = 0
        self.deferred = None
        self.messages = []
        self.tokens = 0

    def table(self, lua):
        names = ["EnumProjects", "SelectProjectInstance", "Main_OnCommand", "CountTracks", "GetTrack",
                 "InsertTrackAtIndex", "GetSetMediaTrackInfo_String", "TrackFX_AddByName", "TrackFX_SetParam",
                 "TrackFX_GetParam", "TrackFX_CopyToTrack", "gmem_attach", "gmem_read", "gmem_write",
                 "time_precise", "defer", "ShowConsoleMsg", "GetResourcePath"]
        return lua.table_from({name: getattr(self, name) for name in names})

    def gmem_of(self, project):
        return self.projects[0]["gmem"] if self.shared_gmem else project["gmem"]

    def new_instance(self, project, params=None):
        self.tokens += 1
        instance = {"project": project, "token": 1000 + self.tokens, "params": list(params or [0] * 8)}
        instance["params"][6] = -1
        self.instances.append(instance)
        return instance

    def tick_instances(self):
        for instance in self.instances:
            project = instance["project"]
            if project is not self.active and not self.background_runs:
                continue
            gmem, params = self.gmem_of(project), instance["params"]
            params[2] += 1
            if params[4] == 0:
                params[4] = instance["token"] * 7 % 16777215 + 1
            at = 100 + 4 * int(params[0])
            gmem[at], gmem[at + 1], gmem[at + 2], gmem[at + 3] = instance["token"], params[2], params[4], gmem.get(1, 0)
            params[1], params[3], params[5], params[7] = gmem.get(1, 0), instance["token"], self.SIZE, 1
            markers = (1048575, 2097151, 4194303, 8388607, 8388608, 16777215, 33554431)
            seen = [index for index in markers if gmem.get(index) == index + 0.25]
            params[6] = max(seen) if seen else -1
            if gmem.get(2) == 2:
                for index in markers:
                    if index < self.SIZE:
                        gmem[index] = index + 0.5

    def drive(self, limit=100000):
        while self.deferred is not None and limit > 0:
            callback, self.deferred = self.deferred, None
            self.tick_instances()
            self.clock += 1
            callback()
            limit -= 1

    def EnumProjects(self, index, buffer):
        if int(index) == -1:
            return self.active, str(Path(self.directory) / (self.active["name"] + ".rpp"))
        project = self.projects[int(index)]
        return project, str(Path(self.directory) / (project["name"] + ".rpp"))

    def SelectProjectInstance(self, project):
        self.active = project

    def Main_OnCommand(self, command, flag):
        if int(command) == 40859:
            self.projects.append({"name": "second", "tracks": [], "gmem": {}})
            self.active = self.projects[-1]

    def CountTracks(self, project):
        return len(project["tracks"])

    def GetTrack(self, project, index):
        return project["tracks"][int(index)]

    def InsertTrackAtIndex(self, index, defaults):
        self.active["tracks"].insert(int(index), {"name": "", "fx": [], "project": self.active})

    def GetSetMediaTrackInfo_String(self, track, key, value, set_new):
        if set_new:
            track["name"] = value
        return True, track["name"]

    def TrackFX_AddByName(self, track, name, record, instantiate):
        track["fx"].append(self.new_instance(track["project"]))
        return len(track["fx"]) - 1

    def TrackFX_CopyToTrack(self, source, source_fx, dest, dest_fx, move):
        dest["fx"].insert(int(dest_fx), self.new_instance(dest["project"], source["fx"][int(source_fx)]["params"]))

    def TrackFX_SetParam(self, track, fx, param, value):
        track["fx"][int(fx)]["params"][int(param)] = value

    def TrackFX_GetParam(self, track, fx, param):
        return float(track["fx"][int(fx)]["params"][int(param)]), 0.0, 1.0

    def gmem_attach(self, name):
        pass

    def gmem_read(self, index):
        return self.projects[0]["gmem"].get(int(index), 0) if int(index) < self.SIZE else 0

    def gmem_write(self, index, value):
        if int(index) < self.SIZE:
            self.projects[0]["gmem"][int(index)] = value

    def time_precise(self):
        return self.clock * 0.001

    def defer(self, callback):
        self.deferred = callback

    def ShowConsoleMsg(self, text):
        self.messages.append(text)

    def GetResourcePath(self):
        return self.directory


@unittest.skipIf(lua54 is None, "lupa is not installed")
class MemoryTest(unittest.TestCase):
    def run_memory(self, **options):
        with tempfile.TemporaryDirectory() as directory:
            stub = StubMemoryReaper(directory, **options)
            run(stub, "furrow_probe_memory.lua")
            stub.drive()
            return (Path(directory) / "furrow-memory-results.txt").read_text().splitlines()

    def test_shared_gmem_with_running_background_tabs(self):
        results = self.run_memory()
        self.assertEqual(results[-1], "45 results, 0 failures")
        for line in (
            "RESULT Furrow Memory A2 sees the Lua nonce = true",
            "RESULT A3 copy runtime token differs from A1 = true",
            "RESULT A3 copy persistent id equals A1 = true",
            "RESULT Furrow Memory B1 sees the Lua nonce = true",
            "RESULT Lua reads back its own write at 8388607 = true",
            "RESULT Lua reads back its own write at 8388608 = false",
            "RESULT A1 highest Lua marker index it read back = 8388607",
            "RESULT Lua reads an instance write at 4194303 = true",
            "RESULT A1 local memory top = 8388608",
        ):
            self.assertIn(line, results)
        background = results.index("== first tab active, second tab in the background")
        self.assertIn("RESULT Furrow Memory B1 runs = true", results[background:])

    def test_separate_gmem_and_paused_background_tabs(self):
        results = self.run_memory(shared_gmem=False, background_runs=False)
        self.assertEqual(results[-1], "45 results, 0 failures")
        second = results.index("== second project tab")
        background = results.index("== first tab active, second tab in the background")
        self.assertIn("RESULT Furrow Memory B1 sees the Lua nonce = false", results[second:background])
        self.assertIn("RESULT Furrow Memory B1 runs = true", results[second:background])
        self.assertIn("RESULT Furrow Memory B1 runs = false", results[background:])
        self.assertIn("RESULT Furrow Memory A1 sees the Lua nonce = true", results[background:])


class StubStateReaper(StubReaper):
    """A state probe whose file_var keeps doubles exactly, or rounds them to 32-bit floats."""

    VALUES = [0.0, -0.0, -16.25, 1 / 3, 288000 + 1 / 3, -(2.0 ** 30) - 1 / 7, 2.0 ** 52 + 2, 2.0 ** -30]
    SEEDS = [0, 16777215, 16777217]

    def __init__(self, project, float32=False):
        super().__init__(project=project)
        self.float32 = float32
        self.gmem = {0: 1}
        self.clock = 0
        self.deferred = None
        self.tokens = 0
        self.stored = None

    def table(self, lua):
        api = super().table(lua)
        for name in ("gmem_write", "defer", "time_precise", "GetTrackStateChunk", "SetTrackStateChunk",
                     "Main_SaveProject"):
            api[name] = getattr(self, name)
        return api

    def file_var(self, value):
        return struct.unpack("<f", struct.pack("<f", value))[0] if self.float32 else value

    def new_instance(self):
        self.tokens += 1
        self.gmem[3] = 1000 + self.tokens

    def write_state(self):
        words = [struct.unpack("<4H", struct.pack("<d", 0.0 if v == 0 else v)) for v in self.VALUES]
        return {"words": [[self.file_var(w) for w in group] for group in words],
                "raw": self.file_var(1 / 3), "seeds": [self.file_var(seed) for seed in self.SEEDS]}

    def read_state(self, state):
        for i, (value, words) in enumerate(zip(self.VALUES, state["words"])):
            decoded = struct.unpack("<d", struct.pack("<4H", *[int(w) for w in words]))[0]
            self.gmem[100 + i] = decoded
            self.gmem[200 + i] = 1 if decoded == value else 0
            for w, word in enumerate(words):
                self.gmem[1000 + 4 * i + w] = word
        self.gmem[300], self.gmem[301] = state["raw"], 1 if state["raw"] == 1 / 3 else 0
        for k, seed in enumerate(state["seeds"]):
            self.gmem[400 + k], self.gmem[410 + k] = seed, 1 if seed == self.SEEDS[k] else 0
        self.gmem[4] = self.gmem[3]
        self.gmem[1] = self.gmem.get(1, 0) + 1

    def TrackFX_AddByName(self, track, name, record, instantiate):
        self.new_instance()
        return super().TrackFX_AddByName(track, name, record, instantiate)

    def GetTrackStateChunk(self, track, buffer, undo):
        return True, "<TRACK>"

    def SetTrackStateChunk(self, track, chunk, undo):
        state = self.write_state()
        self.new_instance()
        self.read_state(state)
        return True

    def Main_SaveProject(self, project, ask):
        self.stored = self.write_state()

    def reload(self):
        self.new_instance()
        self.read_state(self.stored)

    def drive(self, limit=100000):
        while self.deferred is not None and limit > 0:
            callback, self.deferred = self.deferred, None
            self.gmem[2] = self.gmem.get(2, 0) + 1
            self.clock += 1
            callback()
            limit -= 1

    def gmem_write(self, index, value):
        self.gmem[int(index)] = value

    def defer(self, callback):
        self.deferred = callback

    def time_precise(self):
        return self.clock * 0.001


@unittest.skipIf(lua54 is None, "lupa is not installed")
class StateTest(unittest.TestCase):
    def run_state(self, reopen=True, **options):
        with tempfile.TemporaryDirectory() as directory:
            stub = StubStateReaper(str(Path(directory) / "state.rpp"), **options)
            run(stub, "furrow_probe_state.lua")
            stub.drive()
            if reopen:
                stub.reload()
            run(stub, "furrow_probe_state.lua")
            stub.drive()
            return (Path(directory) / "furrow-state-results.txt").read_text().splitlines()

    def test_exact_file_var_passes_both_runs(self):
        results = self.run_state()
        self.assertEqual([line for line in results if line.startswith("FAIL")], [])
        self.assertEqual([line for line in results if line.endswith("failures")], ["0 failures", "0 failures"])
        self.assertIn("RESULT reload seed 16777217, above the spec's bound = 16777217", results)

    def test_float32_file_var_keeps_words_and_bounded_seeds(self):
        results = self.run_state(float32=True)
        self.assertEqual([line for line in results if line.startswith("FAIL")], [])
        self.assertIn("RESULT reload raw 1/3 through file_var = 0.3333333432674408, exact false", results)
        self.assertIn("RESULT reload seed 16777217, above the spec's bound = 16777216", results)

    def test_a_second_run_without_reopening_fails(self):
        results = self.run_state(reopen=False)
        self.assertTrue(any(line.startswith("FAIL the project was reopened") for line in results))


if __name__ == "__main__":
    unittest.main()
