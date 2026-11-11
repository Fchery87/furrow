"""The REAPER bridge: its JSON codec, request handling, file protocol and audio analysis.

The REAPER API is a stub here. A pass shows the protocol and the code agree. The bridge in a real
REAPER is a separate run."""

import math
import os
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "tools" / "reaper_bridge"
sys.path.insert(0, str(BRIDGE))

from analysis import analyze  # noqa: E402
from client import BridgeError, Client, ensure_spool  # noqa: E402

try:
    from lupa import lua54
except ImportError:
    lua54 = None

SKIP = "lupa is not installed" if lua54 is None else None
RATE = 44100


def write_clicks(path, times, seconds):
    """A mono WAV with a short 880 Hz burst at each time."""
    samples = [0] * int(seconds * RATE)
    for t in times:
        start = int(t * RATE)
        for i in range(int(0.04 * RATE)):
            if start + i < len(samples):
                samples[start + i] = int(20000 * math.sin(2 * math.pi * 880 * i / RATE) * math.exp(-i / (0.01 * RATE)))
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(struct.pack("<%dh" % len(samples), *samples))


class Stub:
    """Just enough of the REAPER API for the bridge's ops."""

    def __init__(self):
        self.project = {}
        self.commands = []
        self.cursor = 0.0
        self.dirty = 0
        self.click_times = [0.1, 0.5, 1.0]

    def table(self, lua):
        names = [n for n in dir(self) if n[0].isupper() or n[:2] in ("ti",) or n in ("time_precise",)]
        return lua.table_from({n: getattr(self, n) for n in names})

    def GetAppVersion(self): return "stub 1.0"
    def time_precise(self): return 12.5
    def GetPlayState(self): return 0
    def GetPlayPosition(self): return 0.0
    def GetCursorPosition(self): return self.cursor
    def Master_GetTempo(self): return 120.0
    def Undo_CanUndo2(self, proj): return "Furrow: paint steps"
    def Undo_CanRedo2(self, proj): return None
    def IsProjectDirty(self, proj): return self.dirty
    def GetProjectPath(self, buf): return True, "/tmp/project"
    def CountTracks(self, proj): return 1
    def GetTrack(self, proj, i): return "track"
    def GetSetMediaTrackInfo_String(self, track, key, value, set_new):
        return True, {"P_NAME": "Kick", "P_EXT:Furrow": "1", "P_EXT:FurrowItems": ""}.get(key, "")
    def GetMediaTrackInfo_Value(self, track, key): return {"I_FOLDERDEPTH": 0.0, "B_MUTE": 0.0}.get(key, 0.0)
    def TrackFX_GetCount(self, track): return 1
    def TrackFX_GetFXName(self, track, fx, buf): return True, "JS: Furrow SeqCore"
    def TrackFX_GetEnabled(self, track, fx): return True
    def GetTrackNumSends(self, track, cat): return 1
    def GetTrackSendName(self, track, i, buf): return True, "Snare"
    def GetTrackSendInfo_Value(self, track, cat, i, key): return 4210721.0
    def CountTrackMediaItems(self, track): return 1
    def GetTrackMediaItem(self, track, i): return "item"
    def GetActiveTake(self, item): return "take"
    def TakeIsMIDI(self, take): return True
    def MIDI_CountEvts(self, take): return True, 3, 2, 0
    def GetSetMediaItemInfo_String(self, item, key, value, set_new): return True, "1 group 5"
    def GetMediaItemInfo_Value(self, item, key): return {"D_POSITION": 4.0, "D_LENGTH": 2.0, "B_MUTE": 0.0}[key]
    def SetEditCurPos(self, pos, move, seek): self.cursor = pos
    def OnPlayButton(self): self.commands.append("play")
    def OnStopButton(self): self.commands.append("stop")
    def OnPauseButton(self): self.commands.append("pause")
    def NamedCommandLookup(self, name): return 55555 if name == "_RS_TEST" else 0

    def GetSetProjectInfo(self, proj, key, value, set_new):
        if set_new:
            self.project[key] = value
        return self.project.get(key, 0.0)

    def GetSetProjectInfo_String(self, proj, key, value, set_new):
        if set_new:
            self.project[key] = value
        return True, self.project.get(key, "")

    def Main_OnCommand(self, command, flag):
        self.commands.append(command)
        if command == 42230:
            folder = self.project["RENDER_FILE"]
            write_clicks(Path(folder) / (self.project["RENDER_PATTERN"] + ".wav"), self.click_times,
                         self.project["RENDER_ENDPOS"])

    def EnumerateFiles(self, folder, i):
        self.rescans = getattr(self, "rescans", 0) + (1 if i < 0 else 0)
        if i < 0:
            return None
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            return None
        return names[i] if i < len(names) else None


