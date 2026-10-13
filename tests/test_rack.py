"""The coordinator, host and window code against a stub REAPER, with SeqCore running in ysfx.

The stub records what the Lua asks REAPER to do. It does not prove REAPER honours it."""

import itertools
import unittest

from harness import LUA, Rig, missing, pair

SKIP = missing()

OP_SET_STEP = 2
OP_SET_LANE = 5
OP_SET_RACK = 6
R_SEED, R_SWING, R_TRUNCATE = 1, 4, 5
F_ON = 0
RS5K_PARAMS = ["Volume", "Pan", "Gain for minimum velocity", "Note range start", "Note range end",
               "Pitch for start note", "Pitch for end note", "MIDI channel", "Max voices", "Attack", "Release",
               "Obey note-offs"]


class Item:
    def __init__(self, track, start, stop):
        self.track = track
        self.start = start
        self.stop = stop
        self.ext = {}
        self.events = []
        self.take = self


class Track:
    ids = itertools.count(1)

    def __init__(self):
        self.guid = "{%08d-0000-0000-0000-000000000000}" % next(Track.ids)
        self.name = ""
        self.info = {"I_FOLDERDEPTH": 0.0, "B_MAINSEND": 1.0}
        self.fx = []
        self.sends = []
        self.ext = {}
        self.items = []


