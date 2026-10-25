# Furrow plugin

The VST3 and CLAP edition of Furrow. The REAPER edition lives in the repository root and stays supported.

It is in development. The sequencer engine is done: it plays SeqCore's V1 rules and matches the REAPER edition event for event. The plugin has no interface or sampler yet. A new instance plays a fixed four-lane pattern as MIDI out and as simple tones, and saves the rack with the host project.

- [Plugin spec and plan](docs/plugin-spec.md) covers scope, architecture, licence, the build order and the engine's deliberate differences from SeqCore.
- [UI prototype](ui/prototype/index.html) is a clickable design prototype. Open the file in a Chromium-based browser or Firefox. It needs no build step, and it plays sound through the browser, so click Play or press Space. Its sounds are stand-ins, and its code will not ship in the plugin.

## Layout

| Folder | Holds |
| --- | --- |
| `engine/` | The sequencer engine: header-only C++20 with no JUCE or platform code |
| `src/` | The JUCE plugin around the engine |
| `tests/` | Engine tests, and `golden/`, the SeqCore traces they replay |

## Build and test

The build needs CMake 3.28 or later, Ninja and a C++20 compiler. On Linux it also needs JUCE's usual development packages (ALSA, FreeType, fontconfig, X11 and GTK). CMake downloads JUCE, clap-juce-extensions and Catch2 at pinned versions.

Engine tests only, with address and undefined-behaviour sanitizers:

```sh
cmake -S plugin -B plugin/build-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFURROW_BUILD_PLUGIN=OFF -DFURROW_SANITIZE=ON
cmake --build plugin/build-tests
plugin/build-tests/furrow_engine_tests
```

The plugin, as VST3 and CLAP:

```sh
cmake -S plugin -B plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build plugin/build
```

The bundles land in `plugin/build/FurrowSpike_artefacts/Release/`.

## Checks on GitHub

`.github/workflows/plugin.yml` runs on every push or pull request that changes the plugin, SeqCore or the files its tests read:

- the engine tests on Linux (GCC and Clang), macOS and Windows, with address and undefined-behaviour sanitizers except on Windows;
- a check that the golden traces still match SeqCore;
- a release build of the plugin on each platform, checked with pluginval at strictness 10, Steinberg's VST3 validator and clap-validator.

To try a build in a host, open the run on the repository's Actions tab and download `furrow-spike-Windows`, `furrow-spike-macOS` or `furrow-spike-Linux` from its Artifacts list. Each holds a `VST3` and a `CLAP` folder. Copy the plugin into the host's plugin folder, for example `C:\Program Files\Common Files\VST3` on Windows or `~/Library/Audio/Plug-Ins/VST3` on macOS. The builds are not signed yet, so macOS blocks them until you clear the download flag:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Furrow Spike.vst3"
```

## Golden traces

`tests/golden/make_golden.py` runs the REAPER edition's SeqCore in ysfx and records what it does in each scenario. After changing SeqCore, build the ysfx host once with `tools/jsfx_host/build.sh`, then regenerate the traces from the repository root:

```sh
python3 plugin/tests/golden/make_golden.py
```

The engine tests then fail until the engine agrees with SeqCore again.

## Licence

Everything under `plugin/` is licensed under the GNU Affero General Public License version 3 (see [LICENSE](LICENSE)), because the plugin builds on JUCE under that licence. The REAPER edition stays MIT.
