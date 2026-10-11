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
        gmem = {0: 2, 2: 1, 1000: 0, 1001: 120.0, 3: 2, 4: 0, 200000: 120.0, 200001: 120.0}
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

    def test_wrong_layout_version_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            stub = StubReaper(gmem={0: 1}, project=str(Path(directory) / "probe.rpp"))
            run(stub, "furrow_probe_dump.lua")
            self.assertEqual(list(Path(directory).iterdir()), [])
            self.assertIn("layout version 1", stub.messages[-1])


if __name__ == "__main__":
    unittest.main()
