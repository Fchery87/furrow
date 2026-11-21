# Windows x64 verification, 2026-11-21

## Status

**Windows x64 Release WebView2 builds, CTest suites, `pluginval` strictness 10, and unfiltered `clap-validator` all passed. Interactive smoke checks succeeded in Standalone, REAPER (x64), and Studio One 7 on Windows.**

Toolchain: Visual Studio 2022 BuildTools (MSVC 19.44.35229.0), CMake 3.31.6-msvc6, Ninja, Node.js v22.21.1, JUCE 9.0.4, `clap-juce-extensions` `7adee3a1bd4684d4caa5601100e364abccff4b4d`, `Microsoft.Web.WebView2 1.0.3485.44` (static linking, SHA-256 verified).

## Results

| Gate | Evidence | Result |
| --- | --- | --- |
| Release build (`FURROW_ENABLE_WEBVIEW=ON`) | `Furrow.vst3`, `Furrow.clap`, `Furrow.exe` built with 0 errors and 0 warnings in plugin sources | Passed |
| `furrow_engine_tests` | CTest Release run (engine, SeqCore golden traces, REAPER captures, sampler, state/kit codecs) | Passed (1.78 s) |
| `furrow_ui_bridge_tests` | Node.js UI bridge regression, transport playhead, batching, zero-external-URL check | Passed (0.06 s) |
| `furrow_plugin_tests` | Processor, RT zero-allocation `processBlock`/`processBlockBypassed`, bridge, MIDI, loader, host parameters | Passed (2.51 s) |
| `clap-validator 0.4.1` (unfiltered) | `clap-validator.exe validate Furrow.clap` with no `--exclude` flags; `param-conversions`, `param-default-values`, and `state-reproducibility-*` all passed; 1 known JUCE wrapper `state-invalid-random` warning | Passed (exit 0) |
| `pluginval 1.0.4` (VST3, strictness 10) | `pluginval.exe --strictness-level 10 --timeout-ms 240000 --validate Furrow.vst3` with WebView2 backend enabled (`Editor`, `Open editor whilst processing`, `Editor Automation`, `Background thread state`, `Parameter thread safety`, 16 output buses, `Fuzz parameters`) | Passed (`SUCCESS`, exit 0) |
| Packaging | `python plugin/tools/package.py --build plugin/build-win --output plugin/dist --platform windows` produced `furrow-windows.zip` with VST3, CLAP, Standalone, `LICENSE`, `README.md`, `docs/`, and SHA-256 `manifest.json` | Passed |
| Windows Standalone (`Furrow.exe`) | Verified WebView2 UI loads cleanly via `Backend::webview2` and writable `%APPDATA%\Furrow\WebView2` user data folder | Passed |
| REAPER (x64, Windows) | User-verified interactive load and playback of `Furrow.vst3` | Passed (smoke) |
| Studio One 7 (Windows) | User-verified interactive load and playback of `Furrow.vst3` | Passed (smoke) |

## Exact artifacts

Paths are relative to `plugin/build-win/Furrow_artefacts/Release/` (and `plugin/dist/` for the package archive):

| Artifact | SHA-256 |
| --- | --- |
| `VST3/Furrow.vst3/Contents/x86_64-win/Furrow.vst3` | `c26760deb4ffe427a713e78d25d462de71c2112e88ff59c43e562e54cb852fa6` |
| `CLAP/Furrow.clap` | `70f51d8d60b9d88d491f73b456d40f9866f8801e64918a4351d719e454effb66` |
| `Standalone/Furrow.exe` | `0e286583ad36ba1e4c2a746c1e07f7d421131b243f641d3e33432fe8dba3738d` |
| `plugin/dist/furrow-windows.zip` | `70adc9615ad0aafa8ccb2a1175f51872d2409ab86d5fe43942b34f610d5ce00e` |

## Remaining external gates

- macOS universal builds and DAW checks (REAPER, Studio One, FL Studio).
- Windows FL Studio host verification and deep per-host MIDI-out / multi-out / drag-out capture matrices.
- Linux WebKitGTK rerun confirming the deferred `goToURL` lifecycle fix clears prior `pluginval` WebKit criticals, plus Linux ThreadSanitizer CI run.
- Authenticode Windows signing and Apple Developer ID signing and notarization.
