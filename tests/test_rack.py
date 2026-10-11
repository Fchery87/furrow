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


if __name__ == "__main__":
    unittest.main()
