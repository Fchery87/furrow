#!/usr/bin/env python3
"""Run the probe Lua scripts against a stub REAPER API.

Needs the lupa package for an embedded Lua 5.4. The stub records what the
scripts ask REAPER to do. It does not prove REAPER honours those calls.
"""

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
    """An engine that applies mailbox commands once per tick, and an undo that snapshots everything."""

    def __init__(self, project, steps, engine_runs=True, restores_fx=True):
        super().__init__(project=project)
        for name in ("Furrow Probe Folder", "Furrow Probe Engine", "Furrow Probe Lane 1"):
            self.InsertTrackAtIndex(len(self.tracks), True)
            self.tracks[-1]["name"] = name
        self.steps = list(steps)
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
        for name in ("GetTrackGUID", "gmem_write", "defer", "time_precise", "Undo_OnStateChangeEx2", "Undo_BeginBlock2",
                     "Undo_EndBlock2", "Undo_DoUndo2", "Undo_DoRedo2", "Undo_CanUndo2", "OnPlayButton",
                     "OnStopButton"):
            api[name] = getattr(self, name)
        return api

    def snapshot(self):
        return {"steps": list(self.steps), "tracks": [dict(track) for track in self.tracks]}

    def restore(self, state):
        if self.restores_fx:
            self.steps = list(state["steps"])
            self.gmem[7] = self.gmem.get(7, 0) + 1
            self.gmem[8] = self.gmem.get(8, 0) + 1
        self.tracks[:] = [dict(track) for track in state["tracks"]]

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
        failed = [line for line in results if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertEqual(results[-1], "0 failures")
        self.assertEqual(sum(line.startswith("PASS") for line in results), 47)
        self.assertEqual([track["name"] for track in stub.tracks],
                         ["Furrow Probe Folder", "Furrow Probe Engine", "Furrow Probe Lane 1"])

    def test_an_undo_that_skips_engine_state_fails_the_step_checks(self):
        stub, results = self.run_undo(restores_fx=False)
        failed = [line.split("  ")[0] for line in results if line.startswith("FAIL")]
        self.assertIn("FAIL stopped A undo restores step", failed)
        self.assertIn("FAIL stopped C2 one undo restores step and track", failed)
        self.assertNotIn("FAIL stopped B first undo keeps the rename", failed)

    def test_an_engine_that_never_runs_stops_at_the_first_wait(self):
        stub, results = self.run_undo(engine_runs=False)
        self.assertIn("FAIL engine processes while stopped", results)
        self.assertIn("FAIL engine processes while playing", results)


if __name__ == "__main__":
    unittest.main()
