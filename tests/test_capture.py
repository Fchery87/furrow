"""Capture from an item, in Lua. Restoring an unedited commit must read back the same source,
and any change to the item must show as edited. Runs the real commit renderer, no REAPER."""

import unittest

from harness import LUA, missing

try:
    from lupa import lua54
except ImportError:
    lua54 = None

SKIP = missing()

SETUP = r'''
package.path = "LUAPATH/?.lua;" .. package.path
commit = require("furrow.commit")
capture = require("furrow.capture")
wire = require("furrow.wire")

function make_data()
  local rows = {}
  for step = 0, 3 do
    local r = {}
    for f = 0, 15 do r[f] = wire.STEP_DEFAULTS[f] end
    r[0] = step % 2
    r[1] = 90 + step
    rows[step] = r
  end
  return {
    origin = 0, steps = 4, seed = 11, swing = 0, truncate = 0, pattern_key = 2,
    lanes = { { id = 7, slot = 0, note = 60, volume = 100, pan = 0, bend = 0, mod_a = 0, mod_b = 0,
                swing = 0, prob_key = 1, length = 4, rows = rows } },
  }
end

function events_of(data)
  local rendered = commit.render(data)
  return rendered[0]
end

function fingerprint_of(events)
  return commit.fingerprint(commit.item_text(4.0, 2.0, 0, events))
end
'''.replace("LUAPATH", LUA.as_posix())


@unittest.skipIf(SKIP or lua54 is None, SKIP or "lupa is not installed")
class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.lua = lua54.LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(SETUP)
        self.lua.execute("data = make_data(); events = events_of(data); stored = commit.encode(data)")
        self.g = self.lua.globals()

    def test_the_metadata_line_parses_and_a_bad_line_does_not(self):
        meta = self.lua.eval('capture.parse_meta("1 group 42 lane 9 rack 3 fp 0123456789abcdef")')
        self.assertEqual((meta.group, meta.lane, meta.rack, meta.fp), (42, 9, 3, "0123456789abcdef"))
        self.assertIsNone(self.lua.eval('capture.parse_meta("2 group 42 lane 9 rack 3 fp 0123456789abcdef")'))
        self.assertIsNone(self.lua.eval('capture.parse_meta("1 group 42 lane 9 rack 3 fp XYZ")'))
        self.assertIsNone(self.lua.eval("capture.parse_meta(nil)"))

    def test_an_item_with_its_written_events_is_unedited(self):
        self.lua.execute("meta = capture.parse_meta('1 group 1 lane 7 rack 3 fp ' .. fingerprint_of(events))")
        self.assertEqual(self.lua.eval("capture.check(meta, 4.0, 2.0, 0, events)"), "unedited")

    def test_a_moved_length_or_changed_event_is_edited(self):
        self.lua.execute("meta = capture.parse_meta('1 group 1 lane 7 rack 3 fp ' .. fingerprint_of(events))")
        self.assertEqual(self.lua.eval("capture.check(meta, 4.0, 2.5, 0, events)"), "edited")
        self.assertEqual(self.lua.eval("capture.check(meta, 4.0, 2.0, 0.25, events)"), "edited")
        self.lua.execute("changed = {}; for i, e in ipairs(events) do changed[i] = e end; changed[3] = "
                         "{ beat = changed[3].beat, status = changed[3].status, data1 = changed[3].data1, data2 = 1 }")
        self.assertEqual(self.lua.eval("capture.check(meta, 4.0, 2.0, 0, changed)"), "edited")

    def test_restore_reads_back_the_source_the_commit_stored(self):
        source = self.lua.eval("capture.restore(stored)")
        lane = source.lanes[1]
        self.assertEqual((source.seed, source.pattern_key, source.steps), (11, 2, 4))
        self.assertEqual((lane.id, lane.length, lane.note), (7, 4, 60))
        self.assertEqual(lane.rows[2][1], 92)

    def test_restore_refuses_a_missing_or_broken_snapshot(self):
        self.assertEqual(self.lua.eval("select(2, capture.restore(nil))"), "the commit's source snapshot is missing")
        self.assertEqual(self.lua.eval("select(2, capture.restore('')) "), "the commit's source snapshot is missing")
        self.assertEqual(self.lua.eval("select(2, capture.restore('nonsense'))"), "not a Furrow commit snapshot")


if __name__ == "__main__":
    unittest.main()
