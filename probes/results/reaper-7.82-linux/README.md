First routing and timing proof on REAPER 7.82, Linux x86_64, captured on 11 October 2026. The first two captures come from the engine at commit 6944845, which held one tempo per block. The two `-extrapolate` captures come from the engine at commit df58b67, which extrapolates the tempo change inside each block. The `ramp-edge`, `step-on-beat` and `step-off-grid` captures come from the engine in PR #3, which extrapolates only after a block that advanced as a ramp.

**Host.** REAPER v7.82 (4 October 2026) on Linux 7.0.0-38-generic. `reaper.ini` saved 44,100 Hz and a 1,024-sample block with `linux_audio_mode=2`. The block tempo logs confirm the block size. In both runs, every logged block was 1,024 samples. The probe ran without ReaImGui or SWS.

**Files.** The `-events.txt` files are receiver captures that `lua/furrow_probe_dump.lua` wrote. The `-tempo-block.txt` files are the block tempo logs from the two ramp runs. The sample tempo log is 7 MB, so it stays out of the repository. Its findings are below.

## Results

| Case | Command | Result |
| --- | --- | --- |
| Constant 120 BPM | `python3 probes/diff_log.py probes/expected/constant-120.txt probes/results/reaper-7.82-linux/constant-120-events.txt` | Pass. 38 of 38 events within 1 sample. 37 are exact |
| Ramp 100 to 140 BPM | `python3 probes/diff_log.py probes/expected/ramp-44100-1024.txt probes/results/reaper-7.82-linux/ramp-44100-1024-events.txt` | Fail. 37 of 38 within 1 sample. The last note-off is 2 samples late |

**Routing.** Each receiver logged only its own lane on its own channel. Every event arrived on JSFX bus 0. No receiver logged an extra event.

**Constant case.** The one inexact event is the step 15 note-on. Its exact time is 82,687.5 samples, which the tie rule rounds to 82,688. The engine sent it at 82,687.

## Host behavior the spec left open

**Tempo ramps are linear in time.** A 100 BPM marker with gradual transition at 1.1.00, followed by 140 BPM at 2.1.00, logged 100 + 20 BPM per second at every block start, within 5e-11. It reached 140 at 2.0 s, which is 4 beats at an average of 120 BPM. `probes/schedule.py` now uses this model.

**`tempo` moves only between blocks.** The receiver read `tempo` in `@sample` for every played sample. Within each of the 332 blocks, the value never changed. It equals the tempo at the block's first sample. The `@block` value is the same number.

**`@sample` runs** on a JSFX with stereo pins on a track with no audio source.

## Why the ramp misses

The engine assumes the block-start tempo holds for the whole block. During a rising ramp it therefore places each event late, by an amount that grows with the event's position in the block.

| Offset in block | Events | Error in samples |
| --- | --- | --- |
| 0 to 255 | 5 | 0, 0, 0, 0, 0 |
| 256 to 511 | 4 | 0, 0, 1, 1 |
| 512 to 767 | 6 | 0, 0, 1, 1, 1, 1 |
| 768 to 1,023 | 3 | 1, 1, 2 |

For the worst event, at offset 836 and about 139 BPM, a constant-tempo assumption predicts 1.14 samples of drift before rounding. The rows group events by unique offset and error, so they total 18 rather than 38.

The spec's success criterion is one sample. The gate did not pass with this engine.

## Second run, extrapolating engine

The engine now takes the previous block's tempo change as the slope for the current block and places each event on the resulting curve. A start, stop or jump resets the slope to zero.

| Case | File | Result |
| --- | --- | --- |
| Constant 120 BPM | `constant-120-extrapolate-events.txt` | Pass. 38 of 38 within 1 sample. Identical to the first constant capture |
| Ramp 100 to 140 BPM | `ramp-44100-1024-extrapolate-events.txt` | Pass. 38 of 38 on the exact expected sample |

The ramp run logged the same tempo map as the first run, 100 + 20 BPM per second within 5e-11, in 369 blocks of 1,024 samples. `tempo` again never changed inside a block.

