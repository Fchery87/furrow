# Furrow

Furrow is a MIDI step sequencer that runs inside REAPER. Its Lua and ReaImGui rack edits patterns, and the SeqCore JSFX plays them. You can use audio samples through REAPER's ReaSamplOmatic5000 (RS5K), route a lane to an instrument track, or commit a pattern as editable MIDI items.

## Project status

The core V1 workflows described below are implemented in the repository, and automated tests cover the Lua and JSFX logic. REAPER 7.82 on Linux is the current host-tested baseline, not a certified minimum version. Some host checks passed on that setup, but the full release gates have not passed. Furrow is not yet ready for a V1 release.

The documented V1 scope includes sample and instrument lanes, step editing, patterns, probability, MIDI commit and capture, and Live or Items playback. Conditional trigs, per-lane rate and direction, parameter locks, generators, and song mode are later-version features.

## Implemented capabilities

- Create a rack and add sample lanes. Furrow creates a REAPER track and RS5K instance for each sample.
- Route an instrument lane to a selected REAPER track and MIDI channel. Furrow saves the target and rejects conflicts, the engine track, managed-item tracks, and routing cycles.
- Edit patterns with up to 64 lanes, 512 steps per lane, and independent lane lengths. The grid supports step painting, velocity and other step lanes, mute, solo, swing, probability, and lane tools.
- Switch between **Live** playback from SeqCore and **Items** playback from committed MIDI items.
- Commit the displayed pattern at the edit cursor. Furrow creates MIDI items, stores source metadata for the item group, switches to Items mode, and records the operation as one REAPER undo point.
- Capture one selected MIDI item. Furrow can restore an eligible source snapshot to a new pattern or preview conversion of the current MIDI. You can map MIDI channels to existing lanes or new lanes and apply the choice in one undoable action.
- Use **Panic** to send note releases and All Notes Off to every lane while SeqCore is processing.

