The first proof has two parts. Python checks the schedule. REAPER has to check the sound.

**Checked on this machine.** `python3 probes/check_schedule.py` checks the half-sample tie, the 120 BPM step and half-step gate, the full and partial ramp integrals, the 38 events of the 4 by 16 pattern, and comparator routing, ordering, missing/extra event and timing failures. `probes/schedule.py` independently computes expected samples from the written beat and tempo map; it does not call SeqCore or use SeqCore's event-generation code. The checked-in expected files are regenerated from that independent calculation. `python3 probes/check_lua_scripts.py` runs the Lua probes against a stub REAPER API under Lua 5.4. It needs `lupa` and skips without it. The stub shows what the scripts ask REAPER to do. It does not show that REAPER honours it. `probes/expected/constant-120.txt` is the file a fresh sequenced receiver capture compares against. `probes/expected/ramp-44100-1024.txt` uses the map in `probes/cases/ramp-44100-1024.txt`, with BPM linear in time as measured on REAPER 7.82.

1. Copy every file from `jsfx/` into the REAPER Effects directory, or link them there. Each probe names the JSFX it loads.
2. Run `lua/furrow_probe_layout.lua` once. It gives each receiver its own slot. A second run finds the named tracks and stops. Remove those tracks before creating the layout again. If a JSFX fails to load, the script removes what it created and says which file is missing.
3. Set 44.1 kHz, 1,024-sample blocks, and a constant 120 BPM. Play from beat 0 through the sixteen steps, then stop. Starting playback clears the previous capture.
4. Run `lua/furrow_probe_dump.lua`. It writes three files next to the saved project, or into the REAPER resource directory for an unsaved project. The console lists the paths.
5. Run `python3 probes/diff_log.py probes/expected/constant-120.txt furrow-capture-events.txt`.
6. For a tempo case, write its project from a saved probe project instead of placing markers by hand. Run `python3 probes/make_tempo_project.py probes/cases/ramp-edge-44100-1024.txt constant.RPP ramp-edge.RPP`, then open the new project. The same works for `step-on-beat-44100-1024.txt` and `step-off-grid-44100-1024.txt`.
7. Repeat with the ramp case. `furrow-capture-tempo-block.txt` holds the tempo read in `@block`. `furrow-capture-tempo-sample.txt` holds the tempo read in `@sample`, one line per played sample.

The receiver logger now preserves its observed sequence separately for each receiver, and the dump retains that sequence. The verifier checks receiver, lane, destination MIDI bus, channel, note data, event count, sample tolerance and the documented event-class ordering at equal observed sample offsets. Its event-class rule is note-off, then controller, then note-on. The specification does not define order within one event class or a total order across different receiver tracks, so the verifier does not invent either. `src_bus` is not observable at the receiving JSFX after REAPER's send remaps the bus; routing evidence is the receiver identity plus the observed destination bus/channel and lane mapping. Legacy captures have already discarded sequence and receiver identity; the verifier rejects them as unverifiable rather than reporting a pass.

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

**Latency probe.** `lua/furrow_probe_latency.lua` builds a folder with the probe engine and two lanes in a new, saved project. Each lane has a receiver, then `jsfx/furrow_probe_click.jsfx`, which turns every note-on into a one-sample impulse of 1.0, lane 1 on the left and lane 2 on the right. Lane 2 then has `jsfx/furrow_probe_latency.jsfx`, which delays audio by its slider and reports that delay through `pdc_delay`. `jsfx/furrow_probe_impulse_meter.jsfx` on the folder logs the project sample of every impulse in named gmem `FurrowProbeLatency` and silences the folder. The script plays the engine's 16 steps at 120 BPM with 0 and then 1,000 samples of latency. For each lane it compares the folder impulses and the receiver note-ons with the step times, and it records what `TrackFX_GetNamedConfigParm` returns for `pdc`, `chain_pdc_reporting` and `chain_pdc_actual`. The results go to `furrow-latency-results.txt`.

**Render probe.** `lua/furrow_probe_render.lua` renders the first 3 seconds of a saved timing project, such as `constant.RPP` or `ramp.RPP`, with REAPER's render action that closes its own dialog. It clears the receiver counts and turns on logging while stopped first, in case a render does not count as playback. It then writes `furrow-render-<project>-events.txt` in the dump format and restores the project's render settings. It records the render time, the rendered file, how many block tempo entries logged and the render block sizes. Diff the events file with `probes/diff_log.py` against that case's expected file.

**Cleanup probe.** `lua/furrow_probe_cleanup.lua` measures two things in a new, saved project. First, named gmem lifetime. It writes a mark into a fresh segment, attaches another segment so the script no longer holds the first, and reads the mark back at once and after 0.5 s. It repeats that for a segment the running meter JSFX holds. Second, it holds a note in RS5K through the RS5K probe's driver and meter and sends All Notes Off, CC 123, or All Sound Off, CC 120, with Obey note-offs on and off. It records the level change after each. The results go to `furrow-cleanup-results.txt`.

**gmem layout, version 4.** Named gmem `FurrowProbeReceiver`. `gmem[0]` is the layout version. `gmem[2]` and `gmem[3]` count block and sample tempo entries. `gmem[4]` is the project sample of the first sample tempo. `gmem[10 + slot]` counts slot events. Block entries start at 1,000, three numbers each: project sample, tempo and `beat_position`. Sample tempos start at 200,000. Slot events start at 2,000,000 + slot × 500,000, nine numbers each: sample, per-receiver sequence, receiver slot, mapped lane, observed destination MIDI bus, destination channel, note, velocity, kind. The current layout maps receiver slot to lane ID. Kind 1 is a note-on, kind 0 is a note-off and kind 2 is a controller. Sample positions come from `play_position` plus MIDI offset, so they count from project time zero. The dump does not sort within a receiver; it writes receivers in slot order and events in the sequence captured by that receiver.

**Known limits.** REAPER 7.82 reports `tempo` once per block, at its first sample. The engine assumes the previous block's tempo change continues through the current block and converts beats with that ramp. Historical captures in `probes/results/reaper-7.82-linux/` predate the sequenced logger and are now rejected by the verifier because they discarded receiver identity and receive order. They remain historical observations only; they do not satisfy current timing evidence. The historical ramp run recorded a two-sample-late final note-off, outside the one-sample bound. Do not infer a cause or adjust the tolerance from that artifact. A fresh matrix must use the current source and version 4 receiver logger. A tempo step between grid lines remains a documented engine limitation for events following the step inside the same block, by up to one block. The [ReaScript reference](https://www.reaper.fm/sdk/reascript/reascripthelp.html) documents the `I_MIDIFLAGS` source bus in bits 14 to 21 and the destination bus in bits 22 to 29, where 0 means all and 1 means the normal bus. The layout selects bus 1 for both. A capture still has to show that the host honours it. The receiver stores its observed `midi_bus` and destination channel for each event. The receiver declares stereo pins so that `@sample` has audio to run on.