**What this does not cover.** The pattern puts no events in the block where the ramp ends, because the ramp ends at 2.0 s and the last event is at 1.95 s. At a ramp edge, the previous block's change is the wrong guess for one block. A Python copy of the engine stayed within 1 sample on synthetic ramps that start and end mid-pattern. A REAPER capture of those cases is still open. The rest of the timing matrix is also open. That covers 48 and 96 kHz, 64 and 256-sample blocks, tempo steps, 3/4 time, seeks, loops and offline render.

## Third run, ramp edges and tempo steps

The engine in this run treats a position error below one block's beats as tempo drift, not a seek. It continues the last block's tempo change only if that block advanced exactly as a time-linear ramp would. `probes/make_tempo_project.py` wrote each tempo map into the project.

| Case | Tempo map | Result |
| --- | --- | --- |
| `ramp-edge` | 100 BPM, ramp to 200 between beats 1 and 3, then 200 | Pass. 38 of 38 on the exact expected sample |
| `step-on-beat` | 120 BPM, step to 60 on beat 2 | Pass. 38 of 38 within 1 sample. Four note-offs are 1 sample late |
| `step-off-grid` | 120 BPM, step to 60 at beat 2.46875 | Fail. 36 of 38 within 1 sample. The step 10 note-ons are 689 samples early |

**Host behavior.** REAPER kept every block at 1,024 samples, including the blocks that contain a ramp edge or a tempo step. It does not split a block at a tempo marker. `beat_position` matched the exact tempo map within 5e-13 beats in all three runs. The ramp-edge block tempos matched the time-linear map within 5e-11 BPM.

**The step-off-grid miss.** The step falls at sample 54,436, inside the block that starts at 54,272. The step 10 note-ons follow it in the same block. The engine reads only that block's starting tempo, 120 BPM, so it places them as if the step had not happened. A Python copy of the engine predicted the same 689 samples before the run. No engine that reads one tempo per block can place an event that follows a tempo change inside the same block. The error is at most one block, 23 ms at 1,024 samples and 5.8 ms at 256. Removing it would need one block of lookahead, reported to REAPER as plugin delay, which has not been tested.

## Edit and undo

`lua/furrow_probe_undo.lua` sends a step toggle through the engine's gmem mailbox, waits for the engine's acknowledgement, and then creates a native undo point. The engine saves its steps through `@serialize`. Each test runs stopped and again while playing.

**First run, no change mark.** `undo-v1-results.txt`, 17 of 47 checks failed.
- REAPER created no FX-only undo point after the engine applied a toggle.
- An undo point that also covered a new track undid the track and left the step changed.
- The engine's restore count stayed at 0, so REAPER never called `@serialize` to restore it.
- An undo block left open across deferred callbacks was closed by REAPER as "ReaScript: Run".

**Second run, three change marks.** `undo-v2-results.txt`, 66 checks passed and 8 failed.

| Mark before the undo point | A, toggle | B, rename while pending | C, toggle and new track |
| --- | --- | --- | --- |
| none | Fail, no undo point | Not run | Not run |
| lua, `TrackFX_SetParam` on the hidden State revision slider after the acknowledgement | Pass | Pass | Pass |
| engine, `sliderchange` on that slider from `@block` | Fail, no undo point | Fail, no undo point | Fail, the track was undone and the step was not |

Each result holds both stopped and playing. With the lua mark, one native undo restored the step, the engine counted the restore, redo reapplied it, a pending toggle stayed separate from an unrelated rename, and one undo removed a step change and a new track together.

**What this means for the spec.** Native undo can cover an edit the audio thread applied. It needs three conditions.
- The host waits for the acknowledgement, then changes an engine parameter from Lua, then creates one undo point with `Undo_OnStateChangeEx2`.
- A change made only inside the engine is invisible to REAPER's undo, even with `sliderchange` from `@block`.
- The spec's plan to begin an undo block before the first mutation and end it after the acknowledgement does not work across deferred callbacks.

