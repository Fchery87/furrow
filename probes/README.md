The first proof has two parts. Python checks the schedule. REAPER has to check the sound.

**Checked on this machine.** `python3 probes/check_schedule.py` checks the half-sample tie, the 120 BPM step and half-step gate, the linear ramp integral, the 38 events of the 4 by 16 pattern, and the one-sample diff. All seven tests passed. `probes/expected/constant-120.txt` is the file a capture compares against. `probes/expected/ramp-44100-1024.txt` uses the stated map in `probes/cases/ramp-44100-1024.txt`. That map is not a REAPER capture.

**Not run.** REAPER is not installed here.

1. Copy both files from `jsfx/` into the REAPER Effects directory.
2. Run `lua/furrow_probe_layout.lua` once. A second run finds the named tracks and stops. Remove those tracks before creating the layout again.
3. Set 44.1 kHz, 1,024-sample blocks, and a constant 120 BPM. Play from beat 0 through the sixteen steps.
4. Read gmem `FurrowProbeReceiver`. `gmem[0]` is the event count. Each event is nine numbers starting at index 2,000,000. Sample, order, lane, source bus, destination bus, channel, note, velocity, kind. Kind 1 is a note-on. Kind 0 is a note-off.
5. Write the rows to a text file. Run `python3 probes/diff_log.py probes/expected/constant-120.txt captured.txt`.
6. Block tempo is in `gmem[1]` upward. Sample tempo is in `gmem[100001]` upward. Counts are in `gmem[100000]` and `gmem[1100000]`.

**Known limits.** The engine converts beats with the single `tempo` value it reads in `@block`. That is the constant-tempo case only. A ramp inside a block needs the sample log above, and a new expected file taken from REAPER's tempo map, before the diff means anything. The built-in send API documents `I_SRCCHAN` and `I_MIDIFLAGS`. It does not document a source MIDI bus field, so the layout script sets the destination channel and leaves the bus unproven. The receiver stores `midi_bus` on every event so the capture can show what arrived.
