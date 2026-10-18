# Fresh REAPER timing evidence

## Tested source and host

The captures use the working tree at `HEAD=e4293986bafdfb4f340ab79184a7bb3b477dfa63`. The tracked diff hash, excluding `Testing-furrow.RPP`, is `e7e16dddc4f379d6753f0af816f9d1cc92cb68b88b262e239a2b1252f892894e`. The source was uncommitted. Each case metadata file records the hashes of the timing receiver, dump script, schedule, comparator and expected schedule.

The host is REAPER 7.82 on Linux x86_64 with PulseAudio. The test matrix used 44.1, 48 and 96 kHz; 64, 256 and 1,024 sample blocks; and 60, 120 and 200 BPM. The original 44.1 kHz and 1,024 sample settings were restored. A read-only bridge check after testing confirmed those settings and a stopped transport. `Testing-furrow.RPP` remains unchanged.

## Results

The 27 short constant-tempo cases passed the current sequence-preserving verifier. Each capture has 38 events. The verifier checks routing identity, event count, event order where the spec defines it, and a one-sample timing limit. `verifier-rerun-2026-10-18.txt` records a fresh comparison against all 27 captures.

The following additional cases passed with the same verifier:

- Linear tempo ramp. The historical two-sample-late note-off did not recur.
- On-beat tempo step.
- 4/4 to 3/4 signature change at a measure boundary.
- Nonzero start.
- Loop wrap.
- Seek during playback.
- Stop and restart from project time zero.
- Offline render.

The off-grid tempo-step case fails the strict one-sample comparator for two note-ons. The expected sample is 55,814 and the observed sample is 55,125, a 689-sample early event. The spec allows an error up to one audio block for an event after an off-grid marker in the same block. The block size was 1,024 samples, so this capture falls inside that stated exception. The strict comparator result remains recorded as a failure in `step-off-grid-44100-1024/comparator.log`; the tolerance was not changed.

Three valid pause-and-resume captures show no receiver-count increase during the paused interval. Their event counts increase after resume, and each capture contains duplicate events at the same scheduled onset for lanes 1 and 2. The exact events and transport traces are in the `pause-resume-project-position-*` directories. The repeatable result is an open transport behavior. The spec says paused transport emits no sequenced notes, which the paused receiver-count check confirms. It does not define whether resuming must suppress events that were scheduled before the pause and appear again after the transport position rewinds. The documented stop-and-restart case passes.

The first two pause captures used global `GetPlayPosition2()` to control a project-specific test. A diagnostic showed that its value differed from `GetPlayPositionEx(project)` by more than 54 seconds with multiple project tabs open. Those two captures are retained as invalid harness attempts and are excluded from the result above. The corrected captures use project-scoped transport and position APIs.

## Remaining timing limits

These captures run for only a few seconds. They do not prove the success criterion of timing accuracy over ten minutes. The first four-lane routing proof observed destination bus 0. The spec's separate test of all four internal MIDI buses with `ext_midi_bus` enabled remains open. The ten-minute, 32-lane CPU test also remains open. The spec defines an average CPU threshold below 1% of one core and asks to report peak block duration, but does not define a peak-duration threshold.

Windows, macOS Apple silicon, minimum supported versions, packaging, clean installation and the other open REAPER host gates remain unverified. See `.audit/v1-completion.tsv` and `.audit/v1-completion-run.md` for the release status and prerequisites.