**Still open.** These parts of the gate have no test yet.
- A user edit made between a gesture's host mutation and its undo point.
- Save and reload of the origin and seed encoding.
- Bypass, project tab switching, a closed window and session recovery.

## Memory scope and size

`lua/furrow_probe_memory.lua` loaded `jsfx/furrow_probe_memory.jsfx` on two tracks, copied one to a third, and opened a second project tab with a fourth. Each instance reported through hidden parameters as well as gmem. `memory-results.txt` has 45 results and no failures.

| Question | Result |
| --- | --- |
| Do instances in one project share named gmem? | Yes. Both saw Lua's value, and Lua saw both reports |
| Does a copied FX share it? | Yes |
| Does an instance in another project tab share it? | Yes, while that tab was active. Named gmem is one store for the whole REAPER process |
| Does a background tab keep processing? | No. Once its tab went to the background, the instance's heartbeat stopped and it missed Lua's next value. Its last gmem report stayed in place |
| Does a copied FX keep the original's persistent id? | Yes, because the id lives in a saved slider. The copy got a new runtime token |
| Which named gmem indices round-trip? | Every index tested, up to 33,554,431, both from Lua and from an instance. That is four times the "about 8 million" in the JSFX reference. Higher indices were not tested |
| How much local memory does `maxmem=8388608` give? | `__memtop()` returned 8,388,608, and a write at 8,388,607 read back |

**What this means for the spec.**
- Every rack in every open project tab shares one named gmem. Slot arbitration has to cover tabs, not only racks within one project.
- A rack in a background tab stops processing. It cannot acknowledge commands, and its last report stays in gmem and looks current. The heartbeat is the only sign. This matches the spec's rule that a stalled heartbeat means processing is unavailable, not that the owner has exited.
- A copied FX carries the original's persistent id. Two live reports with the same persistent id and different runtime tokens identify the copy. The spec's rule to give a copy a new id before it accepts edits needs that check.
- Named gmem fits the protocol with room to spare. Local memory is exactly 8,388,608 values. Pattern storage takes 5,259,264 of them, which leaves 3,129,344 for queues, snapshots and transaction staging.

**Not tested.** REAPER may have a setting that keeps background projects running. Separate REAPER processes were not tested. Neither were removing an instance, project switching while a command is pending, or offline render.

## Save and reload

`lua/furrow_probe_state.lua` stored eight start positions through `@serialize` as four 16-bit words each. It also stored three seeds and one raw double. The first run round-tripped the state through a track chunk and saved the project. The second run checked the values after the project was reopened. `state-results.txt` has no failures in either run.

| Stored | Chunk round trip | Reload from disk |
| --- | --- | --- |
| 0, -0, -16.25, 1/3, 288,000 + 1/3, -(2^30) - 1/7, 2^52 + 2, 2^-30 as four words | All bit-exact. Words equal `string.pack("<d")`, -0 stored as 0 | All bit-exact |
| Seeds 0 and 16,777,215 | Unchanged | Unchanged |
| Seed 16,777,217, above the bound | Became 16,777,216 | Became 16,777,216 |
| Raw double 1/3 through `file_var` | Became 0.3333333432674408, the 32-bit value | Same |

**What this means for the spec.** The four-word origin encoding passes the spec's save and load test, so it can be adopted. A raw `file_var` keeps only 32 bits, as the JSFX reference says, so absolute beat positions need the words. Seeds up to 16,777,215 survive as single values. 16,777,217 does not, which is why the spec bounds the seed.

**Unexplained.** The JSFX compared the reloaded raw value with the original 1/3 using `===`, and reported a match. The JSFX reference says `===` means exactly equal. The two values differ after the eighth significant digit. The verdict above does not depend on `===`, because the Lua script compared every decoded value exactly and checked its words against the IEEE layout. Until `===` is tested on its own, Furrow should not rely on it for exactness.

**Other reference notes.** The JSFX reference gives the default named gmem as about 1,048,576 slots. The memory probe used indices up to 33,554,431 without error. The spec's figure of about 8 million needs correcting.

## Host lifecycle

