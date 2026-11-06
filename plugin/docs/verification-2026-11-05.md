# Linux Debug verification, updated 2026-11-06

## Status

**Linux Debug builds and regression suites passed. Full product and release acceptance is incomplete.**

The updated implementation is committed at `2f39627`, following the WebView CMake correction at `186400b`. Tests and artifact checks below do not establish that all planned features are finished.

The headless test configuration is Linux x86-64, `CMAKE_BUILD_TYPE=Debug`, `FURROW_ENABLE_WEBVIEW=OFF`, `FURROW_SANITIZE=OFF`. A separate WebView configuration was built with `FURROW_ENABLE_WEBVIEW=ON` and GTK/WebKitGTK. Artifacts are unstripped and unsigned. JUCE reports 9.0.4.

## Results

| Gate | Evidence | Result |
| --- | --- | --- |
| Shared code | `FurrowSpike` linked after adding Standalone audio utility/device dependencies | Passed |
| VST3 | `FurrowSpike_VST3` linked, moduleinfo generated, `GetPluginFactory` exported | Passed |
| CLAP | `FurrowSpike_CLAP` linked, `clap_entry` exported | Passed |
| Standalone | WebView executable survived a 20-second launch on `DISPLAY=:0`, then was deliberately terminated by `timeout` (124) | Startup smoke only, not interactive/audio acceptance |
| Dynamic dependencies | `ldd` on all three artifacts reported no missing libraries | Passed |
| Engine/sampler/state suite | Refreshed CTest run: 50 cases, 6,494 assertions | Passed |
| Processor/bridge/loader/MIDI suite | Refreshed CTest run: 28 cases, 3,218 assertions | Passed |
| UI bridge suite | Node behavior regression registered in CTest: 1 test | Passed |
| Missing sample persistence | Focused regression: 16 assertions, legacy and current-format sections | Passed |
| pluginval 1.0.4 on final WebView artifact | Strictness 10, seed `0x487a723`, GUI tests enabled, `SUCCESS`, exit 0 | Passed, including editor lifecycle tests |
| clap-validator 0.4.1 on WebView artifact, full | 44 tests: 28 passed, 1 failed, 1 warning, 14 skipped, exit 1 | **Not passed** |
| clap-validator on WebView artifact, filtered | Excludes exactly `^param-conversions$`: 43 tests, 28 passed, 0 failed, 1 warning, 14 skipped, exit 0 | Partial evidence only |

Both C++ executables were explicitly rebuilt after the final opcode change. Direct runs reported the assertion totals above, and CTest passed all three registered suites. Earlier stale-executable passes are not acceptance evidence.

## Diagnoses and changes

- The apparent recurrence of the kit round-trip failure came from a stale engine-test executable. CTest does not build targets. Rebuilding the target included the prior KitState correction and removed the failure.
- The missing-sample test encoded version 2 and compared its bytes with the processor's version 3 save. The test now checks both legacy migration and current-format round-trip, verifies words, setups, metadata and output version, and compares against current-format bytes. No processor behavior was changed for this failure.
- pluginval's initial 30-second watchdog expired while listing the 16 output buses. Runs with a 240-second watchdog completed at strictness 5 and 10 without excluding bus tests.
- clap-validator crashed in `src/tests/plugin_instance/params.rs:82` with `attempt to divide by zero. This is a bug in the validator` during `param-conversions`. This plugin exposes no automatable parameters. The test was excluded only in a separate diagnostic run, not removed from release requirements.
- The CLAP warning is `state-invalid-random`: the wrapper reports successful loading of random bytes. The processor's state setter returns void and ignores non-FRWS input. This warning remains unresolved.
- Debug CLAP processing also logs `JUCE Assertion failure in clap-juce-wrapper.cpp:1824`, alongside the dependency's `FIXME: Handle bypass and deactivated states`. The affected validator checks report passes, but these diagnostics remain relevant for follow-up.
- The editor bridge now uses the backend command schema for step, lane, sampler, pattern, MIDI, kit, resize and sample actions. The UI behavior regression exercises emitted payloads, native state subscription, preview acceptance, retry, lane selection and focused-input preservation.
- `clearLane` uses a plugin-only operation outside SeqCore's wire opcode range. Opcode 12 remains invalid, preserving all golden trace behavior. The regression verifies that clearing one pattern/lane preserves the lane, other patterns and undo state.
- The WebView-enabled VST3, CLAP and Standalone targets linked from the corrected CMake configuration. The effective compile command contains `FURROW_ENABLE_WEBVIEW=1` and `JUCE_WEB_BROWSER=1` without the old conflicting `FURROW_ENABLE_WEBVIEW=0` definition.