class StubReaper:
    def __init__(self, rig):
        self.rig = rig
        self.tracks = []
        self.undo = []
        self.log = []
        self.deferred = []
        self.project_open = True
        self.proj_ext = {}
        self.play_time = 0.0
        self.fail_rs5k = False
        self.fail_gate = False
        self.cursor = 8.0
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
        elif name == "JS:furrow_itemgate.jsfx":
            if self.fail_gate:
                return -1
            track.fx.append({"name": "JS: Furrow Item Gate", "gate": {}})
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
        if "gate" in track.fx[int(fx)]:
            track.fx[int(fx)]["gate"][int(param)] = value
            return True
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

    def GetPlayPosition2Ex(self, proj):
        return self.play_time

    def TimeMap2_timeToQN(self, proj, time):
        return time * 2.0

    def TimeMap2_QNToTime(self, proj, qn):
        return qn / 2.0

    def GetCursorPositionEx(self, proj):
        return self.cursor

    def CreateNewMIDIItemInProj(self, track, start, stop, qn_in):
        item = Item(track, start, stop)
        track.items.append(item)
        return item

    def GetActiveTake(self, item):
        return item

    def MIDI_GetPPQPosFromProjQN(self, take, qn):
        return (qn - take.start * 2.0) * 960.0

    def MIDI_InsertEvt(self, take, selected, muted, ppq, message):
        take.events.append((ppq, message))
        return True

    def MIDI_Sort(self, take):
        take.events.sort(key=lambda e: e[0])
        return True

    def GetSetMediaItemInfo_String(self, item, key, value, set_new):
        if set_new:
            item.ext[key] = value
        return True, item.ext.get(key, "")

    def GetMediaItem_Track(self, item):
        return item.track

    def DeleteTrackMediaItem(self, track, item):
        track.items.remove(item)
        return True

    def TimeMap_QNToMeasures(self, proj, qn):
        start = float(int(qn // 4) * 4)
        return int(qn // 4), start, start + 4.0

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

    def GetUserFileNameForRead(self, name, title, ext):
        self.log.append(("browse", title))
        picked = getattr(self, "pick", None)
        return (True, picked) if picked else (False, "")


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

    def tick(self, count=1, playing=False):
        for _ in range(count):
            self.c("tick")
            self.rig.block(playing=playing)
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
        after = self.rig.engine.save()
        self.rig.engine.load(before)
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 0)
        self.assertEqual(self.coordinator.status, "ready")
        self.rig.engine.load(after)
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 1)
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

    def test_the_gate_probe_script_compiles(self):
        import os
        script = os.path.join(os.path.dirname(__file__), "..", "lua", "Furrow_Gate_Probe.lua")
        loaded, err = pair(self.rig.lua.eval("function(path) local f, e = loadfile(path); return f ~= nil, e end")(script))
        self.assertTrue(loaded, err)

    def committed(self):
        """Settles a commit started with commit_pattern and returns the items tracks."""
        self.c("commit_pattern")
        self.tick(30)
        self.settle(80)
        self.tick(12)
        return [t for t in self.stub.tracks if t.ext.get("P_EXT:FurrowItems")]

    def test_a_commit_writes_one_item_a_lane_stores_the_group_once_and_enters_items_mode_in_one_undo_point(self):
        self.painted_lane([0, 4, 8])
        self.c("add_sample", "/samples/snare.wav")
        self.settle()
        self.tick(8)
        self.stub.undo.clear()
        tracks = self.committed()
        self.assertEqual(self.stub.undo, ["Furrow: commit pattern"])
        self.assertEqual(len(tracks), 2)
        self.assertEqual(self.coordinator.session.snapshot.rack.mode, 1)
        rack_id = self.coordinator.session.snapshot.rack.id
        for track in tracks:
            self.assertEqual(track.info["B_MAINSEND"], 0.0)
            self.assertEqual(track.fx[0]["gate"][0], rack_id)
            self.assertEqual(len(track.sends), 1)
            self.assertEqual(track.sends[0]["I_MIDIFLAGS"], 1 << 5)
            self.assertEqual(len(track.items), 1)
            self.assertTrue(track.items[0].ext["P_EXT:Furrow"].startswith("1 group "))
        kick = [t for t in tracks if t.items[0].events][0]
        item = kick.items[0]
        ons = [e for e in item.events if ord(e[1][0]) == 0x90]
        self.assertEqual([round(e[0]) for e in ons], [0, 960, 1920])
        self.assertEqual((item.start, item.stop), (8.0, 8.0 + 16 * 0.25 / 2))
        group = item.ext["P_EXT:Furrow"].split()[2]
        text = self.stub.proj_ext[("FurrowCommits", group)]
        decoded = self.rig.lua.eval('(require("furrow.commit"))').decode(text)
        self.assertEqual((decoded.steps, decoded.origin), (16, 16.0))

    def test_a_commit_that_cannot_build_its_tracks_leaves_nothing_and_restores_the_mode(self):
        self.painted_lane([0, 4])
        self.stub.fail_gate = True
        before = len(self.stub.tracks)
        tracks = self.committed()
        self.assertEqual(tracks, [])
        self.assertEqual(len(self.stub.tracks), before)
        self.assertEqual(self.coordinator.session.snapshot.rack.mode, 0)
        self.assertIn("commit pattern failed", self.coordinator.message)
        self.assertEqual(self.stub.undo, [])

    def test_the_mode_buttons_switch_between_live_and_items_with_one_undo_point_each(self):
        self.painted_lane([0])
        self.c("set_mode", 1)
        self.settle()
        self.tick(8)
        self.assertEqual(self.coordinator.session.snapshot.rack.mode, 1)
        self.c("set_mode", 0)
        self.settle()
        self.tick(8)
        self.assertEqual(self.coordinator.session.snapshot.rack.mode, 0)
        self.assertEqual(self.stub.undo, ["Furrow: play committed items", "Furrow: play live"])

    def test_a_bar_drag_edits_velocity_pitch_and_gate_that_the_engine_plays(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        self.stub.undo.clear()
        lua = self.rig.lua
        ints = lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 0, F_ON, 1])])
        self.c("run", "paint steps", ints)
        self.settle()
        fields = lua.eval('(require("furrow.fields"))')
        velocity, gate, pitch = (fields.list[i] for i in (1, 2, 3))
        for definition, value in ((velocity, 64), (pitch, 7), (gate, 10000)):
            gesture = self.c("begin", "edit " + definition.name.lower())
            self.assertTrue(self.c("set_field", gesture, 0, 0, definition.field, value))
            self.assertFalse(self.c("set_field", gesture, 0, 0, definition.field, value))
            self.c("finish", gesture)
            self.settle()
        self.tick(6)
        self.assertEqual(self.stub.undo[-3:], ["Furrow: edit velocity", "Furrow: edit pitch", "Furrow: edit gate"])
        self.assertEqual([self.c("step_value", 0, 0, f) for f in (1, 3, 9)], [64, 7, 10000])
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(8)
        on = [e for e in self.rig.events if e[2] & 0xF0 == 0x90 and e[4] > 0][0]
        off = [e for e in self.rig.events if e[2] & 0xF0 == 0x80 and e[0] > 0][0]
        self.assertEqual((on[3], on[4]), (67, 64))
        self.assertLessEqual(abs(off[0] - 44100 * 0.5 / 4), 1)

    def test_bar_heights_map_onto_each_field_range(self):
        fields = self.rig.lua.eval('(require("furrow.fields"))')
        velocity, gate, pitch = (fields.list[i] for i in (1, 2, 3))
        self.assertEqual([fields.value_at(velocity, f) for f in (0, 0.5, 1, -3, 7)], [1, 64, 127, 1, 127])
        self.assertEqual([fields.value_at(gate, f) for f in (0, 1)], [500, 10000])
        self.assertEqual([fields.value_at(pitch, f) for f in (0, 0.5, 1)], [-24, 0, 24])
        self.assertAlmostEqual(fields.fraction(pitch, 0), 0.5)
        self.assertEqual(fields.fraction(velocity, 500), 1)

    def test_swing_cut_and_reroll_reach_the_snapshot_as_separate_undo_points(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        self.stub.undo.clear()
        lua = self.rig.lua
        for name, field, value in (("set swing", R_SWING, 3000), ("set swing cut", R_TRUNCATE, 1),
                                   ("reroll chances", R_SEED, 4242)):
            commands = lua.table_from([lua.table_from([OP_SET_RACK, field, value])])
            self.c("run", name, commands)
            self.settle()
        self.tick(6)
        rack = self.coordinator.session.snapshot.rack
        self.assertEqual((rack.swing, rack.truncate, rack.seed), (3000, 1, 4242))
        self.assertEqual(self.stub.undo, ["Furrow: set swing", "Furrow: set swing cut", "Furrow: reroll chances"])
        self.assertEqual(self.c("lanes")[1].swing, 10000)

    def test_a_probability_bar_edit_reaches_the_step(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        fields = self.rig.lua.eval('(require("furrow.fields"))')
        probability = fields.list[6]
        self.assertEqual((probability.name, probability.min, probability.max), ("Probability", 0, 100))
        gesture = self.c("begin", "edit probability")
        self.assertTrue(self.c("set_field", gesture, 0, 3, probability.field, 40))
        self.c("finish", gesture)
        self.settle()
        self.tick(6)
        self.assertEqual(self.c("step_value", 0, 3, 12), 40)
        self.assertEqual(self.c("step_value", 0, 4, 12), 100)

    def test_shift_and_ratchet_bars_map_their_ranges_and_reach_the_step(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        fields = self.rig.lua.eval('(require("furrow.fields"))')
        shift, ratchet = fields.list[4], fields.list[5]
        self.assertEqual((shift.name, shift.min, shift.max), ("Shift", 0, 9900))
        self.assertEqual((ratchet.name, ratchet.min, ratchet.max), ("Ratchet", 1, 8))
        self.assertEqual([fields.value_at(ratchet, f) for f in (0, 0.5, 1)], [1, 5, 8])
        self.assertEqual(ratchet.text(4), "4x")
        gesture = self.c("begin", "edit ratchet")
        self.assertTrue(self.c("set_field", gesture, 0, 2, shift.field, 9900))
        self.assertTrue(self.c("set_field", gesture, 0, 2, ratchet.field, 8))
        self.c("finish", gesture)
        self.settle()
        self.tick(6)
        self.assertEqual([self.c("step_value", 0, 2, f) for f in (8, 10)], [9900, 8])
        self.assertEqual([self.c("step_value", 0, 3, f) for f in (8, 10)], [0, 1])

    def test_the_remaining_step_bars_map_their_ranges_and_reach_the_step(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        fields = self.rig.lua.eval('(require("furrow.fields"))')
        wanted = {"Release": (0, 127), "Fine": (-100, 100), "Pan": (-64, 63), "Mod A": (0, 127), "Mod B": (0, 127)}
        found = {fields.list[i].name: fields.list[i] for i in range(7, 12)}
        self.assertEqual({k: (v.min, v.max) for k, v in found.items()}, wanted)
        self.assertEqual(found["Fine"].text(-25), "-25 ct")
        gesture = self.c("begin", "edit fields")
        for name, value in (("Release", 90), ("Fine", -50), ("Pan", 20), ("Mod A", 100), ("Mod B", 5)):
            self.assertTrue(self.c("set_field", gesture, 0, 1, found[name].field, value))
        self.c("finish", gesture)
        self.settle()
        self.tick(6)
        self.assertEqual([self.c("step_value", 0, 1, f) for f in (2, 4, 5, 6, 7)], [90, -50, 20, 100, 5])
        self.assertEqual([self.c("step_value", 0, 0, f) for f in (2, 4, 5, 6, 7)], [64, 0, 0, 64, 64])

    def test_lane_swing_bend_range_and_mod_controllers_reach_the_snapshot(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        self.tick(6)
        lane = self.c("lanes")[1]
        self.assertEqual((lane.swing, lane.bend, lane.mod_a, lane.mod_b), (10000, 2, 74, 71))
        commands = self.rig.lua.table_from([self.rig.lua.table_from(c) for c in (
            [OP_SET_LANE, 0, 10, 4000], [OP_SET_LANE, 0, 9, 12], [OP_SET_LANE, 0, 13, 1], [OP_SET_LANE, 0, 14, 2])])
        self.c("run", "lane settings", commands)
        self.settle()
        self.tick(6)
        lane = self.c("lanes")[1]
        self.assertEqual((lane.swing, lane.bend, lane.mod_a, lane.mod_b), (4000, 12, 1, 2))
        self.assertEqual((lane.volume, lane.pan), (5000, 0))

    def test_a_long_lane_shows_every_step_in_one_window_and_plays_to_its_length(self):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        self.stub.undo.clear()
        lua = self.rig.lua
        self.c("set_length", 0, 40)
        self.settle()
        steps = lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 3, F_ON, 1]),
                                lua.table_from([OP_SET_STEP, 0, 0, 35, F_ON, 1])])
        self.c("run", "paint steps", steps)
        self.settle()
        self.tick(8)
        self.assertEqual(self.stub.undo, ["Furrow: set length", "Furrow: paint steps"])
        self.assertEqual(self.c("lanes")[1].length, 40)
        self.assertTrue(self.c("page_ready"))
        self.assertEqual([self.c("step_value", 0, s, F_ON) for s in (3, 35)], [1, 1])
        window = self.coordinator.session.window
        self.assertEqual((window.nl, window.s0, window.ns), (1, 0, 48))
        self.c("set_page", 2)
        self.assertEqual(self.coordinator.page, 0)
        self.rig.events.clear()
        self.rig.seek(0.0)
        self.rig.run(200)
        starts = [round(e[0] / 5512.5) for e in self.rig.events if e[2] == 0x90 and e[4] > 0]
        self.assertEqual(starts, [3, 35])

    def test_lanes_too_long_for_one_window_split_into_pages_that_fit(self):
        for name in ("a", "b", "c", "d", "e"):
            self.c("add_sample", "/samples/%s.wav" % name)
            self.settle()
        lua = self.rig.lua
        self.c("set_length", 0, 512)
        self.settle()
        self.c("run", "paint steps", lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 450, F_ON, 1])]))
        self.settle()
        self.tick(8)
        # Five lanes leave room for 409 steps, which rounds down to 400 in groups of 16.
        self.assertEqual(tuple(self.c("view")), (5, 400, 2))
        window = self.coordinator.session.window
        self.assertEqual((window.nl, window.s0, window.ns), (5, 0, 400))
        self.c("set_page", 1)
        self.tick(8)
        self.assertTrue(self.c("page_ready"))
        window = self.coordinator.session.window
        self.assertEqual((window.s0, window.ns), (400, 112))
        self.assertEqual(self.c("step_value", 0, 450, F_ON), 1)
        self.c("set_length", 0, 16)
        self.settle()
        self.tick(8)
        self.assertEqual((self.coordinator.page, tuple(self.c("view"))), (0, (5, 16, 1)))

    def lane_steps(self, slot, steps, fields=(0, 1)):
        return {s: [self.c("step_value", slot, s, f) for f in fields] for s in steps}

    def run_tool(self, kind, arg=None):
        self.c("lane_tool", 0, kind, arg)
        self.settle(60)
        self.tick(10)

    def painted_lane(self, on_steps, length=None):
        self.c("add_sample", "/samples/kick.wav")
        self.settle()
        if length:
            self.c("set_length", 0, length)
            self.settle()
        commands = self.rig.lua.table_from([self.rig.lua.table_from([OP_SET_STEP, 0, 0, s, F_ON, 1])
                                            for s in on_steps])
        self.c("run", "paint steps", commands)
        self.settle()
        self.tick(10)
        self.stub.undo.clear()

    def on_steps(self, count=16):
        return [s for s in range(count) if self.c("step_value", 0, s, F_ON) == 1]

    def test_fill_every_n_turns_on_those_steps_and_clears_the_rest_in_one_undo_point(self):
        self.painted_lane([1, 5, 6])
        self.run_tool("fill", 4)
        self.assertEqual(self.on_steps(), [0, 4, 8, 12])
        self.assertEqual(self.stub.undo, ["Furrow: fill every 4"])
        self.run_tool("fill", 3)
        self.assertEqual(self.on_steps(), [0, 3, 6, 9, 12, 15])

    def test_rotate_moves_steps_with_their_fields_and_wraps_across_pages(self):
        self.painted_lane([2, 39], length=40)
        lua = self.rig.lua
        edit = lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 2, 1, 64])])
        self.c("run", "edit velocity", edit)
        self.settle()
        self.tick(8)
        self.stub.undo.clear()
        self.run_tool("rotate", 1)
        self.assertEqual(self.stub.undo, ["Furrow: rotate right"])
        self.c("set_page", 0)
        self.tick(8)
        self.assertEqual(self.lane_steps(0, (0, 2, 3)), {0: [1, 100], 2: [0, 100], 3: [1, 64]})
        self.c("set_page", 2)
        self.tick(8)
        self.assertEqual(self.lane_steps(0, (38, 39)), {38: [0, 100], 39: [0, 100]})
        self.run_tool("rotate", -1)
        self.assertEqual(self.stub.undo[-1], "Furrow: rotate left")
        self.assertEqual(self.lane_steps(0, (38, 39)), {38: [0, 100], 39: [1, 100]})

    def test_invert_and_clear_work_on_the_whole_lane_and_clear_resets_every_field(self):
        self.painted_lane([0, 4])
        self.run_tool("invert")
        self.assertEqual(self.on_steps(), [s for s in range(16) if s not in (0, 4)])
        self.assertEqual(self.stub.undo, ["Furrow: invert steps"])
        lua = self.rig.lua
        self.c("run", "edit velocity", lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 1, 1, 33])]))
        self.settle()
        self.run_tool("clear")
        self.assertEqual(self.on_steps(), [])
        self.assertEqual(self.c("step_value", 0, 1, 1), 100)
        self.assertEqual(self.rig.engine.var("nd_count"), 0)
        self.assertEqual(self.stub.undo[-1], "Furrow: clear steps")

    def test_a_tool_that_changes_nothing_makes_no_undo_point_and_the_grid_window_returns(self):
        self.painted_lane([0, 4, 8, 12])
        self.run_tool("fill", 4)
        self.assertEqual(self.stub.undo, [])
        self.assertEqual(self.on_steps(), [0, 4, 8, 12])
        self.assertIsNone(self.coordinator.tool)
        window, grid = self.coordinator.session.window, self.c("window")
        self.assertEqual((window.nl, window.s0, window.ns), (grid.nl, grid.s0, grid.ns))
        self.assertIsNone(self.coordinator.session.side)

    def test_a_tool_waits_for_pending_edits_before_it_reads_the_lane(self):
        self.painted_lane([])
        gesture = self.c("begin", "paint steps")
        self.c("add", gesture, *self.edit(0, 7, 1))
        self.c("lane_tool", 0, "invert", None)
        self.c("finish", gesture)
        self.settle(60)
        self.tick(10)
        self.assertEqual(self.on_steps(), [s for s in range(16) if s != 7])

    def pattern_names(self):
        return [(e.index, e.name) for e in self.c("patterns").values()]

    def test_choosing_a_pattern_while_stopped_shows_and_plays_it_at_once(self):
        self.painted_lane([])
        self.c("select_pattern", 2)
        self.settle()
        self.tick(6)
        self.assertEqual((self.rig.engine.var("pattern"), self.coordinator.pattern), (2, 2))
        self.assertEqual(self.stub.undo, ["Furrow: switch pattern"])
        lua = self.rig.lua
        self.c("run", "paint steps", lua.table_from([lua.table_from([OP_SET_STEP, 2, 0, 3, F_ON, 1])]))
        self.settle()
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 1)
        self.c("select_pattern", 0)
        self.settle()
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 0)
        self.c("select_pattern", 2)
        self.settle()
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 3, F_ON), 1)

    def test_edits_go_to_the_shown_pattern(self):
        self.painted_lane([])
        self.c("select_pattern", 4)
        self.settle()
        self.tick(6)
        gesture = self.c("begin", "paint steps")
        self.assertTrue(self.c("set_field", gesture, 0, 9, F_ON, 1))
        self.c("finish", gesture)
        self.settle()
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 9, F_ON), 1)
        self.c("select_pattern", 0)
        self.settle()
        self.tick(8)
        self.assertEqual(self.c("step_value", 0, 9, F_ON), 0)

    def test_a_new_pattern_gets_the_next_free_index_a_default_name_and_one_undo_point(self):
        self.painted_lane([])
        self.c("new_pattern")
        self.settle()
        self.tick(6)
        self.assertEqual(self.pattern_names(), [(0, "Pattern 1"), (1, "Pattern 2")])
        self.assertEqual((self.coordinator.pattern, self.rig.engine.var("pattern")), (1, 1))
        self.assertEqual(self.stub.undo, ["Furrow: new pattern"])
        self.assertIn("pattern 1 Pattern 2", self.rack["rack"].ext["P_EXT:FurrowPatterns"])
        self.c("new_pattern")
        self.settle()
        self.assertEqual([i for i, _ in self.pattern_names()], [0, 1, 2])

    def test_renaming_a_pattern_cleans_the_name_and_makes_one_host_only_undo_point(self):
        self.painted_lane([])
        self.c("new_pattern")
        self.settle()
        self.stub.undo.clear()
        revision = self.rig.engine.var("revision")
        self.c("rename_pattern", 1, "  Verse\n two  ")
        self.settle()
        self.assertEqual(self.pattern_names(), [(0, "Pattern 1"), (1, "Verse  two")])
        self.assertEqual(self.stub.undo, ["Furrow: rename pattern"])
        self.assertEqual(self.rig.engine.var("revision"), revision)
        self.c("rename_pattern", 1, "   ")
        self.settle()
        self.assertEqual(self.stub.undo, ["Furrow: rename pattern"])

    def test_clearing_a_pattern_empties_only_that_pattern(self):
        self.painted_lane([1, 2])
        self.c("new_pattern")
        self.settle()
        self.tick(6)
        lua = self.rig.lua
        self.c("run", "paint steps", lua.table_from([lua.table_from([OP_SET_STEP, 1, 0, 5, F_ON, 1])]))
        self.settle()
        self.assertEqual(self.rig.engine.var("nd_count"), 3)
        self.c("clear_pattern", 1)
        self.settle()
        self.tick(8)
        self.assertEqual(self.rig.engine.var("nd_count"), 2)
        self.assertEqual(self.c("step_value", 0, 5, F_ON), 0)
        self.c("select_pattern", 0)
        self.settle()
        self.tick(8)
        self.assertEqual(self.on_steps(), [1, 2])

    def test_choosing_a_pattern_while_playing_queues_the_next_measure_start(self):
        self.painted_lane([])
        self.tick(4, playing=True)
        self.stub.play_time = 3.1
        self.c("select_pattern", 1)
        self.assertEqual(self.coordinator.pattern, 1)
        self.tick(4, playing=True)
        self.assertEqual(self.rig.engine.var("pend_pattern"), 1)
        self.assertEqual(self.rig.engine.var("pend_beat"), 8.0)
        self.assertEqual((self.coordinator.engine_pattern, self.rig.engine.var("pattern")), (0, 0))
        self.assertEqual(self.coordinator.pattern, 1)

    def test_the_grid_follows_the_engine_when_it_changes_pattern_by_itself(self):
        self.painted_lane([])
        before = self.rig.engine.save()
        self.c("select_pattern", 3)
        self.settle()
        self.tick(6)
        self.assertEqual(self.coordinator.pattern, 3)
        self.rig.engine.load(before)
        self.tick(10)
        self.assertEqual((self.coordinator.pattern, self.rig.engine.var("pattern")), (0, 0))

    def set_fields(self, *edits):
        gesture = self.c("begin", "edit steps")
        for slot, step, field, value in edits:
            self.c("set_field", gesture, slot, step, field, value)
        self.c("finish", gesture)
        self.settle()
        self.tick(8)

    def test_cloning_copies_every_value_and_length_into_a_new_pattern_in_one_undo_point(self):
        self.painted_lane([3, 35], length=40)
        self.set_fields((0, 3, 1, 64), (0, 35, 12, 40))
        self.stub.undo.clear()
        self.c("clone_pattern")
        self.settle(80)
        self.tick(12)
        self.assertEqual(self.pattern_names(), [(0, "Pattern 1"), (1, "Pattern 1 copy")])
        self.assertEqual((self.coordinator.pattern, self.rig.engine.var("pattern")), (1, 1))
        self.assertEqual(self.stub.undo, ["Furrow: clone pattern"])
        self.assertEqual(self.c("lanes")[1].length, 40)
        for pattern in (1, 0, 1):
            self.c("select_pattern", pattern)
            self.settle()
            self.tick(10)
            self.assertEqual([self.c("step_value", 0, 3, f) for f in (0, 1)], [1, 64])
            self.assertEqual(self.c("step_value", 0, 4, 0), 0)
            self.c("set_page", 2)
            self.tick(10)
            self.assertEqual([self.c("step_value", 0, 35, f) for f in (0, 12)], [1, 40])
            self.c("set_page", 0)
            self.tick(10)

    def test_cloning_leaves_the_source_alone_when_the_copy_is_edited(self):
        self.painted_lane([2])
        self.c("clone_pattern")
        self.settle(80)
        self.tick(10)
        self.set_fields((0, 2, 0, 0), (0, 7, 0, 1))
        self.assertEqual(self.on_steps(), [7])
        self.c("select_pattern", 0)
        self.settle()
        self.tick(10)
        self.assertEqual(self.on_steps(), [2])

    def test_a_clone_that_fails_part_way_rolls_the_copy_back_and_returns_to_the_source(self):
        self.painted_lane([2])
        lua = self.rig.lua
        opts = lua.eval("""{ abort_on_failure = true, rollback = { { 2, 0, 0, 9, 0, 1 } },
                              on_abort = function(c) ABORTED = true end,
                              after = function(c) AFTER_RAN = true; return true end }""")
        lua.globals().ABORTED = False
        lua.globals().AFTER_RAN = False
        gesture = self.c("begin", "bad", opts)
        good = lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 5, F_ON, 1])])
        bad = lua.table_from([lua.table_from([OP_SET_STEP, 0, 0, 6, 99, 1])])
        self.c("add", gesture, good)
        self.c("add", gesture, bad)
        self.c("finish", gesture)
        self.settle()
        self.tick(8)
        self.assertTrue(lua.globals().ABORTED)
        self.assertFalse(lua.globals().AFTER_RAN)
        self.assertIn("bad failed", self.coordinator.message)
        self.assertEqual(self.stub.undo, [])
        self.assertEqual(self.c("step_value", 0, 9, F_ON), 1)

    def aborting_gesture(self, commands, after):
        lua = self.rig.lua
        opts = lua.eval("""{ abort_on_failure = true, rollback = { { 2, 0, 0, 9, 0, 1 } },
                              on_abort = function(c) ABORTED = true end,
                              after = function(c) return %s end }""" % after)
        lua.globals().ABORTED = False
        gesture = self.c("begin", "bad", opts)
        self.c("add", gesture, lua.table_from([lua.table_from(commands)]))
        self.c("finish", gesture)
        self.settle()
        self.tick(8)
        return lua.globals().ABORTED

    def test_a_clone_whose_first_step_fails_still_restores_the_source_view(self):
        self.painted_lane([2])
        self.assertTrue(self.aborting_gesture([OP_SET_STEP, 0, 0, 6, 99, 1], "true"))
        self.assertIn("bad failed", self.coordinator.message)

    def test_a_clone_whose_host_changes_fail_restores_the_source_view(self):
        self.painted_lane([2])
        self.assertTrue(self.aborting_gesture([OP_SET_STEP, 0, 0, 5, F_ON, 1], "false, 'no rack'"))
        self.assertIn("no rack", self.coordinator.message)
        self.assertEqual(self.stub.undo, [])

    def test_cloning_right_after_a_length_change_copies_the_new_length(self):
        self.painted_lane([3])
        self.c("set_length", 0, 40)
        self.c("clone_pattern")
        self.tick(60)
        self.settle(80)
        self.tick(12)
        self.assertEqual(self.coordinator.pattern, 1)
        self.assertEqual(self.c("lanes")[1].length, 40)

    def rack_links(self):
        return self.rack["rack"].ext.get("P_EXT:Furrow", "")


