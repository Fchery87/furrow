# Furrow plugin user guide

Furrow 1.2.0 is a step sequencer and drum sampler instrument available as VST3, CLAP, and Standalone.

## Installation

- **Windows.** Copy `Furrow.vst3` to `C:\Program Files\Common Files\VST3\` and `Furrow.clap` to `C:\Program Files\Common Files\CLAP\`, or run the Inno Setup installer built from `plugin/tools/installer-windows.iss`.
- **macOS.** Copy `Furrow.vst3` to `~/Library/Audio/Plug-Ins/VST3/`, `Furrow.clap` to `~/Library/Audio/Plug-Ins/CLAP/`, and `Furrow.app` to `/Applications/`, or run the `.pkg` built by `plugin/tools/installer-macos.sh`.
- **Linux.** Run `plugin/tools/install-linux.sh` to install to `~/.vst3`, `~/.clap`, and `~/.local/bin/furrow`.

## DAW vs. Standalone

In a DAW, Furrow follows your host transport, tempo, and bar boundaries. Load `Furrow.vst3` or `Furrow.clap` on an instrument track and press Play in your DAW to run the sequencer.

In Standalone mode (`Furrow.exe`, `Furrow.app`, or `Furrow`), Furrow exposes an internal **Play/Stop** button, editable **BPM** input (20 to 300 BPM), and a fullscreen toggle button in the top bar. Press Spacebar (when not editing the grid or a text input) to start or stop playback, use the window Maximize button, or press `F11` for borderless fullscreen.

## Channel Rack workflow

- **Top bar & workspace drawers.** The single-row Graphite top bar groups controls into three zones: Browser toggle (`B`) and pattern picker on the left; Play/Stop (Standalone), BPM readout/scrubber, Global Swing knob, and the **Groove** popover (`Truncate` and probability `Seed`) in the center; Undo/Redo, draggable `MIDI` and `WAV` export chips (with inline `Tail` toggle), **Kit** menu, `Panic`, and Inspector toggle (`I`) on the right. Press `B` or `I` (or click the Step Lanes header) to collapse drawers and maximize grid space.
- **Step grid & ruler.** A sticky bar/beat ruler sits above the grid alongside `S` / `M` / `Fit` step zoom pills. Left-click toggles a step. Left-drag paints steps on or off in one undoable edit. Right-drag erases steps. Shift-drag vertically adjusts step velocity live. Alt-click cycles step probability between 100%, 75%, 50%, and 25%. Hovering any step displays its values in the status bar.
- **Keyboard.** Use arrow keys to move across steps and lanes, Enter or Space to toggle a focused step, Shift+Up/Down to nudge velocity, `B` to toggle the Sample Browser, `I` to toggle the Inspector, Ctrl/Cmd+Z to undo, and Ctrl/Cmd+Shift+Z or Ctrl+Y to redo.
- **Rotary knobs & live drag.** Drag vertically to update audio in real time (one atomic undo step on release), hold Shift for fine control, double-click to reset to default, or focus and use arrow keys. Filter **Cutoff** uses a logarithmic 20 Hz to 20 kHz curve.
- **Step lane editor.** Click any of the 11 V1 tabs in the horizontal pill strip below the grid (`Velocity`, `Probability`, `Pitch`, `Fine`, `Pan`, `Shift`, `Gate`, `Ratchet`, plus MIDI-only `Release`, `Mod A`, `Mod B`) and drag across the bars to draw values live for the selected lane. Double-click any bar to reset that step to its default value.
- **Lane headers and tools.** Each lane header includes a drag handle to reorder lanes, a 60fps peak meter, Mute/Solo buttons, and a `⋯` tools menu button (also reachable by right-clicking any lane row). Double-click a lane name to audition while stopped. The tools menu provides Audition, Duplicate lane, Drag lane MIDI/WAV, Fill every 2/3/4/8 steps, Rotate left/right, Invert, Clear steps, and Delete lane.
- **Sample playback and shaping.** The right Inspector groups controls into **Sample**, **Envelope & Playback**, **Filter**, and **Mix & Routing** cards, plus a color-dot swatch popover next to the lane name. Drag the **Start** and **End** trim handles directly on the waveform display to trim a sample live. Sample lanes default to **One-Shot** mode so full drum hits play through cleanly; switch to **Gate** mode when you want step gate length to truncate the hit. Toggle **Reverse** to flip sample playback, and enable the zero-allocation **Low-pass** or **High-pass** filter with logarithmic **Cutoff** and **Res** knobs.
- **256-pattern bank picker.** Use the `<` / `>` buttons in the top-bar pattern pill to step patterns, or click the pattern name to open the 16x16 bank picker covering all 256 patterns, with amber dots marking non-empty patterns and dashed borders showing queued patterns. Rename inline, clone, or clear patterns from the popover. Switching patterns while playing queues the switch for the next bar boundary.
- **MIDI input.** Incoming MIDI notes trigger lanes whose base note matches the incoming note. Incoming MIDI Program Change messages (0 to 127) switch patterns (queued on the next bar while playing). You can also pick an optional **Keyswitch** MIDI channel (1 to 16) in the pattern popover to switch patterns by note number.
- **Host automation.** `Global Swing`, `Truncate Swing`, and `Bypass` are exposed as host-automatable plugin parameters.

## Sample browser, kits, MIDI, and WAV drag-out

- **Collapsible sample browser.** Click **Browser** (or press `B`) to open the left sample drawer. Add one or more local drum folders with **+** (persisted in your user settings). Filter files by name, click a sample to preview it, double-click or press Enter to hot-swap it onto the selected lane without losing steps or lane settings, or click **+** on a file to add it as a new lane. Enable **Auto-swap** to step through a drum folder with Up/Down arrow keys and hear each sample in context while your beat loops.
- **Audio formats and drag-and-drop.** Furrow decodes WAV, AIFF, FLAC, OGG, and MP3 files. Drop one or more audio files onto the window to create new sample lanes automatically, or click **Choose** in the Inspector Sample card.
- **Missing-sample relink.** If a moved sample shows a warning badge, use **Retry** or **Relink folder** in the Inspector's missing-sample banner (or **Kit > Relink missing samples...** in the top bar) to match and relink missing samples by SHA-256 hash.
- **Kits (`.furrowkit`).** Open the **Kit** menu in the top bar to save or load lane settings, names, colors, and sample references without overwriting your patterns. Legacy `.furrow` kit files remain readable.
- **WAV audio drag-out.** Drag the **⋮⋮ WAV** chip in the top bar (full pattern) or **⋮⋮ Lane WAV** in the Inspector (single lane) straight onto your DAW timeline as a 24-bit stereo WAV. By default Furrow exports an exact loop-cut bounce; toggle the inline **Tail** button next to `WAV` to include up to 2 seconds of ringing decay past the loop boundary.
- **MIDI drag-out and import.** Drag the **⋮⋮ MIDI** chip in the top bar (full pattern) or **⋮⋮ Lane MIDI** in the Inspector onto your DAW timeline, or choose **Kit > Export pattern as MIDI...** to save a `.mid` file. Drop a `.mid` file onto the window or choose **Kit > Import MIDI file...** to inspect conversion diagnostics in the banner above the rack and click **Import into pattern**.

## Per-host routing and REAPER templates

> [!NOTE]
> Initial Windows x64 interactive smoke checks passed in REAPER and Studio One 7 (see [verification-windows-2026-11-21.md](verification-windows-2026-11-21.md)). Deep per-host MIDI-out and multi-out matrices, FL Studio, macOS, and Linux DAW runs remain pending.

- **REAPER.** Ready-made templates live in `plugin/extras/reaper/` (`Furrow-16Out-MultiBus.RTrackTemplate` and `Furrow-MIDI-Out-Routing.RTrackTemplate`). Copy them into your REAPER `TrackTemplates` folder to set up 16 stereo buses or 4-channel MIDI-out routing in one click.
- **Studio One.** Load Furrow (VST3) on an Instrument Track. Create a second Instrument Track for your target synth and set its MIDI Input to `Furrow` on the matching MIDI channel. Enable additional outputs in the Studio One Mix console Outputs selector for per-lane audio routing.
- **FL Studio.** Load Furrow (VST3), open the VST Wrapper Settings gear icon, and assign an **Output port** number. Set the receiving instrument plugin's wrapper **Input port** to the same number.