## Exact artifacts

Paths below are relative to `plugin/build-webview/FurrowSpike_artefacts/Debug/`.

| Artifact | SHA-256 |
| --- | --- |
| `VST3/Furrow Spike.vst3/Contents/x86_64-linux/Furrow Spike.so` | `590853a157f92fe5f3cc58641707cc70f2b613bc5910c676bc857e12c3ef86c2` |
| `CLAP/Furrow Spike.clap` | `52ddde76ec70d292f72656190510a081b28189f721d87fc323c1268054a469a9` |
| `Standalone/Furrow Spike` | `2303cffb15961a2d8c53ce1405226e0f3ad875b29bb8189629999fdcbe75812e` |

## Reproduction

For an already configured Debug build, build before testing:

```sh
cmake --build plugin/build --parallel 2 --target FurrowSpike
cmake --build plugin/build --parallel 2 --target FurrowSpike_VST3
cmake --build plugin/build --parallel 2 --target FurrowSpike_CLAP
cmake --build plugin/build --parallel 2 --target FurrowSpike_Standalone
cmake --build plugin/build --parallel 2 --target furrow_engine_tests furrow_plugin_tests
ctest --test-dir plugin/build --output-on-failure

cmake --build plugin/build-webview --parallel 2 --target FurrowSpike_VST3 FurrowSpike_CLAP FurrowSpike_Standalone

# Run on an available X display. No virtual display is required when DISPLAY already works.
"$PLUGINVAL" --strictness-level 10 --timeout-ms 240000 \
  --validate "$PWD/plugin/build-webview/FurrowSpike_artefacts/Debug/VST3/Furrow Spike.vst3"

"$CLAP_VALIDATOR" validate --jobs 1 \
  "$PWD/plugin/build-webview/FurrowSpike_artefacts/Debug/CLAP/Furrow Spike.clap"

# Diagnostic only, not a full acceptance pass:
"$CLAP_VALIDATOR" validate --jobs 1 --exclude '^param-conversions$' \
  "$PWD/plugin/build-webview/FurrowSpike_artefacts/Debug/CLAP/Furrow Spike.clap"
```

On this machine the validator executables are under `$JCODE_SCRATCH_DIR/furrow-validators/pluginval/pluginval` and `$JCODE_SCRATCH_DIR/furrow-validators/clap-validator/clap-validator`. Their reported versions above take precedence over old archive names.

Historical headless logs are in [verification-2026-11-05/](verification-2026-11-05/). Final-hash GUI-inclusive pluginval, full CLAP and Standalone startup logs are in [verification-2026-11-06/](verification-2026-11-06/). The filtered CLAP result above preceded the final UI-only resource rebuild and is diagnostic evidence, not acceptance of the final hash.

## Remaining acceptance gates

1. **Full CLAP run:** resolve the validator's zero-parameter crash and rerun without exclusions. Review the invalid-state warning and dependency assertions.
2. **Steinberg validator:** not run. pluginval explicitly skipped this integration because no validator path was supplied.
3. **WebView/editor:** The earlier missing-`xvfb-run` blocker was incorrect. `DISPLAY=:0` and Xorg are available. GUI-inclusive pluginval now passes Editor, Open editor whilst processing, Editor Automation and Background thread state checks. Node tests cover command payloads with a stub DOM, not real browser interactions. File dialogs, native state delivery, rendered controls and drag/drop still need interactive verification. The installed `orca-ide` automation wrapper cannot start because its configured AppImage is missing.
4. **DAW acceptance:** not run against these artifacts. No REAPER application process was running and no `reaper` executable was found on PATH or the checked standard locations. A bridge folder exists but is not evidence of an active host. Use an isolated scratch project to verify audio, MIDI routing, transport/loop behavior, sample recovery and project reload. Older RS5K/engine captures do not prove these plugin binaries work in a DAW.
5. **Release/platforms:** build and validate optimized Release artifacts. Windows/macOS builds, installers, signing, notarization and other hosts remain unverified here. Debug tests without sanitizers do not establish sanitizer coverage.

Do not label the release complete on the basis of this report.