class FakeImGui:
    """Answers ReaImGui calls with neutral values and can hand the window one dropped file."""

    def __init__(self):
        self.calls = []
        self.drop = []
        self.small_labels = []
        self.press = None
        self.menu_open = False
        self.popup = None
        self.expanded = True
        self.typed = None
        self.slide = None

    def table(self, lua):
        fake = self

        def call(name):
            def fn(*args):
                fake.calls.append(name)
                if name == "InputInt" and fake.typed is not None:
                    typed, fake.typed = fake.typed, None
                    return True, typed
                if name == "SliderInt" and fake.slide and args[1] == fake.slide[0]:
                    _, value = fake.slide
                    fake.slide = None
                    return True, value
                if name == "BeginPopupContextItem":
                    return fake.menu_open
                if name == "BeginPopup":
                    return args[1] == fake.popup
                if name == "CollapsingHeader":
                    return fake.expanded
                if name == "MenuItem" and args[1] == fake.press:
                    fake.press = None
                    return True
                if name in ("SmallButton", "Button"):
                    fake.small_labels.append(args[1])
                    if args[1] == fake.press:
                        fake.press = None
                        return True
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
            imgui.popup = "groove"
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
            self.assertIn("Dummy", imgui.calls)
            self.assertIn("SliderInt", imgui.calls)
            self.assertIn("Checkbox", imgui.calls)
            self.assertIn("BeginCombo", imgui.calls)
            self.assertIn("CollapsingHeader", imgui.calls)
            self.assertEqual(stub.undo, ["Furrow: add lane"])
        finally:
            rig.close()

    def test_a_longer_lane_grows_its_row_instead_of_adding_pages(self):
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
            for frame in range(60):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 25:
                    imgui.press = "+16##length16"
                if frame == 35:
                    imgui.typed = 100
                if frame == 36:
                    imgui.press = "Set##lengthset"
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            # A 100-step lane fits one window, so its row grows and no page buttons appear.
            self.assertFalse([label for label in imgui.small_labels if "##page" in label])
            imgui.calls.clear()
            pending, stub.deferred = stub.deferred, []
            for fn in pending:
                fn()
            # One frame draws a cell and a bar for each of the 100 steps.
            self.assertEqual(imgui.calls.count("InvisibleButton"), 200)
        finally:
            rig.close()

    def test_the_lane_menu_starts_a_tool(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(70):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 30:
                    imgui.menu_open = True
                    imgui.press = "Fill every 4"
                if frame == 31:
                    imgui.menu_open = False
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(stub.undo, ["Furrow: add lane", "Furrow: fill every 4"])
        finally:
            rig.close()

    def test_the_clone_button_starts_a_clone(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(90):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 30:
                    imgui.popup = "pattern menu"
                    imgui.press = "Clone pattern##pattern"
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(stub.undo, ["Furrow: add lane", "Furrow: clone pattern"])
        finally:
            rig.close()

    def test_the_lane_volume_and_pan_sliders_each_make_one_undo_point(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(80):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 30:
                    imgui.slide = ("Volume", 80)
                if frame == 45:
                    imgui.slide = ("Pan##lane", -30)
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(stub.undo, ["Furrow: add lane", "Furrow: set lane volume", "Furrow: set lane pan"])
        finally:
            rig.close()

    def test_the_lane_setting_buttons_each_make_one_undo_point(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(80):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 30:
                    imgui.press = "+##bend"
                if frame == 40:
                    imgui.press = "-##mod_a"
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(stub.undo, ["Furrow: add lane", "Furrow: set bend range", "Furrow: set mod a cc"])
        finally:
            rig.close()

    def test_the_pattern_buttons_start_gestures(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(70):
                if frame == 6:
                    imgui.drop.append("/samples/perc.wav")
                if frame == 30:
                    imgui.popup = "pattern menu"
                    imgui.press = "New pattern##pattern"
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual(stub.undo, ["Furrow: add lane", "Furrow: new pattern"])
        finally:
            rig.close()

    def test_clicking_the_drop_zone_browses_for_a_file_that_becomes_a_lane(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            stub.pick = "/samples/kick.wav"
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            lua.eval('(require("furrow.host"))').create_rack()
            imgui = FakeImGui()
            ui = lua.eval('(require("furrow.ui"))')
            ui.run(imgui.table(lua))
            for frame in range(40):
                if frame == 6:
                    imgui.press = "Drop audio files here, or click to browse##addlanes"
                if frame == 20:
                    stub.pick = None
                    imgui.press = "+ Add audio files: click to browse, or drop them here##addlanes"
                pending, stub.deferred = stub.deferred, []
                for fn in pending:
                    fn()
                rig.block(playing=False)
                stub.clock += 0.03
            self.assertEqual([t.name for t in stub.tracks], ["Furrow Rack", "Furrow Engine", "kick"])
            self.assertEqual([e for e in stub.log if e[0] == "browse"], [("browse", "Add an audio file to Furrow")] * 2)
            self.assertEqual(stub.undo, ["Furrow: add lane"])
        finally:
            rig.close()

    def test_the_multi_file_dialog_answer_becomes_one_path_each(self):
        rig = Rig()
        try:
            stub = StubReaper(rig)
            lua = rig.lua
            lua.globals().reaper = stub.table(lua)
            ui = lua.eval('(require("furrow.ui"))')
            answers = {"several": (1, "/samples/drums\0kick.wav\0snare.wav"), "one": (1, "/samples/hat.wav"),
                       "cancel": (0, "")}
            for case, (code, text) in answers.items():
                lua.globals().reaper.JS_Dialog_BrowseForOpenFiles = lambda *a, code=code, text=text: (code, text)
                files = ui.browse_files()
                got = [files[i] for i in range(1, len(files) + 1)]
                expected = {"several": ["/samples/drums/kick.wav", "/samples/drums/snare.wav"],
                            "one": ["/samples/hat.wav"], "cancel": []}[case]
                self.assertEqual(got, expected, case)
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