`lua/furrow_probe_lifecycle.lua` measured five cases. `lifecycle-v2-results.txt` has 35 results and no failures.

**First run.** `lifecycle-v1-results.txt` lost the engine after L3. The cleanup undo in L3 assumed the probe's own undo point was on top. REAPER's own "FX offline toggle" point was on top instead, so the undo took the engine offline again. Every later test then found it stopped. The second run undoes only its own points.

| Case | Result |
| --- | --- |
| L1, user edit with its own undo point while a gesture waits for the engine | When the gesture's track change runs before the command, the user's undo point captures the track. One undo of the gesture restores the step and leaves the track. When the track change runs after the acknowledgement, just before the gesture's point, one undo removes both, and the user's rename stays separate. Both runs agree |
| L2, bypass with a command pending | A bypassed engine does not process or acknowledge. After re-enable, the command applies once. REAPER adds its own "FX bypass toggle" undo point |
| L3, offline and online | The engine stops while offline and resumes online. An edit survives with or without the State revision mark. REAPER adds its own "FX offline toggle" undo point |
| L4, new project tab with a command pending | The engine does not acknowledge while its tab is in the background. On return, the command applies once |
| L5, held note at bypass | No note-off or controller reaches the receiver. The note is released only when the engine is re-enabled and SeqCore releases it |
| L5, held note at offline | No note-off or controller reaches the receiver, and none follows when the engine comes back online. The note stays held |
| L5, held note at stop | A note-off reaches the receiver |

**What this means for the spec.**
- A gesture applies host changes after the engine acknowledges, immediately before its single undo point, in the same callback. Applying them first lets a user edit capture them in its own undo point.
- REAPER records bypass and offline changes as undo points. A user's Ctrl+Z can therefore bypass SeqCore or take it offline. The coordinator detects that through the heartbeat, as the spec requires.
- An offline SeqCore leaves held notes held, and so does a bypassed one until it returns. The host sends no cleanup. Furrow cannot claim stuck-note protection for these transitions. It needs the rack panic action, and SeqCore should release every lane route when it starts. That release has not been tested.
- Taking SeqCore offline keeps its current state even without the revision mark. The undo mark is still needed for undo, as the edit and undo runs showed.

**Not tested.** A coordinator that exits while a command is pending and a new one that recovers. The ReaImGui window does not exist yet.

## RS5K response

`lua/furrow_probe_rs5k.lua` drove RS5K 7.82 with a generated 1 kHz sine. A full-velocity note held 0.3535 RMS on each channel, which is the sample's own level, at 1000.1 Hz.

**First run.** `rs5k-v1-results.txt` left Obey note-offs off for its first section. Each note's voice kept playing for the full 3-second sample, about 1.3 s into the next note, so copies of the same sine overlapped at different phases. Raw CC 7 = 127 read -19.4 dB and velocity 64 read -1.6 dB from that overlap, not from RS5K. Its link, learn, pan and Obey note-offs results match the second run.

**Second run.** `rs5k-v2-results.txt` has 38 results and no failures. It turns Obey note-offs on before every level case. Before each note-on it confirmed no voice was sounding.

| Case | Result |
| --- | --- |
| Note-off, default settings | Level unchanged. RS5K plays the sample to its end |
| Note-off, Obey note-offs on | Silence, -310 dB. Turning Obey note-offs on also let a note-off stop a voice that started before |
| Velocity 64 and 1, default Gain for minimum velocity of +0.0 dB | 0.0 dB and 0.0 dB. RS5K ignores velocity by default |
| Velocity 64 and 1, Gain for minimum velocity at -inf | -6.02 dB and silence. Inferred, amplitude is (velocity - 1) / 126 |
| Raw CC 7 at 0 and 127 | Level unchanged and Volume reads 0.5. RS5K ignores raw CC 7 |
| Raw CC 10 at 0 and 127 | Balance unchanged and Pan reads 0.5. RS5K ignores raw CC 10 |
| Pitch bend at both extremes | +1.99 and -1.98 semitones. The default bend range is 2 |
| CC 7 linked to Volume | CC 0 silences. CC 127 gives +6.02 dB. The parameter follows the CC linearly |
| CC 10 linked to Pan | CC 0 is hard left and CC 127 hard right, about 314 dB apart |
| Links removed after CC 7 = 100 and CC 10 = 64 | Volume stays at 0.7874 and Pan at 0.5039, the last linked values |
| CC 7 and CC 10 through MIDI learn | No effect. Volume and Pan stayed at 0.5 |

