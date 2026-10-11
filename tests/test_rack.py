"""The coordinator, host and window code against a stub REAPER, with SeqCore running in ysfx.

The stub records what the Lua asks REAPER to do. It does not prove REAPER honours it."""

import itertools
import unittest

from harness import LUA, Rig, missing, pair

SKIP = missing()

OP_SET_STEP = 2
F_ON = 0
RS5K_PARAMS = ["Volume", "Pan", "Gain for minimum velocity", "Note range start", "Note range end",
               "Pitch for start note", "Pitch for end note", "MIDI channel", "Max voices", "Attack", "Release",
               "Obey note-offs"]


class Track:
    ids = itertools.count(1)

    def __init__(self):
        self.guid = "{%08d-0000-0000-0000-000000000000}" % next(Track.ids)
        self.name = ""
        self.info = {"I_FOLDERDEPTH": 0.0, "B_MAINSEND": 1.0}
        self.fx = []
        self.sends = []
        self.ext = {}


class StubReaper:
    def __init__(self, rig):
        self.rig = rig
        self.tracks = []
        self.undo = []
        self.log = []
        self.deferred = []
        self.project_open = True
        self.proj_ext = {}
        self.fail_rs5k = False
        self.clock = 1000.0
        self.selected = None

    def table(self, lua):
        names = [n for n in dir(self) if n[0].isupper() or n.startswith("gmem") or n in ("defer", "time_precise")]
        return lua.table_from({n: getattr(self, n) for n in names})

    def gmem_attach(self, name):
        assert name == "Furrow"

    def gmem_read(self, index):
        return self.rig.mirror.read(index)

    def gmem_write(self, index, value):
        self.rig.mirror.write(index, value)

    def time_precise(self):
        return self.clock

    def defer(self, fn):
        self.deferred.append(fn)

    def ValidatePtr2(self, proj, pointer, kind):
        return self.project_open

    def SetProjExtState(self, proj, section, key, value):
        self.proj_ext[(section, key)] = value
        return 0

    def GetProjExtState(self, proj, section, key):
        value = self.proj_ext.get((section, key))
        return (1, value) if value else (0, "")

    def EnumProjects(self, index, *rest):
        return "project" if index in (-1, 0) else None

    def CountTracks(self, proj):
        return len(self.tracks)

    def GetTrack(self, proj, index):
        index = int(index)
        return self.tracks[index] if 0 <= index < len(self.tracks) else None

    def GetSelectedTrack(self, proj, index):
        return self.selected

    def GetTrackGUID(self, track):
        return track.guid

    def InsertTrackAtIndex(self, index, defaults):
        self.tracks.insert(int(index), Track())
        self.log.append(("insert track", self.rig.engine.var("lane_count")))

    def DeleteTrack(self, track):
        self.tracks.remove(track)
        self.log.append(("delete track", track.name))

    def GetSetMediaTrackInfo_String(self, track, key, value, set_new):
        if key == "P_NAME":
            if set_new:
                track.name = value
            return True, track.name
        if set_new:
            track.ext[key] = value
        return True, track.ext.get(key, "")

    def GetMediaTrackInfo_Value(self, track, key):
        if key == "IP_TRACKNUMBER":
            return float(self.tracks.index(track) + 1)
        return track.info.get(key, 0.0)

    def SetMediaTrackInfo_Value(self, track, key, value):
        track.info[key] = float(value)

    def GetParentTrack(self, track):
        depth = 0
        for other in reversed(self.tracks[:self.tracks.index(track)]):
            depth += other.info["I_FOLDERDEPTH"]
            if depth >= 1:
                return other
        return None

    def TrackFX_AddByName(self, track, name, rec, instantiate):
        if name == "JS:furrow_seqcore.jsfx":
            track.fx.append({"name": "JS: Furrow SeqCore", "engine": self.rig.engine})
        elif name == "ReaSamplOmatic5000":
            if self.fail_rs5k:
                raise RuntimeError("RS5K failed to load")
            track.fx.append({"name": "VSTi: ReaSamplOmatic5000 (Cockos)", "params": [0.5] * len(RS5K_PARAMS),
                             "config": {}})
        else:
            return -1
        return len(track.fx) - 1

    def TrackFX_GetCount(self, track):
        return len(track.fx)

    def TrackFX_GetFXName(self, track, fx, buf):
        return True, track.fx[int(fx)]["name"]

    def TrackFX_GetParam(self, track, fx, param):
        return track.fx[int(fx)]["engine"].slider(int(param)), 0, 0

    def TrackFX_SetParam(self, track, fx, param, value):
        track.fx[int(fx)]["engine"].set_slider(int(param), value)
        self.log.append(("revision", value))
        return True

    def TrackFX_GetNumParams(self, track, fx):
        return len(RS5K_PARAMS)

    def TrackFX_GetParamName(self, track, fx, param, buf):
        return True, RS5K_PARAMS[int(param)]

    def TrackFX_FormatParamValueNormalized(self, track, fx, param, value, buf):
        # Stand-in curve with +0.0 at 0.5 and -inf at 0, as RS5K showed. It is not RS5K's curve.
        if value <= 0:
            return True, "-inf"
        return True, "%.1f" % (-60.0 * (1.0 - value / 0.5))

    def TrackFX_SetParamNormalized(self, track, fx, param, value):
        track.fx[int(fx)]["params"][int(param)] = value
        return True

    def TrackFX_SetNamedConfigParm(self, track, fx, key, value):
        track.fx[int(fx)]["config"][key] = value
        return True

    def TrackFX_GetNamedConfigParm(self, track, fx, key):
        return True, track.fx[int(fx)]["config"].get(key, "")

    def CreateTrackSend(self, src, dst):
        src.sends.append({"dst": dst})
        return len(src.sends) - 1

    def SetTrackSendInfo_Value(self, track, category, index, key, value):
        track.sends[int(index)][key] = value
        return True

    def Undo_OnStateChangeEx2(self, proj, name, states, trackparm):
        self.undo.append(name)
        self.log.append(("undo point", name))

    def Undo_BeginBlock2(self, proj):
        pass

    def Undo_EndBlock2(self, proj, name, flags):
        self.undo.append(name)

    def GetAppVersion(self):
        return "stub"

    def RecursiveCreateDirectory(self, path, flags):
        import os
        os.makedirs(path, exist_ok=True)
        return 1

    def ShowConsoleMsg(self, text):
        self.log.append(("console", text))

    def Undo_CanUndo2(self, proj):
        return self.undo[-1] if self.undo else None

    def Master_GetTempo(self):
        return 120.0

    def Main_OnCommand(self, command, flag):
        self.log.append(("command", command))

    def ShowMessageBox(self, text, title, kind):
        self.log.append(("message", text))
        return 1


