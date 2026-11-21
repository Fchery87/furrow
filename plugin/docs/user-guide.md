# Furrow plugin user guide

Furrow 1.0.0 is a step sequencer and drum sampler instrument available as VST3, CLAP, and Standalone.

## Installation

- **Windows.** Copy `Furrow.vst3` to `C:\Program Files\Common Files\VST3\` and `Furrow.clap` to `C:\Program Files\Common Files\CLAP\`, or run the Inno Setup installer built from `plugin/tools/installer-windows.iss`.
- **macOS.** Copy `Furrow.vst3` to `~/Library/Audio/Plug-Ins/VST3/`, `Furrow.clap` to `~/Library/Audio/Plug-Ins/CLAP/`, and `Furrow.app` to `/Applications/`, or run the `.pkg` built by `plugin/tools/installer-macos.sh`.
- **Linux.** Run `plugin/tools/install-linux.sh` to install to `~/.vst3`, `~/.clap`, and `~/.local/bin/furrow`.

## DAW vs. Standalone

Furrow follows your DAW's transport, tempo, and bar boundaries. Load `Furrow.vst3` or `Furrow.clap` on an instrument track and press Play in your DAW to run the sequencer. The Standalone app (`Furrow.exe`, `Furrow.app`, or `Furrow`) has no host transport, so use it to audition sounds, edit kits, and build patterns offline.

## Channel Rack workflow

- **Step grid.** Left-click toggles a step. Left-drag paints steps on or off in one undoable edit. Right-drag erases steps. Shift-drag vertically adjusts step velocity. Alt-click cycles step probability between 100%, 75%, 50%, and 25%.
- **Keyboard.** Use arrow keys to move across steps and lanes, Enter or Space to toggle a step, Shift+Up/Down to nudge velocity, Ctrl/Cmd+Z to undo, and Ctrl/Cmd+Shift+Z or Ctrl+Y to redo.
- **Rotary knobs.** Drag vertically to change a value, hold Shift for fine control, double-click to reset to default, or focus and use arrow keys.
- **Step lane editor.** Select any of the 11 V1 tabs below the grid (Velocity, Release, Pitch, Fine pitch, Pan, Mod A, Mod B, Shift, Gate, Ratchet, Probability) and drag across the bars to draw values for the selected lane.
- **Lane tools.** Double-click a lane name to audition while stopped. Right-click a lane name or use the Inspector tools to Audition, Fill every 2/3/4/8 steps, Rotate left/right, Invert, Clear steps, or Delete lane.
- **Patterns.** Click the pattern name in the top bar to switch between patterns (patterns 1 to 16 appear in the dropdown; the engine stores up to 256), rename inline, clone, or clear patterns. Switching patterns while host transport is playing queues the switch for the next bar boundary.
- **Host automation.** `Global Swing`, `Truncate Swing`, and `Bypass` are exposed as host-automatable plugin parameters.

## Samples, kits, and MIDI files

- **Audio drag-and-drop.** Drop one or more WAV, AIFF, or FLAC files onto the window to add new sample lanes automatically. Use **Choose sample** in the Inspector to replace a lane's sample.
- **Missing-sample relink.** If a moved sample shows a warning badge, click **Relink folder** in the Inspector and choose a directory to match and relink missing samples by SHA-256 hash.
- **Kits (`.furrowkit`).** Save and load lane settings, names, colors, and sample references without overwriting your patterns. Legacy `.furrow` kit files remain readable.
- **MIDI drag-out and export.** Drag **Drag MIDI** in the top bar (full pattern) or **Drag lane** in the Inspector (available on MIDI out lanes) directly onto your DAW timeline, or click **Export MIDI** to save a `.mid` file.
- **MIDI drop-in and import.** Drop a `.mid` file onto the window or click **Preview MIDI** in the Inspector to inspect track count, step length, and any conversion diagnostics. Click **Import into pattern** to create a new pattern without overwriting your existing beat.

## Per-host MIDI routing

> [!NOTE]
> Initial Windows x64 interactive smoke checks passed in REAPER and Studio One 7 (see [verification-windows-2026-11-21.md](verification-windows-2026-11-21.md)). Deep per-host MIDI-out and multi-out matrices, FL Studio, macOS, and Linux DAW runs remain pending.

- **REAPER.** Insert Furrow on a track and set a lane's Target to **MIDI out** on channel `N`. Add a send from the Furrow track to your target instrument track with Audio set to None and MIDI set to channel `N` -> `All` (or `1`). For multi-out audio, add 16-channel track outputs and route `Out 2`..`Out 16` from the Inspector.
- **Studio One.** Load Furrow (VST3) on an Instrument Track. Create a second Instrument Track for your target synth and set its MIDI Input to `Furrow` on the matching MIDI channel. Enable additional outputs in the Studio One Mix console Outputs selector for per-lane audio routing.
- **FL Studio.** Load Furrow (VST3), open the VST Wrapper Settings gear icon, and assign an **Output port** number. Set the receiving instrument plugin's wrapper **Input port** to the same number.