**Readbacks.** Every `plink` and `learn` key read back the value written. MIDI learn failed on both runs. The cause is not known.

**Pitch bend precision.** The zero-crossing count over 0.4 s resolves about 0.005 semitones, inferred. The -1.98 reading is 0.02 semitones short of -2. The cause is not known, and it does not affect a lane.

**What this means for the spec.**
- Parameter links from the track's MIDI are the working method for per-lane volume and pan, and for V2 parameter locks. MIDI learn did not respond to the same CCs.
- A link maps CC 0 to 127 onto the whole parameter range. On Volume that is silence to +6 dB, so CC 100 gives +3.9 dB, measured in the first run, not unity. Lane volume must pick the CC from the dB it wants, or set `scale` and `offset`.
- Removing a link leaves the parameter at its last linked value. A lane that drops a link must restore the base value itself.
- Gate length needs Obey note-offs on. Per-step velocity needs Gain for minimum velocity below +0.0 dB. Furrow must set both on every RS5K it creates. At -inf, velocity 1 is silent.

## Item metadata

`lua/furrow_probe_items.lua` ran once in a new project. `items-results.txt` has 40 results and no failures. Every payload read back exact, at its full length, both at once and from disk after the reopen.

| Case | Result |
| --- | --- |
| Item payloads of 1 KiB, 64 KiB, 2 MiB and 8 MiB, and a 64 KiB take payload | Exact. Writing 8 MiB took 0.029 s and reading it 0.009 s |
| 2 MiB item copied by Duplicate, Copy and Paste, state chunk copy, and split | Exact on every copy, including both split halves, before and after the reopen |
| Native undo of a replaced payload | The original 1 KiB payload came back exact |
| Quotes, brackets, tab, pipe, backslash, UTF-8, a newline and chunk syntax | Exact when written and through a state chunk copy. These items were deleted before the save, so they were not tested through a save |
| Save and reopen | The file grew from 2,115 to 30,786,554 bytes. Save took 0.21 s and reopen 0.34 s |

**How REAPER stores the data.** It writes item extension data in an `<EXTI` block and take extension data in an `<EXT` block. The 1 KiB payload was one plain line. Every larger payload was a `<BIN Furrow` block of base64 lines 128 characters long. That is why the 2 MiB item's state chunk, 2,806,817 characters, did not contain the payload as one string. The size where REAPER switches to base64 lies between 1 KiB and 64 KiB and was not measured.

**Size cost.** The payloads total 21,103,616 bytes. Base64 adds a third, about 28.1 MB, and the line breaks and the rest of the project make up the 30.8 MB. Inferred, the file costs about 1.46 bytes per payload byte, and save and reopen cost about 7 and 11 ms per MB on this machine.

**What this means for the spec.**
- Item extension data holds a 2 MiB source snapshot, and an 8 MiB one, through every common copy, split, undo and save and reopen. The storage mechanism the spec names works.
- REAPER base64-encodes large values itself, so Furrow gains nothing by escaping or encoding beyond plain printable text. Whether a NUL byte survives was not tested. Inferred, the string API stops at NUL, so the encoding must avoid it.
- The spec stores the full pattern snapshot on every lane's item. A 64-lane commit at the full size would add about 64 × 2 MiB × 1.46, near 196 MB, to the project and about 2 s to each reopen. Inferred. Storing the snapshot once per commit group, with each item holding a reference and its own fingerprint, would cut that by the lane count.
- A split copies the full snapshot to both halves. The fingerprint must therefore cover item position and length, or a split half will look like an unedited commit.
