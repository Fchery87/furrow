The first proof has two parts. Python checks the schedule. REAPER has to check the sound.

**Checked on this machine.** `python3 probes/check_schedule.py` checks the half-sample tie, the 120 BPM step and half-step gate, the full and partial ramp integrals, the 38 events of the 4 by 16 pattern, and the one-sample diff. `python3 probes/check_lua_scripts.py` runs both Lua scripts against a stub REAPER API under Lua 5.4. It needs `pip install lupa` and skips without it. The stub shows what the scripts ask REAPER to do. It does not show that REAPER honours it. `probes/expected/constant-120.txt` is the file a capture compares against. `probes/expected/ramp-44100-1024.txt` uses the map in `probes/cases/ramp-44100-1024.txt`, with BPM linear in time as REAPER 7.82 played it.

1. Copy every file from `jsfx/` into the REAPER Effects directory, or link them there. Each probe names the JSFX it loads.
2. Run `lua/furrow_probe_layout.lua` once. It gives each receiver its own slot. A second run finds the named tracks and stops. Remove those tracks before creating the layout again. If a JSFX fails to load, the script removes what it created and says which file is missing.
3. Set 44.1 kHz, 1,024-sample blocks, and a constant 120 BPM. Play from beat 0 through the sixteen steps, then stop. Starting playback clears the previous capture.
4. Run `lua/furrow_probe_dump.lua`. It writes three files next to the saved project, or into the REAPER resource directory for an unsaved project. The console lists the paths.
5. Run `python3 probes/diff_log.py probes/expected/constant-120.txt furrow-capture-events.txt`.
6. For a tempo case, write its project from a saved probe project instead of placing markers by hand. Run `python3 probes/make_tempo_project.py probes/cases/ramp-edge-44100-1024.txt constant.RPP ramp-edge.RPP`, then open the new project. The same works for `step-on-beat-44100-1024.txt` and `step-off-grid-44100-1024.txt`.
7. Repeat with the ramp case. `furrow-capture-tempo-block.txt` holds the tempo read in `@block`. `furrow-capture-tempo-sample.txt` holds the tempo read in `@sample`, one line per played sample.

**Undo probe.** `lua/furrow_probe_undo.lua` tests the milestone 1 edit and undo gate. Open one project built by the layout script, with no other probe project open, because every engine instance shares the `FurrowProbeEngine` gmem. Run the script from the action list and leave REAPER alone until the console prints the failure count. It runs every test while stopped and again while playing.

Each gesture marks the engine state as changed before its undo point, in one of three ways. `none` adds no mark. `lua` sets the engine's hidden State revision parameter after the acknowledgement. `engine` has the engine bump that parameter with `sliderchange` when it applies the command.

- **A.** Toggle a step, wait for the acknowledgement, mark, then create an FX undo point. Undo and redo must restore the step.
- **B.** Toggle a step that the engine applies 20 blocks later, and rename the folder track with its own undo point while the toggle is pending. The first undo must restore only the step, and the second only the name.
- **C.** Toggle a step and add a track, wait for the acknowledgement, mark, then create one undo point for both. One undo must remove both.

On REAPER 7.82 only the `lua` mark passed. With no mark or the `engine` mark, REAPER created no FX-only undo point and never asked the engine to reload its state. See the edit and undo section of `probes/results/reaper-7.82-linux/README.md`.

The engine saves its steps through `@serialize` and counts each restore in `gmem[8]`. The script writes `furrow-undo-results.txt` next to the project. `probes/check_lua_scripts.py` runs it against a simulated REAPER, once with an undo that rereads engine state and once with an undo that rereads it only when a parameter changes. That shows the script reports each case. It does not show what REAPER does.

**Memory probe.** `lua/furrow_probe_memory.lua` measures what the spec's memory-scope proof needs before any slot arbitration or address map exists. It loads `jsfx/furrow_probe_memory.jsfx` on two tracks, copies one of them to a third, then opens a new project tab with a fourth. Each instance reports through hidden parameters as well as gmem, so a separate gmem per tab still shows up.

- **Scope.** Whether instances in one project, a copied FX and an instance in another tab all see a value Lua writes, and whether Lua sees what they write.
- **Background tabs.** Whether an instance in a tab that is not active keeps processing.
- **Identity.** Whether a copied FX keeps the original's persistent id and gets a new runtime token.
- **Size.** Which named gmem indices round-trip, from 1,048,575 up to 33,554,431, written by Lua and by an instance.
- **Local memory.** The instance's `__memtop()` with `maxmem=8388608`, and a write at local index 8,388,607.

Start with only one project tab open. The script writes `furrow-memory-results.txt` next to that project. Close the new tab without saving afterwards.

**Save and reload probe.** `lua/furrow_probe_state.lua` tests the spec's proposed origin encoding. `jsfx/furrow_probe_state.jsfx` stores each value as four unsigned 16-bit words, least significant first, split by exact arithmetic because EEL2 has no bit cast. It writes them through `@serialize`, with the seeds 0, 16,777,215 and 16,777,217, and a raw `file_var` of 1/3 as a control. The values are zero, negative zero, -16.25, 1/3, 288,000 + 1/3, -(2^30) - 1/7, 2^52 + 2 and 2^-30. EEL2's `==` has a tolerance, so the JSFX compares with `===`. The script compares the stored words with `string.pack("<d")`.

Save the project, then run the script. The first run adds the probe track, round-trips its state through a track chunk, and saves. Reopen the project from File > Recent projects and run the script again. Both runs append to `furrow-state-results.txt`. A Python copy of the word split matched `struct.pack` on 199,929 finite doubles.

