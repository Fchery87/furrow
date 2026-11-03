# Furrow V1 completion implementation plan

**Goal:** Complete the specified plugin workflows in verifiable increments, retaining the existing sampler development work and development-state compatibility.

**Architecture:** Keep the pure C++ sequencing engine and sampler. Publish prepared rack objects from a serialized non-audio model to the audio thread, retire replaced objects off-thread, and embed a local-only WebView UI with a validated command boundary. Implement file formats as separately testable services. Record release acceptance separately from compilation and unit tests.

**Stack:** C++20, JUCE 9.0.4, CLAP JUCE wrapper, CMake/Ninja, Catch2, embedded HTML/CSS/JavaScript.

## Ownership and verification sequence

1. Processor safety and state recovery (`src/SpikeProcessor.*`, `tests/plugin/ProcessorTests.cpp`). Reproduce old-channel release and bypass bugs, preserve unreadable FRWS bytes, test malformed-state recovery and repeated loads. Build and run all processor tests before committing.
2. Model/audio handover (`src/SpikeProcessor.*`, `tests/plugin/BridgeTests.cpp`). Add bounded handover, no callback serialization locks, prepared state allocation and retirement off-thread, visible edit errors, editor snapshots, panic and undo. Verify edits survive host save and playback receives the edits. Verify empty blocks, queue saturation, host reset and malformed commands.
3. Metadata and kit codecs (`sampler/include/furrow/{PluginState,KitState}.h`, `tests/MetadataKitTests.cpp`). Keep v1/v2 reads, version metadata for lane/pattern names/colors and window size. Reject malformed input atomically. Verify kits omit patterns.
4. MIDI file service (`src/PatternMidi.*`, `tests/plugin/PatternMidiTests.cpp`). Export type-1 MIDI from origin through the actual engine. Parse import into a preview, reject unrepresentable inputs without mutation. Verify channels, offsets, probability and refusal diagnostics.
5. Production editor (`src/PluginEditor.*`, `ui/app/index.html`). Follow the approved prototype's rack/inspector visual language. Embed resources without external dependencies, disable unrelated navigation, wire native drop and async file dialogs. Validate JavaScript and exercise real bridge controls, not simulated browser audio.
6. Integrate metadata, history, kits, MIDI import preview/accept and missing-sample retry in processor/edit boundary. Add focused integration tests for host save/reopen and file workflows.
7. Build integration (`CMakeLists.txt`, `.github/workflows/plugin.yml`). Enable native WebView packages and standalone. Include all new tests. Add packaging with source/license/docs and platform prerequisites without pretending signing credentials exist.
8. Run engine tests, golden tests, sanitizer tests and processor tests. Run available format validators and actual REAPER checks if installed and safely automatable. Record exact evidence and unresolved external platform/signing gates in docs.

## Acceptance gates

- Backend: old routes release exactly once on state replacement and bypass, unreadable state remains byte-identical and silent until recovery, queue errors visible, no audio-thread allocation/free or wait introduced.
- Usable alpha: plugin window renders, edit steps, add a sample, select patterns, and close/reopen with settings preserved.
- Product: undo/redo, kit save/load without overwriting patterns, MIDI export and previewed import, missing-sample retry, names/colors, resizable accessible controls.
- Release: validators and host/platform checks recorded for the exact artifact. Windows/macOS checks, signing and notarization require actual runners/credentials. Missing evidence stays pending, never inferred from Linux tests.

## Working-tree policy

The initial tree already includes uncommitted sampler, loader, tests and probe changes. Preserve them. Do not reset, clean, stage unrelated paths or push automatically. Commit integrated verified plugin work with explicit descriptions of inherited baseline where it shares files. Build locally with at most two parallel compiler jobs on this machine. Workers own disjoint files, coordinator reviews and runs integration tests.
