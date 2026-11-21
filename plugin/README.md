# Furrow plugin

The VST3, CLAP, and Standalone edition of Furrow (v1.2.0). The REAPER edition lives in the repository root and stays supported.

The sequencer engine plays SeqCore's V1 rules and matches the REAPER edition event for event. The sampler plays WAV, AIFF, FLAC, OGG, and MP3 sample lanes plus a synthesized built-in drum kit with RS5K curves, One-Shot or Gate playback, Reverse, and a per-lane resonant low-pass/high-pass filter across up to 16 stereo outputs. MIDI input triggers pads by lane note and switches patterns via Program Change or an optional keyswitch channel, while MIDI lanes output on channels 1 to 16. The embedded local-only WebView interface provides a canvas step grid (up to 64 lanes by 512 steps), all 11 V1 step-lane editors, live peak meters, a 256-pattern bank picker, a collapsible sample browser with live arrow-key hot-swap, standalone internal Play/Stop/BPM transport, `.furrowkit` kits, MIDI and 24-bit WAV drag-out, MIDI import preview, and SHA-256 missing-sample relink.

- [User guide](docs/user-guide.md) covers installation, workflows, shortcuts, and per-host MIDI routing.
- [Design system](docs/design-system.md) documents the Graphite visual tokens, layout rules, live-drag gesture protocol, and embedded UI asset layout.
- [Plugin spec and plan](docs/plugin-spec.md) covers scope, architecture, licence, and engine decisions.
- [UI prototype](ui/prototype/index.html) is the original standalone browser design prototype.

## Layout

| Folder | Holds |
| --- | --- |
| `engine/` | The sequencer engine: header-only C++20 with no JUCE or platform code |
| `sampler/` | The sampler, the built-in kit and the plugin's state format, also with no JUCE code |
| `src/` | The JUCE plugin around the engine |
| `ui/` | Embedded local-only Graphite WebView interface (`app/index.html`, `styles.css`, `app.js`) and original browser prototype (`prototype/index.html`) |
| `tools/` | Packaging script (`package.py`), local validation script, and platform installer definitions |
| `extras/` | REAPER multi-bus and MIDI-out `.RTrackTemplate` routing templates |
| `tests/` | Engine and sampler tests, `golden/` with the SeqCore traces they replay, and `plugin/` with tests that drive the built plugin |

## Build and test

The build needs CMake 3.28 or later, Ninja and a C++20 compiler. On Linux it also needs JUCE's development packages (ALSA, FreeType, fontconfig, X11, GTK 3, and WebKitGTK `libwebkit2gtk-4.1-dev` when `FURROW_ENABLE_WEBVIEW=ON`, the default). CMake downloads JUCE, clap-juce-extensions and Catch2 at pinned versions.

Engine tests only, with address and undefined-behaviour sanitizers:

```sh
cmake -S plugin -B plugin/build-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFURROW_BUILD_PLUGIN=OFF -DFURROW_SANITIZE=ON
cmake --build plugin/build-tests
plugin/build-tests/furrow_engine_tests
```

The plugin, as VST3 and CLAP, with its processor tests:

```sh
cmake -S plugin -B plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/build
plugin/build/furrow_plugin_tests
```

The bundles land in `plugin/build/Furrow_artefacts/Release/`. On Windows, CMake automatically downloads and verifies the pinned `Microsoft.Web.WebView2` NuGet package for static WebView2 linking.

## Checks on GitHub

`.github/workflows/plugin.yml` runs on every push or pull request that changes the plugin, SeqCore or the files its tests read:

- the engine tests on Linux (GCC and Clang), macOS and Windows, with address and undefined-behaviour sanitizers except on Windows;
- Linux ThreadSanitizer tests across engine and processor suites;
- a check that the golden traces still match SeqCore;
- a WebView-enabled release build of the plugin on each platform, checked with its processor and UI bridge tests, pluginval at strictness 10, Steinberg's VST3 validator and unfiltered clap-validator.

To try a build in a host, open the run on the repository's Actions tab and download `furrow-Windows`, `furrow-macOS` or `furrow-Linux` from its Artifacts list. Each archive includes `VST3`, `CLAP`, `Standalone`, `LICENSE`, `README.md`, `docs/`, and `manifest.json`. Copy the plugin into the host's plugin folder, for example `C:\Program Files\Common Files\VST3` on Windows or `~/Library/Audio/Plug-Ins/VST3` on macOS. Unsigned macOS builds need the quarantine flag cleared:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Furrow.vst3
```

## Golden traces

`tests/golden/make_golden.py` runs the REAPER edition's SeqCore in ysfx and records what it does in each scenario. After changing SeqCore, build the ysfx host once with `tools/jsfx_host/build.sh`, then regenerate the traces from the repository root:

```sh
python3 plugin/tests/golden/make_golden.py
```

The engine tests then fail until the engine agrees with SeqCore again.

## Licence

Everything under `plugin/` is licensed under the GNU Affero General Public License version 3 (see [LICENSE](LICENSE)), because the plugin builds on JUCE under that licence. The REAPER edition stays MIT.