@unittest.skipIf(SKIP, SKIP)
class RackTest(unittest.TestCase):
    def setUp(self):
        self.rig = Rig()
        self.stub = StubReaper(self.rig)
        lua = self.rig.lua
        lua.globals().reaper = self.stub.table(lua)
        self.host = lua.eval('(require("furrow.host"))')
        coordinator = lua.eval('(require("furrow.coordinator"))')
        self.coordinator_module = coordinator
        rack = pair(self.host.create_rack())[0]
        self.assertIsNotNone(rack)
        self.rack = rack
        self.coordinator = coordinator.new("project", rack)
        self.call = lua.eval("function(o, m, ...) return o[m](o, ...) end")
        self.tick(8)

    def tearDown(self):
        self.rig.close()

    def c(self, method, *args):
        return self.call(self.coordinator, method, *args)

    def tick(self, count=1):
        for _ in range(count):
            self.c("tick")
            self.rig.block(playing=False)
            self.stub.clock += 0.03

    def settle(self, limit=40):
        for _ in range(limit):
            self.tick()
            if not self.c("busy"):
                return
        self.fail("gestures did not settle")

    def test_new_rack_layout(self):
        names = [(t.name, t.info["I_FOLDERDEPTH"], t.info["B_MAINSEND"]) for t in self.stub.tracks]
        self.assertEqual(names, [("Furrow Rack", 1.0, 1.0), ("Furrow Engine", -1.0, 0.0)])
        self.assertEqual(self.coordinator.status, "ready")

    def test_adding_a_sample_follows_the_acknowledgement_in_one_undo_point(self):
        self.c("add_sample", "/samples/Kick 01.wav")
        self.settle()
        self.assertEqual(self.stub.undo, ["Furrow: add lane"])
        events = [e[0] for e in self.stub.log]
        self.assertEqual(events[-3:], ["insert track", "revision", "undo point"])
        # The engine had the lane before the track existed, so the host change followed the ack.
        self.assertEqual([e for e in self.stub.log if e[0] == "insert track"][-1], ("insert track", 1.0))
        rack, engine, lane = self.stub.tracks
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine", "Kick 01"])
        self.assertEqual((engine.info["I_FOLDERDEPTH"], lane.info["I_FOLDERDEPTH"]), (0.0, -1.0))
        rs5k = lane.fx[0]
        self.assertEqual(rs5k["config"]["FILE0"], "/samples/Kick 01.wav")
        self.assertEqual(rs5k["params"][11], 1)
        self.assertAlmostEqual(rs5k["params"][2], 0.25, delta=0.001)
        self.assertEqual({k: rs5k["config"][k] for k in ("param.0.plink.midi_msg2", "param.1.plink.midi_msg2",
                                                          "param.0.plink.effect")},
                         {"param.0.plink.midi_msg2": "7", "param.1.plink.midi_msg2": "10",
                          "param.0.plink.effect": "-100"})
        self.assertEqual(engine.sends[0]["I_MIDIFLAGS"], 1 | (1 << 5) | (1 << 14) | (1 << 22))
        self.assertEqual(engine.sends[0]["I_SRCCHAN"], -1)
        self.tick(6)
        lanes = self.c("lanes")
        self.assertEqual(len(lanes), 1)
        self.assertEqual(rack.ext["P_EXT:Furrow"], "1\nlane %d %s" % (lanes[1].id, lane.guid))
        self.assertEqual(self.rig.engine.var("rack_id") > 0, True)

    def test_a_drag_paints_many_steps_in_one_undo_point(self):
        self.c("add_sample", "/samples/hat.wav")
        self.settle()
        gesture = self.c("begin", "paint steps")
        for step in (0, 2, 4, 6):
            commands = self.rig.lua.table_from([self.rig.lua.table_from([OP_SET_STEP, 0, 0, step, F_ON, 1])])
            overlay = self.rig.lua.table_from([self.rig.lua.table_from([0, step, F_ON, 1])])
            self.c("add", gesture, commands, overlay)
            self.assertEqual(self.c("step_value", 0, step, F_ON), 1)
        self.c("finish", gesture)
        self.settle()
        self.assertEqual(self.stub.undo[-1], "Furrow: paint steps")
        self.assertEqual(self.stub.undo.count("Furrow: paint steps"), 1)
        self.tick(6)
        self.assertEqual([self.c("step_value", 0, s, F_ON) for s in range(8)], [1, 0, 1, 0, 1, 0, 1, 0])
        self.assertEqual(len(list(self.coordinator.overlay.values())), 0)

    def test_undo_replaces_the_display(self):
        self.c("add_sample", "/samples/snare.wav")
        self.settle()
        before = self.rig.engine.save()
        self.c("run", "paint steps", self.rig.lua.table_from(
            [self.rig.lua.table_from([OP_SET_STEP, 0, 0, 3, F_ON, 1])]))
        self.settle()
        self.tick(6)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 1)
        self.rig.engine.load(before)
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 0)
        self.assertEqual(self.coordinator.status, "ready")

    def test_a_stalled_engine_pauses_edits(self):
        for _ in range(20):
            self.c("tick")
            self.stub.clock += 0.05
        self.assertEqual(self.coordinator.status, "stalled")
        self.tick(2)
        self.assertEqual(self.coordinator.status, "ready")

    def test_deleting_a_lane_removes_its_track(self):
        self.c("add_sample", "/samples/clap.wav")
        self.settle()
        self.tick(6)
        lane = self.c("lanes")[1]
        self.c("remove_lane", lane)
        self.settle()
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine"])
        self.assertEqual(self.stub.tracks[1].info["I_FOLDERDEPTH"], -1.0)
        self.assertEqual(self.stub.undo[-1], "Furrow: delete lane")
        self.assertEqual(self.rig.engine.var("lane_count"), 0)

    def journal(self):
        return self.stub.proj_ext.get(("Furrow", "journal " + self.coordinator.engine_guid), "")

    def replace_coordinator(self):
        """A new coordinator for the same rack, as after a closed window or a stopped script."""
        self.stub.clock += 2.0
        self.coordinator = self.coordinator_module.new("project", self.rack)
        self.tick(12)

    def test_a_lost_coordinator_after_the_engine_applied_an_add_leaves_no_orphan_lane(self):
        self.c("add_sample", "/samples/kick.wav")
        self.assertEqual(self.journal().startswith("lane "), True)
        self.rig.block(playing=False)
        self.assertEqual(self.rig.engine.var("lane_count"), 1)
        self.replace_coordinator()
        self.assertEqual(self.rig.engine.var("lane_count"), 0)
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine"])
        self.assertEqual((self.journal(), self.stub.undo), ("", []))
        self.assertEqual(self.coordinator.status, "ready")

    def test_a_lost_coordinator_before_the_engine_applied_an_add_changes_nothing(self):
        self.c("add_sample", "/samples/kick.wav")
        self.replace_coordinator()
        self.assertEqual(self.rig.engine.var("lane_count"), 0)
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine"])
        self.assertEqual((self.journal(), self.stub.undo), ("", []))

    def test_a_lost_coordinator_mid_delete_finishes_the_delete(self):
        self.c("add_sample", "/samples/clap.wav")
        self.settle()
        self.tick(6)
        self.c("remove_lane", self.c("lanes")[1])
        self.rig.block(playing=False)
        self.assertEqual(self.rig.engine.var("lane_count"), 0)
        self.assertEqual(len(self.stub.tracks), 3)
        self.replace_coordinator()
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine"])
        self.assertEqual(self.rack_links(), "1")
        self.assertEqual(self.journal(), "")

    def test_a_lost_coordinator_mid_drag_keeps_applied_edits_and_adds_one_undo_point(self):
        self.c("add_sample", "/samples/hat.wav")
        self.settle()
        self.stub.undo.clear()
        gesture = self.c("begin", "paint steps")
        for step in (1, 3, 5):
            commands = self.rig.lua.table_from([self.rig.lua.table_from([OP_SET_STEP, 0, 0, step, F_ON, 1])])
            self.c("add", gesture, commands)
        self.rig.run(6, playing=False)
        self.assertEqual(self.rig.engine.var("nd_count"), 3)
        self.replace_coordinator()
        self.assertEqual([self.c("step_value", 0, s, F_ON) for s in range(6)], [0, 1, 0, 1, 0, 1])
        self.assertEqual(self.stub.undo, ["Furrow: recover edits"])
        self.assertEqual(self.journal(), "")

    def test_a_thrown_error_while_building_a_lane_rolls_everything_back(self):
        self.stub.fail_rs5k = True
        self.c("add_sample", "/samples/boom.wav")
        self.settle()
        self.assertEqual([t.name for t in self.stub.tracks], ["Furrow Rack", "Furrow Engine"])
        self.assertEqual(self.stub.undo, [])
        self.assertIn("failed", self.coordinator.message)
        self.tick(6)
        self.assertEqual(self.rig.engine.var("lane_count"), 0)
        self.assertEqual(self.journal(), "")

    def two_lanes(self):
        for name in ("kick", "snare"):
            self.c("add_sample", "/samples/%s.wav" % name)
        self.settle()
        self.tick(8)
        self.stub.undo.clear()

    def random_edits(self, count):
        import random
        rng = random.Random(500)
        return [(rng.randrange(2), rng.randrange(16), rng.randrange(2)) for _ in range(count)]

    def edit(self, lane, step, value):
        lua = self.rig.lua
        commands = lua.table_from([lua.table_from([OP_SET_STEP, 0, lane, step, F_ON, value])])
        overlay = lua.table_from([lua.table_from([lane, step, F_ON, value])])
        return commands, overlay

    def assert_clean_timing(self):
        budget_us = self.rig.block_size / self.rig.srate * 1e6
        self.assertEqual(self.rig.engine.var("overruns"), 0)
        self.assertLess(self.rig.engine.var("block_max_us"), budget_us)

    def test_500_rapid_edits_in_one_drag_keep_the_final_state_and_every_result(self):
        self.two_lanes()
        edits = self.random_edits(500)
        before = self.coordinator.completed
        gesture = self.c("begin", "paint steps")
        for lane, step, value in edits:
            self.c("add", gesture, *self.edit(lane, step, value))
        self.c("finish", gesture)
        self.settle(200)
        self.tick(10)
        self.assertEqual(self.coordinator.completed - before, 500)
        self.assertIsNone(self.coordinator.message)
        final = {}
        for lane, step, value in edits:
            final[(lane, step)] = value
        shown = {(lane, step): self.c("step_value", lane, step, F_ON) for lane in range(2) for step in range(16)}
        self.assertEqual(shown, {k: final.get(k, 0) for k in shown})
        self.assertEqual(self.stub.undo, ["Furrow: paint steps"])
        self.assertEqual(len(list(self.coordinator.overlay.values())), 0)
        self.assert_clean_timing()

    def test_500_separate_gestures_each_get_a_result_and_an_undo_point(self):
        self.two_lanes()
        edits = self.random_edits(500)
        before = self.coordinator.completed
        for lane, step, value in edits:
            gesture = self.c("begin", "paint steps")
            self.c("add", gesture, *self.edit(lane, step, value))
            self.c("finish", gesture)
            self.tick()
        self.settle(200)
        self.tick(10)
        self.assertEqual(self.coordinator.completed - before, 500)
        self.assertEqual(self.stub.undo, ["Furrow: paint steps"] * 500)
        final = {}
        for lane, step, value in edits:
            final[(lane, step)] = value
        shown = {(lane, step): self.c("step_value", lane, step, F_ON) for lane in range(2) for step in range(16)}
        self.assertEqual(shown, {k: final.get(k, 0) for k in shown})
        self.assert_clean_timing()

    def test_the_stress_script_passes_against_the_engine(self):
        import os
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        self.tick(8)
        self.stub.clock += 2.0
        script = os.path.join(os.path.dirname(__file__), "..", "lua", "Furrow_Stress.lua")
        report = os.path.join(os.path.dirname(script), "..", "diag", "stress.txt")
        if os.path.exists(report):
            os.remove(report)
        self.rig.lua.eval('function(path) return dofile(path) end')(script)
        for _ in range(400):
            pending, self.stub.deferred = self.stub.deferred, []
            if not pending:
                break
            for fn in pending:
                fn()
            self.rig.block(playing=True)
            self.stub.clock += 0.03
        self.assertEqual(self.stub.deferred, [])
        with open(report) as handle:
            text = handle.read()
        self.assertIn("RESULT PASS", text)
        self.assertIn("results received 500", text)
        self.assertIn("mismatches 0", text)

    def rack_links(self):
        return self.rack["rack"].ext.get("P_EXT:Furrow", "")