**Lifecycle probe.** `lua/furrow_probe_lifecycle.lua` measures the host lifecycle cases the spec requires before milestone 2. Run it in a project built by the layout script, with no other project tab open, and do not save afterwards.

- **L1.** A user edit with its own undo point lands while a gesture waits for the engine. The gesture's track change runs either before the command or after the acknowledgement, just before the gesture's undo point.
- **L2.** The engine is bypassed while a command is pending.
- **L3.** The engine goes offline and back online after an edit with and without the State revision mark.
- **L4.** A new project tab opens while a command is pending.
- **L5.** A note is held when the engine is bypassed or taken offline, or when the transport stops, at 30 BPM. The receiver logs controllers as kind 2, and logs while stopped when its gmem[5] is 1.

The script writes `furrow-lifecycle-results.txt` next to the project. `probes/check_lua_scripts.py` runs it against a simulated REAPER to show it completes.

**RS5K probe.** `lua/furrow_probe_rs5k.lua` measures whether ReaSamplOmatic5000 responds to the controls a Furrow lane sends. It writes a 3-second 1 kHz sine next to the project and loads it into RS5K. RS5K sits between `jsfx/furrow_probe_driver.jsfx`, which sends the MIDI the script asks for, and `jsfx/furrow_probe_meter.jsfx`, which measures each channel's level and counts zero crossings, then silences the track.

- **Notes.** The level after a note-off with default settings. Then, with Obey note-offs on, level, pitch and the level after a note-off. Velocity 64 and 1 against 127, at the default and the lowest Gain for minimum velocity.
- **Raw controllers.** CC 7 and CC 10 sent straight to RS5K. Pitch bend at both extremes.
- **Parameter links.** CC 7 to Volume and CC 10 to Pan through `param.X.plink.*` with `effect=-100`, the mechanism the spec plans for parameter locks.
- **MIDI learn.** The same two mappings through `param.X.learn.midi1` and `midi2`.

The script records every RS5K parameter with its value, and reads each link setting back. It also records Volume and Pan after their links are removed. Before every note-on it measures 0.1 s of output and fails if a voice is still sounding, because a leftover voice adds to the next measurement at an unknown phase. Save the project first. The results go to `furrow-rs5k-results.txt`.

**Item metadata probe.** `lua/furrow_probe_items.lua` measures whether item and take extension data, `P_EXT:Furrow`, can hold a committed source snapshot. A full V1 pattern is 64 lanes × 512 steps × 16 values, which is 2 MiB at four hex digits a value. The spec does not fix that encoding yet.

- **Sizes.** Item payloads of 1 KiB, 64 KiB, 2 MiB and 8 MiB, plus a 64 KiB take payload. Each is a seeded string of 64 printable characters, read back at once and timed.
- **Copies.** The 2 MiB item copied by Item: Duplicate items, by Edit: Copy items and Item: Paste items/tracks, by a state chunk copy, and by a split into two halves.
- **Undo.** A replaced payload, then native undo.
- **Special characters.** Quotes, brackets, tab, pipe, backslash, UTF-8, a newline and chunk syntax, read back at once and through a state chunk copy. These items are deleted before the save.
- **Save and reopen.** The script saves and records the file size. It then clears every payload in memory and reopens the project with `noprompt:`, so the reload readings come from disk.

Run it once in a new, saved project. The take name of each item records its case. The results go to `furrow-items-results.txt`.

**Startup release probe.** `jsfx/furrow_probe_engine.jsfx` now releases every lane on its first block. It sends each lane's note-off and then All Notes Off, CC 123, on the lane channel. It counts those releases in `gmem[11]` and its `@serialize` writes in `gmem[10]`, and sets `gmem[12]` to 1. `lua/furrow_probe_startup.lua` runs in a saved project built by the layout script. It records whether the release runs and reaches all four receivers after a bypass, after offline and online while stopped and while a note is held at 30 BPM, and after a save and reopen. It also counts the state writes while the engine goes down. Restart REAPER first so it loads the changed engine. The results go to `furrow-startup-results.txt`.

**gmem layout, version 3.** Named gmem `FurrowProbeReceiver`. `gmem[0]` is the layout version. `gmem[2]` and `gmem[3]` count block and sample tempo entries. `gmem[4]` is the project sample of the first sample tempo. `gmem[10 + slot]` counts slot events. Block entries start at 1,000, three numbers each. Project sample, tempo and `beat_position`. Sample tempos start at 200,000. Slot events start at 2,000,000 + slot × 500,000, nine numbers each. Sample, order, lane, bus, destination bus, channel, note, velocity, kind. Kind 1 is a note-on. Kind 0 is a note-off. Sample positions come from `play_position` in each block, so they count from project time zero.

**Known limits.** REAPER 7.82 reports `tempo` once per block, at its first sample. The engine assumes the previous block's tempo change continues through the current block and converts beats with that ramp. With the engine that held one tempo per block, the ramp capture missed the one-sample target by one sample on one event. See `probes/results/reaper-7.82-linux/README.md`. The extrapolating engine put all 38 ramp events on the exact sample in REAPER. The engine continues a tempo change only after a block that advanced as a ramp, so a ramp edge or a step makes it hold its tempo for one block. REAPER captures of a ramp edge and a step on a beat pass. A tempo step between grid lines still misplaces events that follow it inside the same block, by up to one block. The [ReaScript reference](https://www.reaper.fm/sdk/reascript/reascripthelp.html) documents the `I_MIDIFLAGS` source bus in bits 14 to 21 and the destination bus in bits 22 to 29, where 0 means all and 1 means the normal bus. The layout selects bus 1 for both. A capture still has to show that the host honours it. The receiver stores `midi_bus` on every event so the capture can show what arrived. The receiver declares stereo pins so that `@sample` has audio to run on.
