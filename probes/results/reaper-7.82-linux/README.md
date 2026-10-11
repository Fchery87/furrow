First routing and timing proof on REAPER 7.82, Linux x86_64, captured on 11 October 2026.

**Host.** REAPER v7.82 (4 October 2026) on Linux 7.0.0-38-generic. `reaper.ini` saved 44,100 Hz and a 1,024-sample block with `linux_audio_mode=2`. The block tempo logs confirm the block size. In both runs, every logged block was 1,024 samples. The probe ran without ReaImGui or SWS.

**Files.** `constant-120-events.txt` and `ramp-44100-1024-events.txt` are the receiver captures that `lua/furrow_probe_dump.lua` wrote. `ramp-44100-1024-tempo-block.txt` is the block tempo log from the ramp run. The sample tempo log is 7 MB, so it stays out of the repository. Its findings are below.

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

The spec's success criterion is one sample. The gate does not pass with this engine. Spec line 268 says to revise the supported tempo behavior or the architecture before UI work.