class FakeImGui:
    """Answers ReaImGui calls with neutral values and can hand the window one dropped file."""

    def __init__(self):
        self.calls = []
        self.drop = []

    def table(self, lua):
        fake = self

        def call(name):
            def fn(*args):
                fake.calls.append(name)
                if name == "Begin":
                    return True, True
                if name == "BeginDragDropTarget":
                    return bool(fake.drop)
                if name == "AcceptDragDropPayloadFiles":
                    return (True, len(fake.drop)) if fake.drop else (False, 0)
                if name == "GetDragDropPayloadFile":
                    return True, fake.drop.pop(0)
                if name in ("GetItemRectMin", "GetContentRegionAvail"):
                    return 0.0, 0.0
                if name.startswith(("Is", "Button", "SmallButton", "MenuItem", "BeginPopup", "InvisibleButton")):
                    return False
                return None
            return fn

        meta = lua.eval("function(f) return setmetatable({}, {__index = function(t, k) "
                        "if k:find('_') and not k:find('^DrawList_') then return 1 end local v = f(k) rawset(t, k, v) return v end}) end")
        return meta(call)


@unittest.skipIf(SKIP, SKIP)
class WindowTest(unittest.TestCase):
    def test_the_window_draws_and_a_dropped_file_becomes_a_lane(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            host = lua.eval('(require("furrow.host"))')
            host.create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(30):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual([t.name for t in stub.tracks], ["Furrow Rack", "Furrow Engine", "perc"])
            self.assertIn("InvisibleButton", imgui.calls)
            self.assertEqual(stub.undo, ["Furrow: add lane"])
        finally:
            rig.close()

    def test_the_window_stops_when_its_project_closes(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(FakeImGui().table(lua))
            for frame in range(10):
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(len(stub.deferred), 1)
            stub.project_open = False
            pending, stub.deferred = stub.deferred, []
            for fn in pending:
                fn()
            self.assertEqual(stub.deferred, [])
        finally:
            rig.close()


if __name__ == "__main__":
    unittest.main()
