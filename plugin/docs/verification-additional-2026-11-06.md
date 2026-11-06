# Additional Linux verification, 2026-11-06

Implementation under test: `2f39627`. Documentation baseline: `799bcb8`.

## Repeated Debug checks

- `ctest --test-dir plugin/build --output-on-failure --repeat until-fail:3 -j1` passed all three registered suites three times, nine executions in total, in 123.46 seconds. See `verification-2026-11-06/debug-ctest-repeat.log`.
- The three Debug WebView SHA-256 hashes still match the main verification report. No implementation was changed during this pass.
- GUI-inclusive pluginval strictness 10 passed again, exit 0, seed `0x4231fa4`, in 74.4 seconds. See `verification-2026-11-06/pluginval-gui-repeat.log`. However, the subprocess emitted three WebKit critical diagnostics: `webkit_web_view_load_request: assertion 'WEBKIT_IS_WEB_VIEW(webView)' failed`. The earlier GUI run also contains four such diagnostics. Validator success does not establish clean WebView rendering or initialization. Individual rendered controls and file dialogs remain unverified.
- Unfiltered clap-validator reproduced 44 tests, 28 passed, 1 failed, 1 warning, 14 skipped, exit 1. See `verification-2026-11-06/clap-repeat.log`. The CLAP binary exports `clap_entry`.

## Source-backed limits

The pinned CLAP wrapper's `stateLoad` in `src/wrapper/clap-juce-wrapper.cpp` returns true after passing any nonempty successfully read stream to JUCE's void `setStateInformation`. This explains the invalid-random-state warning, rather than proving the processor accepted corrupt state. Processor tests cover unrelated bytes preserving valid state and unreadable Furrow containers silencing playback. The wrapper also contains the bypass/deactivated-state FIXME directly before `processBlock`; processor bypass tests do not prove that this wrapper calls the bypass path correctly.

The current test inventory exercises MIDI note release on bypass and state replacement, edit queue saturation, sample retirement off the audio thread, latest-request sample loading, kit undo, nonmutating MIDI preview, and host-save round trips. Repetition improves confidence in these paths but is not a ThreadSanitizer or race-freedom result. A configured `plugin/build-tsan` directory exists, but no executable or successful run was established in this pass.

## Release continuation

The first two-job Release build hit the 600-second cap. This is not an environment blocker: completed object files are reusable. A second bounded invocation resumed the existing Unix Makefiles tree with WebView enabled, `-O3`, and LTO disabled. It linked the VST3 target successfully before timing out during CLAP compilation. A third invocation linked CLAP and Standalone, then continued compiling tests. Logs are `verification-2026-11-06/release-resume.log` and `verification-2026-11-06/release-resume-2.log`.

The optimized VST3 passed GUI-inclusive pluginval strictness 10, exit 0, seed `0x8ab7a`, in 27.58 seconds, with three of the same WebKit critical messages. See `verification-2026-11-06/pluginval-release.log`. Full optimized CLAP validation reproduced 28 passed, 1 failed, 1 warning and 14 skipped out of 44, exit 1. See `verification-2026-11-06/clap-release.log`. Optimized Standalone remained alive for 20 seconds before intentional timeout, exit 124, with no other logged output. This is startup smoke only, not audio or rendered-control acceptance.

Direct window-scoped X11 verification went further than startup smoke. The Debug Standalone editor rendered at 728x454. Clear steps removed Kick hits, a grid click restored one, Add lane created Lane 5, and Save kit opened the native GTK file chooser. Later, native sample selection displayed the scratch WAV path, and native kit save/mutate/load restored the lane name and sample reference. Screenshots and scope are documented in `native-editor-verification-2026-11-06.md`. The GDB run followed the wrong descendant for the WebKit critical, so absence of a stack does not diagnose its severity. Drag/drop, audio/host routing and persistence after process close remain unverified.
Direct window-scoped X11 verification went further than startup smoke. The Debug Standalone editor rendered at 728x454. Clear steps removed Kick hits, a grid click restored one, Add lane created Lane 5, and Save kit opened the native GTK file chooser. Later, native sample selection displayed the scratch WAV path, native kit save/mutate/load restored the lane name and sample reference, and MIDI export wrote a valid 352-byte `MThd` file. Screenshots and scope are documented in `native-editor-verification-2026-11-06.md`. The GDB run followed the wrong descendant for the WebKit critical, so absence of a stack does not diagnose its severity. Drag/drop, exact native MIDI preview acceptance, audio/host routing and persistence after process close remain unverified.

Paths below are relative to `plugin/build-release/FurrowSpike_artefacts/Release/`:

| Artifact | SHA-256 |
| --- | --- |
| `VST3/Furrow Spike.vst3/Contents/x86_64-linux/Furrow Spike.so` | `5d38a1cac7d607781ed86829bd3f704989bebea0c47ff71b4143c235ab8c42aa` |
| `CLAP/Furrow Spike.clap` | `eb08cb63d7af9771c25b044915c0b88be5907dddab7e39d66bbab854f267b20e` |
| `Standalone/Furrow Spike` | `a4f341ab1e97cc9bb308700e00195b3b7cb6bff45c47db3a96c9cd0b67f5e5e8` |

## Repository and remaining gates

The third Release build invocation finished successfully, exit 0, in 415.59 seconds. Fresh Release CTest then passed all three suites in 5.20 seconds: engine 50 cases/6,494 assertions, plugin 28 cases/3,218 assertions, and UI bridge. See `verification-2026-11-06/release-ctest.log`. All three optimized artifact hashes remained unchanged after the test build, and `ldd` reported no missing dependencies. These builds are unsigned Linux x86-64 artifacts, not packaged releases.

The tracked tree was clean at `799bcb8` before these logs were added. `5677d73` is an ancestor. The cached `origin/feat/plugin` reference was six commits behind, not a freshly fetched remote-state claim. No push was performed.

Full CLAP, clean WebKit runtime, interactive native controls/dialogs/drag-drop, DAW acceptance, Steinberg validation, sanitizers, signing and non-Linux platform gates remain open. Tasks requiring these outcomes must not be marked complete merely because the limitation has been documented.
