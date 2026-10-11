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