@unittest.skipIf(SKIP, SKIP)
class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.spool = Path(self.temp.name) / "bridge"
        self.client = Client(self.spool)
        self.lua = lua54.LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute('package.path = "%s/?.lua;" .. package.path' % BRIDGE.as_posix())
        self.core = self.lua.eval('(require("core"))')
        self.stub = Stub()
        self.reaper = self.stub.table(self.lua)
        self.ctx = self.lua.table()

    def tearDown(self):
        self.temp.cleanup()

    def pump(self):
        self.core.poll_once(self.reaper, str(self.spool), self.ctx, 4)

    def call(self, op, **params):
        return self.client.call(op, timeout=5, pump=self.pump, **params)

    def test_json_round_trips_text_numbers_and_nesting(self):
        json = self.lua.eval('(require("json"))')
        text = json.encode(self.lua.table_from({"a": 1, "b": self.lua.table_from([1, 2.5, "x\ny\"z"]), "c": True}))
        decoded = json.decode(text)
        self.assertEqual((decoded["a"], decoded["b"][2], decoded["b"][3], decoded["c"]), (1, 2.5, 'x\ny"z', True))
        value, message = json.decode("{bad")
        self.assertIsNone(value)
        self.assertIn("expected", str(message))

    def test_ping_state_and_eval_answer_through_the_spool(self):
        self.assertEqual(self.call("ping")["reaper"], "stub 1.0")
        state = self.call("state")
        track = state["tracks"][0]
        self.assertEqual((track["name"], track["fx"][0]["name"], track["items"][0]["notes"]), ("Kick", "JS: Furrow SeqCore", 3))
        self.assertEqual(track["items"][0]["furrow"], "1 group 5")
        self.assertEqual(state["undo_next"], "Furrow: paint steps")
        self.assertEqual(state["project_path"], "/tmp/project")
        self.assertEqual(self.call("eval", code="return reaper.GetAppVersion(), 7"), ["stub 1.0", 7])

    def test_the_project_path_is_read_when_reaper_returns_only_the_path(self):
        self.stub.GetProjectPath = lambda buf: "/tmp/saved project"
        self.reaper = self.stub.table(self.lua)
        self.assertEqual(self.call("state")["project_path"], "/tmp/saved project")

    def test_a_failing_request_answers_with_an_error_and_the_bridge_goes_on(self):
        with self.assertRaisesRegex(BridgeError, "compile error"):
            self.call("eval", code="this is not lua")
        with self.assertRaisesRegex(BridgeError, "unknown op"):
            self.call("explode")
        with self.assertRaisesRegex(BridgeError, "boom"):
            self.call("eval", code="error('boom', 0)")
        self.assertEqual(self.call("ping")["reaper"], "stub 1.0")

    def test_a_huge_result_is_refused_instead_of_written(self):
        with self.assertRaisesRegex(BridgeError, "past the"):
            self.call("eval", code="return string.rep('x', 500000)")

    def test_transport_and_actions_reach_reaper(self):
        self.call("transport", action="play", position=3.5)
        self.call("action", command="_RS_TEST")
        self.call("action", command=40044)
        self.assertEqual((self.stub.cursor, self.stub.commands), (3.5, ["play", 55555, 40044]))
        with self.assertRaisesRegex(BridgeError, "unknown command"):
            self.call("action", command="_NOPE")

    def test_only_well_formed_request_names_are_read(self):
        (self.spool / "in" / "bad name.req").write_text('{"op":"ping"}')
        (self.spool / "in" / "ok1.req").write_text('{"op":"ping"}')
        self.pump()
        self.assertTrue((self.spool / "out" / "ok1.res").exists())
        self.assertTrue((self.spool / "in" / "bad name.req").exists())

    def test_render_restores_the_projects_settings_and_the_analysis_finds_each_click(self):
        self.stub.project["RENDER_SRATE"] = 48000.0
        self.stub.project["RENDER_FILE"] = "/keep/me"
        result = self.call("render", start=0, stop=2.0, name="take one")
        self.assertTrue(result["path"].endswith("take_one.wav"))
        self.assertEqual((self.stub.project["RENDER_SRATE"], self.stub.project["RENDER_FILE"]), (48000.0, "/keep/me"))
        found = analyze(result["path"])
        self.assertFalse(found["silent"])
        self.assertEqual([round(o["time"], 2) for o in found["onsets"]], [0.1, 0.5, 1.0])

    def test_a_repeat_render_returns_the_new_file_and_a_longer_name_is_not_mistaken_for_it(self):
        other = self.spool / "render" / "render_x.wav"
        write_clicks(other, [0.2], 1.0)
        first = self.call("render", stop=1.0, name="render")
        self.assertEqual(Path(first["path"]).name, "render.wav")
        self.stub.click_times = [0.3]
        second = self.call("render", stop=1.0, name="render")
        self.assertEqual([round(o["time"], 2) for o in analyze(second["path"])["onsets"]], [0.3])
        self.assertTrue(other.exists())

    def test_the_directory_listing_is_reread_before_each_poll(self):
        before = getattr(self.stub, "rescans", 0)
        self.call("ping")
        self.assertGreater(self.stub.rescans, before)

    def test_a_silent_render_is_reported_as_silent(self):
        path = self.spool / "render" / "quiet.wav"
        write_clicks(path, [], 1.0)
        self.assertTrue(analyze(path)["silent"])

    def test_shutdown_stops_the_loop_flag(self):
        self.call("shutdown")
        self.assertTrue(self.ctx["stopping"])

    def test_the_spool_must_be_private_to_the_user(self):
        loose = Path(self.temp.name) / "loose"
        loose.mkdir(mode=0o755)
        os.chmod(loose, 0o755)
        with self.assertRaisesRegex(BridgeError, "private"):
            ensure_spool(loose)
        self.assertEqual(oct(self.spool.stat().st_mode & 0o777), "0o700")

    def test_the_bridge_script_compiles(self):
        loaded, err = self.lua.eval("function(p) local f, e = loadfile(p); return f ~= nil, e end")(str(BRIDGE / "bridge.lua"))
        self.assertTrue(loaded, err)


if __name__ == "__main__":
    unittest.main()
