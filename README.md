# Furrow

Furrow is a MIDI step sequencer for REAPER. A rack contains sample lanes or instrument lanes, patterns, step-level controls, and a Live/Items playback mode. Committed MIDI items retain source metadata so an unchanged commit can restore its pattern, while edited or unrelated MIDI can be previewed for conversion.

## Requirements

- REAPER with the ReaImGui extension. The entry action requests ReaImGui API 0.9 and reports missing or incompatible installations.
- No compiler is required to run Furrow. The JSFX and Lua sources are installed as files.
- The automated harness needs Python with `lupa`; some engine and gate cases also need the local JSFX harness library built with CMake and a C++17 compiler.

REAPER 7.82 on Linux is the current host-tested baseline. Minimum supported REAPER and ReaImGui versions have not been established against the complete routing and timing matrix. Windows and macOS are not yet host-validated, so this repository is not yet a V1 release.

## Development installation

Until the ReaPack index and clean-install checks are complete, install from a local checkout:

1. Copy `jsfx/furrow_seqcore.jsfx` and `jsfx/furrow_itemgate.jsfx` into REAPER's Effects directory, preserving the filenames. Rescan the JSFX directory or restart REAPER.
2. Copy `lua/Furrow_Rack.lua` into REAPER's Scripts directory. Copy the `lua/furrow/` directory alongside it so the path is `Scripts/furrow/`.
3. Install ReaImGui API 0.9 or newer, then restart REAPER.
4. In Actions, choose **New action > Load ReaScript**, select `Furrow_Rack.lua`, and load it.
5. Run the newly loaded `Furrow_Rack.lua` entry from the action list. Create a rack in the window or select an existing rack to open it.

The script declares `Furrow: Open rack` as its action description. A manually registered copy can appear under a custom file name; the packaged action name has not yet been verified in a clean REAPER profile.

The other ReaScripts in `lua/` are diagnostic or stress tools. They are not required for normal use. Probe JSFX and scripts are under `jsfx/` and `lua/` with `probe` in their names.

## Tests

Build the JSFX harness library once, then run the documented suites:

```sh
tools/jsfx_host/build.sh
cd tests
python -m unittest test_seqcore test_rack test_commit test_capture test_gate test_bridge
```

The Python environment must have `lupa` installed. The harness exercises the real Lua modules and JSFX under ysfx or a stub REAPER API. It does not replace host validation in REAPER. See [tests/README.md](tests/README.md) and [probes/README.md](probes/README.md) for full prerequisites and procedures.

## Release status

The ReaPack repository index, clean-profile installation test, complete timing and CPU evidence, coordinator recovery test, third-party instrument validation, and Windows/macOS host runs remain open. See [the V1 implementation map](Furrow-Spec-and-Architecture.md#current-v1-implementation-map-18-october-2026) and [.audit/v1-completion.tsv](.audit/v1-completion.tsv) for current evidence.

Furrow is licensed under the [MIT License](LICENSE).
