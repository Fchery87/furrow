# Windows x64 verification, 2026-11-21

## Status

**Furrow 1.2.0 Windows x64 Release WebView2 builds, CTest suites, `pluginval` strictness 10, and unfiltered `clap-validator` all passed. Prior 1.0.0 interactive smoke checks succeeded in Standalone, REAPER (x64), and Studio One 7 on Windows; interactive DAW re-checks of the new 1.2.0 features remain user-verified.**

Toolchain: Visual Studio 2022 BuildTools (MSVC 19.44.35229.0), CMake 3.31.6-msvc6, Ninja, Node.js v22.21.1, JUCE 9.0.4, `clap-juce-extensions` `7adee3a1bd4684d4caa5601100e364abccff4b4d`, `Microsoft.Web.WebView2 1.0.3485.44` (static linking, SHA-256 verified).

## Results (v1.2.0)

| Gate | Evidence | Result |
| --- | --- | --- |
| Release build (`FURROW_ENABLE_WEBVIEW=ON`) | `Furrow.vst3`, `Furrow.clap`, `Furrow.exe` built with 0 errors and 0 warnings in plugin sources | Passed |
| `furrow_engine_tests` | Engine, SeqCore golden traces, REAPER captures, V1.1 sampler (One-Shot, Reverse, Cytomic SVF LP/HP, peak meters, preview voice, 0 allocations), FRWS v4 state/kit codecs | Passed |
| `furrow_ui_bridge_tests` | Node.js UI bridge regressions (V1.1 meters/standalone/filter/reverse/oneshot/duplicate/drag, V1.2 sample browser/auto-swap/WAV drag, zero external URLs) | Passed |
| `furrow_plugin_tests` | Processor, RT 0-allocation `processBlock`/`processBlockBypassed`, incoming MIDI pads + Program Change, bypass state sync, V1.1/V1.2 bridge, OGG/MP3 loader formats, offline 24-bit WAV export (`PatternAudioTests`) | Passed |
| `clap-validator 0.4.1` (unfiltered) | `clap-validator.exe validate Furrow.clap` with no `--exclude` flags; 1 known JUCE wrapper `state-invalid-random` warning | Passed (exit 0) |
| `pluginval 1.0.4` (VST3, strictness 10) | `pluginval.exe --strictness-level 10 --validate-in-process --validate Furrow.vst3` with WebView2 backend enabled | Passed (`SUCCESS`, exit 0) |
| Packaging | `python plugin/tools/package.py --build plugin/build-win --output plugin/dist --platform windows` produced `furrow-windows.zip` with VST3, CLAP, Standalone, `extras/reaper/` track templates, `LICENSE`, `README.md`, `docs/`, and SHA-256 `manifest.json` | Passed |

## Exact binary artifacts (v1.2.0)

Paths are relative to `plugin/build-win/Furrow_artefacts/Release/`:

| Artifact | SHA-256 |
| --- | --- |
| `VST3/Furrow.vst3/Contents/x86_64-win/Furrow.vst3` | `5e7008a75f24790ebbef0ab1857c7245fc57d000b5e9d6c6c106500884bc18b3` |
| `CLAP/Furrow.clap` | `3087c80f8b80ea0ccbecfff19d337ecb4f7d82e40e31445a3a228cf93f43205c` |
| `Standalone/Furrow.exe` | `c82630dfaaae549c100070de4c25ef01d3c28978d4975275ab453c71c53e87b9` |

## Remaining external gates

- macOS universal DAW checks (REAPER, Studio One, FL Studio).
- Windows interactive re-verification of V1.1/V1.2 features in REAPER, Studio One 7, and FL Studio.
- Authenticode Windows signing and Apple Developer ID signing and notarization (conditional workflows wired in `.github/workflows/release.yml`, pending repository signing secrets).
