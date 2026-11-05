# Linux Debug verification, 2026-11-05

## Status

**Integrated build and tests verified. Release acceptance is incomplete.**

This report applies to the local working tree, which includes pre-existing uncommitted implementation work. It is not evidence that the committed baseline alone builds or that all planned features are finished.

Configuration: Linux x86-64, `CMAKE_BUILD_TYPE=Debug`, `FURROW_ENABLE_WEBVIEW=OFF`, `FURROW_SANITIZE=OFF`. Artifacts are unstripped and unsigned. JUCE reports 9.0.4.

## Results

| Gate | Evidence | Result |
| --- | --- | --- |
| Shared code | `FurrowSpike` linked after adding Standalone audio utility/device dependencies | Passed |
| VST3 | `FurrowSpike_VST3` linked, moduleinfo generated, `GetPluginFactory` exported | Passed |
| CLAP | `FurrowSpike_CLAP` linked, `clap_entry` exported | Passed |
| Standalone | `FurrowSpike_Standalone` linked | Build only, not interactive/audio acceptance |
| Dynamic dependencies | `ldd` on all three artifacts reported no missing libraries | Passed |
| Engine/sampler/state suite | Fresh CTest run: 50 cases, 6,494 assertions | Passed |
| Processor/bridge/loader/MIDI suite | Fresh CTest run: 21 cases, 2,986 assertions | Passed |
| Missing sample persistence | Focused regression: 16 assertions, legacy and current-format sections | Passed |
| pluginval 1.0.4 | Strictness 10, seed `0x4575ff`, 240,000 ms timeout, GUI skipped, `SUCCESS`, exit 0 | Passed within stated scope |
| clap-validator 0.4.1, full | 44 tests: 28 passed, 1 failed, 1 warning, 14 skipped, exit 1 | **Not passed** |
| clap-validator, filtered | Excludes exactly `^param-conversions$`: 43 tests, 28 passed, 0 failed, 1 warning, 14 skipped, exit 0 | Partial evidence only |

Assertion totals above come from the final `plugin/build/Testing/Temporary/LastTest.log`, not an earlier binary or configuration. CTest passed both registered suites. Earlier runs reported different assertion totals and are not substituted for this run.

## Diagnoses and changes

- The apparent recurrence of the kit round-trip failure came from a stale engine-test executable. CTest does not build targets. Rebuilding the target included the prior KitState correction and removed the failure.
- The missing-sample test encoded version 2 and compared its bytes with the processor's version 3 save. The test now checks both legacy migration and current-format round-trip, verifies words, setups, metadata and output version, and compares against current-format bytes. No processor behavior was changed for this failure.
- pluginval's initial 30-second watchdog expired while listing the 16 output buses. Runs with a 240-second watchdog completed at strictness 5 and 10 without excluding bus tests.
- clap-validator crashed in `src/tests/plugin_instance/params.rs:82` with `attempt to divide by zero. This is a bug in the validator` during `param-conversions`. This plugin exposes no automatable parameters. The test was excluded only in a separate diagnostic run, not removed from release requirements.
- The CLAP warning is `state-invalid-random`: the wrapper reports successful loading of random bytes. The processor's state setter returns void and ignores non-FRWS input. This warning remains unresolved.
- Debug CLAP processing also logs `JUCE Assertion failure in clap-juce-wrapper.cpp:1824`, alongside the dependency's `FIXME: Handle bypass and deactivated states`. The affected validator checks report passes, but these diagnostics remain relevant for follow-up.

## Exact artifacts

Paths below are relative to `plugin/build/FurrowSpike_artefacts/Debug/`.

| Artifact | SHA-256 |
| --- | --- |
| `VST3/Furrow Spike.vst3/Contents/x86_64-linux/Furrow Spike.so` | `e918b0e96cf80b98ebd9e0afcdf243318d2df4106031f4ad5ad623b816b60f4e` |
| `CLAP/Furrow Spike.clap` | `06cd160f89ea329e222098eae9a5aaa62c4c0c96c6f9dcb6591e2bd9856779d1` |
| `Standalone/Furrow Spike` | `2f4fb8dff40da5442b4dd19ee86416ffcd66c611fce7fc537f0ec504612dbee1` |

## Reproduction

For an already configured Debug build, build before testing:

```sh
cmake --build plugin/build --parallel 2 --target FurrowSpike
cmake --build plugin/build --parallel 2 --target FurrowSpike_VST3
cmake --build plugin/build --parallel 2 --target FurrowSpike_CLAP
cmake --build plugin/build --parallel 2 --target FurrowSpike_Standalone
cmake --build plugin/build --parallel 2 --target furrow_engine_tests furrow_plugin_tests
ctest --test-dir plugin/build --output-on-failure

"$PLUGINVAL" --strictness-level 10 --timeout-ms 240000 --skip-gui-tests \
  --validate "$PWD/plugin/build/FurrowSpike_artefacts/Debug/VST3/Furrow Spike.vst3"

"$CLAP_VALIDATOR" validate --jobs 1 \
  "$PWD/plugin/build/FurrowSpike_artefacts/Debug/CLAP/Furrow Spike.clap"

# Diagnostic only, not a full acceptance pass:
"$CLAP_VALIDATOR" validate --jobs 1 --exclude '^param-conversions$' \
  "$PWD/plugin/build/FurrowSpike_artefacts/Debug/CLAP/Furrow Spike.clap"
```

On this machine the validator executables are under `$JCODE_SCRATCH_DIR/furrow-validators/pluginval/pluginval` and `$JCODE_SCRATCH_DIR/furrow-validators/clap-validator/clap-validator`. Their reported versions above take precedence over old archive names.

Preserved logs are in [verification-2026-11-05/](verification-2026-11-05/).

## Remaining acceptance gates

1. **Full CLAP run:** resolve the validator's zero-parameter crash and rerun without exclusions. Review the invalid-state warning and dependency assertions.
2. **Steinberg validator:** not run. pluginval explicitly skipped this integration because no validator path was supplied.
3. **WebView/editor:** not built or exercised in this run. GTK/WebKitGTK packages were detected previously, so missing packages are not an established blocker. The CMake ON branch currently adds `FURROW_ENABLE_WEBVIEW=1` alongside the unconditional `=0`; resolve that conflicting definition before treating an ON build as meaningful. Verify editor launch, bridge actions, resizing, state restoration and MIDI drag/drop.
4. **DAW acceptance:** not run against these artifacts. No REAPER application process was running and no `reaper` executable was found on PATH or the checked standard locations. A bridge folder exists but is not evidence of an active host. Use an isolated scratch project to verify audio, MIDI routing, transport/loop behavior, sample recovery and project reload. Older RS5K/engine captures do not prove these plugin binaries work in a DAW.
5. **Release/platforms:** build and validate optimized Release artifacts. Windows/macOS builds, installers, signing, notarization and other hosts remain unverified here. Debug tests without sanitizers do not establish sanitizer coverage.

Do not label the release complete on the basis of this report.
