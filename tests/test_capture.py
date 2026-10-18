"""Furrow item metadata and conversion preview rules."""

import unittest
from pathlib import Path

try:
    from lupa import lua54
except ImportError:
    lua54 = None

ROOT_LUA = (Path(__file__).resolve().parent.parent / "lua").as_posix()
SOURCE = (
    "furrow-commit 1\n"
    "identity rack 3\n"
    "rack seed 1 swing 0 truncate 0 pattern 0 steps 16 origin 0\n"
    "lane 10 slot=0 note=60 volume=10000 pan=0 bend=2 mod_a=74 mod_b=71 "
    "swing=10000 prob_key=3 length=16 target={TRACK-1}\n"
)


@unittest.skipIf(lua54 is None, "lupa is not installed")
class CaptureTest(unittest.TestCase):
    def setUp(self):
        self.lua = lua54.LuaRuntime(unpack_returned_tuples=True, encoding="latin-1")
        self.lua.execute('package.path = "%s/?.lua;" .. package.path' % ROOT_LUA)
        self.capture = self.lua.eval('(require("furrow.capture"))')

    def test_unchanged_commit_metadata_resolves_the_group_snapshot(self):
        metadata = "1 group 7 lane 10 rack 3 fp 0123456789abcdef"

        result = self.capture.inspect(metadata, "0123456789abcdef", SOURCE)

        self.assertEqual(result.kind, "restore")
        self.assertEqual(result.can_restore, True)
        self.assertEqual(result.snapshot.lanes[1].id, 10)

    def test_edited_commit_offers_restore_or_conversion(self):
        metadata = "1 group 7 lane 10 rack 3 fp 0123456789abcdef"

        result = self.capture.inspect(metadata, "fedcba9876543210", SOURCE)

        self.assertEqual(result.kind, "choice")
        self.assertEqual(result.can_restore, True)
        self.assertEqual(result.can_convert, True)

    def test_unrelated_midi_only_allows_conversion(self):
        result = self.capture.inspect("", "0123456789abcdef", None)

        self.assertEqual(result.kind, "convert")
        self.assertEqual(result.can_restore, False)
        self.assertEqual(result.can_convert, True)

    def test_missing_or_invalid_metadata_only_allows_conversion(self):
        for metadata in ("", "broken", "1 group 7 lane 10 rack 3 fp invalid"):
            with self.subTest(metadata=metadata):
                result = self.capture.inspect(metadata, "0123456789abcdef", SOURCE)
                self.assertEqual(result.kind, "convert")
                self.assertEqual(result.can_restore, False)

    def test_conversion_preview_preserves_grid_remainder_pitch_velocity_and_gate(self):
        notes = self.lua.table_from([self.lua.table_from({
            "channel": 1, "pitch": 62, "velocity": 110, "release": 20,
            "start_qn": 0.3125, "end_qn": 0.4375,
        })])
        mapping = self.lua.table_from({1: self.lua.table_from({"slot": 0, "note": 60})})

        preview = self.capture.preview(notes, 0.25, mapping, self.lua.table())

        self.assertEqual(preview.accepted, True)
        row = preview.lanes[0].rows[0]
        self.assertEqual((row[0], row[1], row[2], row[3], row[8], row[9]),
                         (1, 110, 20, 2, 2500, 5000))

    def test_conversion_refuses_unsupported_midi_events_without_an_explicit_loss_choice(self):
        notes = self.lua.table()
        mapping = self.lua.table_from({1: self.lua.table_from({"slot": 0, "note": 60})})
        unsupported = self.lua.table_from(["pitch bend"])

        preview = self.capture.preview(notes, 0, mapping, unsupported)

        self.assertEqual(preview.accepted, False)
        self.assertEqual(preview.reasons[1], "unsupported MIDI events: pitch bend")

    def test_midi_blob_scan_pairs_notes_and_reports_non_note_events(self):
        blob = self.lua.eval('''function()
          return string.pack("i4Bs4", 0, 0, string.char(0x91, 62, 110))
            .. string.pack("i4Bs4", 240, 0, string.char(0x81, 62, 20))
            .. string.pack("i4Bs4", 0, 0, string.char(0xB1, 7, 64))
        end''')()

        notes, unsupported, fingerprint_blob = self.capture.scan_midi(
            blob, self.lua.eval("function(ppq) return ppq / 960 end"), 1.0)

        self.assertEqual((notes[1].channel, notes[1].pitch, notes[1].velocity, notes[1].release),
                         (2, 62, 110, 20))
        self.assertEqual((notes[1].start_qn, notes[1].end_qn), (0, 0.25))
        self.assertEqual(unsupported[1], "control change")
        self.assertTrue(fingerprint_blob)

    def test_reaper_terminal_all_notes_off_does_not_block_note_only_conversion(self):
        blob = self.lua.eval('''function()
          return string.pack("i4Bs4", 0, 0, string.char(0x90, 60, 100))
            .. string.pack("i4Bs4", 120, 0, string.char(0x80, 60, 0))
            .. string.pack("i4Bs4", 3720, 0, string.char(0xB0, 123, 0))
        end''')()

        notes, unsupported, _ = self.capture.scan_midi(
            blob, self.lua.eval("function(ppq) return ppq / 960 end"), 4.0)

        self.assertEqual(len(notes), 1)
        self.assertEqual(len(unsupported), 0)

    def test_selected_note_flags_do_not_change_content_fingerprint_data(self):
        plain = self.lua.eval('string.pack("i4Bs4", 0, 0, string.char(0x90, 60, 100))')
        selected = self.lua.eval('string.pack("i4Bs4", 0, 1, string.char(0x90, 60, 100))')

        self.assertEqual(self.capture.normalize_midi(plain), self.capture.normalize_midi(selected))


if __name__ == "__main__":
    unittest.main()