The automated suites exercise these paths with ysfx and a stub REAPER API. REAPER host checks have verified selected sample-lane, routing, commit, capture, undo, gate, and timing cases on REAPER 7.82 for Linux. They do not establish behavior on every supported system or instrument. See [release status](#release-status) and the [V1 implementation map](Furrow-Spec-and-Architecture.md#current-v1-implementation-map-18-october-2026).

## Requirements

### Runtime

- REAPER. The project has been host-tested on REAPER 7.82 for Linux. The minimum supported REAPER version is not established.
- ReaImGui with API version 0.9. `lua/Furrow_Rack.lua` requests API `0.9` and displays an error if the API is missing or incompatible. The minimum supported ReaImGui release has not been established by a version matrix.
- Furrow's JSFX and Lua files. No compiler is needed to run the plugin.

RS5K is used for sample lanes. SWS is not required. The `js_ReaScriptAPI` extension is optional; it enables multi-file browsing, while REAPER's built-in file dialog can add one file at a time.

### Development and test

- Python with `lupa` for the documented unittest suites and Lua-host probe.
- Git, CMake, a C++17 compiler, and network access to build the pinned ysfx test host with `tools/jsfx_host/build.sh`. The script builds `tools/jsfx_host/libfurrow_jsfx_host.so`; this build script is for the Linux test setup, not plugin installation.

The repository does not pin a Python or Lupa version in a dependency manifest. The test suites do not need a compiler after the JSFX host library has been built. See [test prerequisites and coverage](tests/README.md).

## Development installation

There is no ReaPack index or clean-profile installation package yet. Install from a checkout for development.

1. Install REAPER and ReaImGui. In REAPER, install or update ReaImGui so the script can load API `0.9`, then restart REAPER. If ReaPack is installed, find ReaImGui under **Extensions > ReaPack > Browse packages**.
2. Find REAPER's resource directory. In REAPER, choose **Options > Show REAPER resource path in explorer/finder**. In a Linux or macOS shell, set `REAPER_RESOURCE` to that directory, replacing the example path:

   ```sh
   export REAPER_RESOURCE="/path/to/REAPER/resource"
   ```

   On Windows, copy the same files and directory structure with File Explorer. Windows installation has not been host-verified.
3. From the repository root, copy the runtime files while preserving the directory structure:

   ```sh
   mkdir -p "$REAPER_RESOURCE/Effects" "$REAPER_RESOURCE/Scripts/furrow"
   cp jsfx/furrow_seqcore.jsfx jsfx/furrow_itemgate.jsfx "$REAPER_RESOURCE/Effects/"
   cp lua/Furrow_Rack.lua "$REAPER_RESOURCE/Scripts/"
   cp lua/furrow/*.lua "$REAPER_RESOURCE/Scripts/furrow/"
   ```

4. Rescan REAPER's JSFX Effects directory or restart REAPER.
5. Open **Actions > Show action list**, choose **New action > Load ReaScript**, and select `Scripts/Furrow_Rack.lua` in the resource directory.
6. Run the loaded script. The source declares the action description **Furrow: Open rack**. In the checked REAPER setup, the manually loaded script appeared as **Custom: Furrow_Rack.lua**. The action label from a future package has not been verified.

The development install path has not yet been tested from a clean REAPER profile. ReaPack packaging, including its index and clean installation, remains open.

## Basic use

1. Run the loaded Furrow action. In an empty project, choose **New rack**.
2. Add samples with **Drop audio files here, or click to browse**, or drag audio files onto the rack. Furrow creates one sample lane per file.
3. Click or drag across grid cells to paint steps. Select a lane name to edit its step lanes and settings. Double-click or Alt-click the name, or use **Hear sample** in its context menu, to audition it.
4. Use the pattern picker and its **...** menu to create, clone, clear, or rename patterns. Use the **Groove** controls for global swing and probability seed. Right-click a lane for fill, rotate, invert, clear, and delete tools.
5. Choose **Live** to play SeqCore's pattern. Choose **Items** to play committed MIDI items. **Commit** renders the displayed pattern at the edit cursor and switches to Items mode. Use REAPER's undo and redo for edits and commits.
6. To restore or convert MIDI, select exactly one MIDI item and choose **Capture MIDI item**. Map each source MIDI channel to an existing lane or choose a target track and channel for a new lane. Review the preview, then choose **Restore source to new pattern**, **Apply source settings and restore**, or **Convert current MIDI to new pattern**, as offered.
7. Use **Panic** to release notes when SeqCore is processing. If the rack reports that SeqCore is not processing, bring its project tab to the front and restore the JSFX to an active, online state before using Panic. Panic cannot send while SeqCore is bypassed, offline, or in a background project tab.

Instrument controllers are MIDI channel-wide. Reserve one target track and channel for each Furrow lane, and avoid sending unrelated MIDI on that channel if it needs independent controller or note-off behavior. See the [instrument and controller contract](Furrow-Spec-and-Architecture.md#instrument-and-controller-contract).

## Development and verification

Run commands from the repository root unless the command changes into `tests/`.

1. On Linux, create and activate a Python virtual environment, then install `lupa`:

   ```sh
   python3 -m venv /tmp/furrow-test-env
   . /tmp/furrow-test-env/bin/activate
   python -m pip install lupa
   ```

2. Build the JSFX test host. This step needs Git, CMake, a C++17 compiler, and network access:

   ```sh
   tools/jsfx_host/build.sh
   ```

3. Run the full documented suite:

   ```sh
   cd tests
   python -m unittest test_seqcore test_rack test_commit test_capture test_gate test_bridge
   cd ..
   ```

4. Run the standalone probes from the repository root:

   ```sh
   python3 probes/check_schedule.py
   python3 probes/check_engine_bits.py
   python3 probes/check_lua_scripts.py
   ```

The suite exercises SeqCore under ysfx and REAPER-facing Lua against stubs. It does not replace host testing. The final committed snapshot `f5a504da` has a reported full-suite result of 171 tests with exit status 0, plus passing scheduling, engine-bit, Lua-host, Python compilation, and whitespace checks. This is evidence for that snapshot, not a fresh test run or a permanent test count. See [test coverage and host limitations](tests/README.md) and [probe procedures](probes/README.md).

## Release status

V1 is not release-ready. Confirmed implementation and host evidence do not cover every required release gate.

- **Timing.** Short captures on REAPER 7.82/Linux passed the 27-case constant-tempo grid and several timing variants. The captures use an uncommitted `e429398` working tree, not committed snapshot `f5a504da`. The off-grid tempo-step capture places two events 689 samples early. That is inside the specification's one-block exception at 1,024 samples, but the strict one-sample comparator records a failure. Pause-and-resume produced duplicate events and remains unresolved. Four-bus routing and the ten-minute timing run remain open. See the [timing evidence](.audit/results/2026-10-18/fresh-timing/README.md).
- **Performance.** The required 32-lane, ten-minute CPU workload at 48 kHz and 256-sample blocks has not been verified. The specification sets an average target below 1% of one core, but no peak block-time threshold.
- **REAPER host checks.** REAPER 7.82/Linux ReaSynth checks covered Panic after routing repair and bypass recovery. The complete repeatable item-gate matrix, Panic UI after offline recovery, edited-source capture choices, visible lane colors, coordinator recovery, and third-party instrument routing still need host evidence. See the [implementation map](Furrow-Spec-and-Architecture.md#current-v1-implementation-map-18-october-2026) for the cases that remain open.
- **Installation and compatibility.** The repository has no `index.xml`, ReaPack package metadata, or verified clean-profile installation. REAPER and ReaImGui minimum versions are not established. The source action description and observed manual action label differ as described above.
- **Platforms.** Windows and macOS Apple silicon host checks remain unverified. Do not infer support from the Linux results.

The audit records contain detailed run notes and raw captures. `.audit/results/` currently uses about 164 MB, mostly timing and host evidence files, so a checkout includes a substantial audit-data footprint. See the [V1 audit summary](.audit/v1-completion-run.md) and [audit table](.audit/v1-completion.tsv).

Furrow is licensed under the [MIT License](LICENSE).
