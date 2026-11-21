# Furrow Graphite design system

This document defines the visual tokens, layout rules, interaction contracts, and embedded WebView asset layout for the Furrow plugin interface (`plugin/ui/app/`).

## Visual principles

- **Matte studio graphite, zero neon.** The interface uses neutral dark charcoal surfaces, crisp 1px hairlines, and high-contrast off-white ink. It avoids gradients, glassmorphism, and neon glows.
- **Monochrome chrome, functional lane color.** All structural controls stay neutral so each lane's color carries the musical state across the lane header, step grid, waveform, knobs, and step-lane bars.
- **The step grid stays primary.** Every side pane (Sample Browser, Lane Inspector, and Step Lanes editor) collapses cleanly so the grid remains usable down to the `720 x 420` minimum plugin window.
- **100% offline.** The shipping UI loads zero external URLs or remote fonts. `plugin/tests/ui_bridge_test.cjs` enforces this across `index.html`, `styles.css`, and `app.js`.

## Color tokens

All CSS tokens live in `:root` inside `plugin/ui/app/styles.css`. Keep the native WebView backing color in `plugin/src/PluginEditor.cpp` synced to `--bg` (`0xff111215`) to prevent background flash when the plugin window opens.

| Token | Hex / Value | Role |
| --- | --- | --- |
| `--bg` | `#111215` | Main canvas backdrop, inspector card background, native WebView2 fill |
| `--surface` | `#17191E` | Top bar, side drawers, lane header rail, bottom editor chrome, footer |
| `--surface-raised` | `#1F2229` | Buttons, active tabs, selected lane row, active segmented controls |
| `--surface-hover` | `#262A33` | Pointer hover state for buttons and menu rows |
| `--well` | `#0C0D10` | Recessed inputs, selects, transport cluster, waveform backdrop, meters |
| `--grid-trough` | `#0D0E12` | Recessed rounded channel behind each lane's step pads |
| `--grid-cell` | `#1D2027` | Inactive step pad on odd 4-step beat groups |
| `--grid-cell-alt` | `#181A20` | Inactive step pad on even 4-step beat groups |
| `--border` | `#252932` | Standard 1px panel dividers and control borders |
| `--border-subtle` | `#1D2028` | Inner hairline separators inside panels and cards |
| `--border-strong` | `#373D4B` | Popover borders, hover borders, keyboard focus outlines |
| `--text` | `#ECEEF2` | Primary ink, active values, playhead bar, waveform trim handles |
| `--text-secondary` | `#B4BAC6` | Button labels, unselected lane names, secondary readouts |
| `--text-muted` | `#7F8797` | Uppercase section headers, field labels, status bar hints |
| `--text-faint` | `#505766` | Beat ticks, muted lane names, drag grips, disabled chrome |
| `--accent` / `--accent-ink` | `#ECEEF2` / `#111215` | High-contrast active toggle fill and inverted text |
| `--play-green` | `#69C27B` | Active transport indicator and nominal peak meters |
| `--danger` | `#E0605B` | Clipping meters (> 92%), missing sample badges, Panic, Delete |

### Lane palette

The default 6-swatch lane palette is mirrored in three places so newly initialized racks, dropped audio files, and the inspector color popover match:

1. `#D99B38` (amber)
2. `#5A98D4` (steel blue)
3. `#D96B4F` (terracotta)
4. `#6FB065` (sage green)
5. `#C66A92` (muted rose)
6. `#857FD1` (soft indigo)

Defined in `plugin/ui/app/app.js` (`PALETTE`), `plugin/src/FurrowProcessor.cpp` (`FurrowProcessor::FurrowProcessor`, `resetMetadata`), and `plugin/src/EditorBridge.cpp` (`addSampleFiles`).

## Typography

- **UI sans (`--font-ui`):** `"Inter", "Segoe UI Variable Text", -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, sans-serif` at `12.5px / 1.35`.
- **Technical mono (`--font-mono`):** `"JetBrains Mono", "Cascadia Mono", "SF Mono", Consolas, ui-monospace, monospace`.
- **Tabular figures:** Every live numeric readout (BPM, bar/beat ruler, step counts, Hz, ms, percentages, pattern indices) sets `font-variant-numeric: tabular-nums` so numbers stay aligned while dragging.

## Layout and responsive geometry

- **Top bar (`48px` fixed height, non-wrapping):** Three horizontal zones.
  - **Left:** Furrow mark and wordmark, Sample Browser toggle (`B`), Pattern pill (`<`, bank picker trigger with `01` index and name, `>`), and queued pattern chip.
  - **Center:** Transport cluster (Play/Stop in Standalone or live status dot in DAW, BPM display/scrubber, Global Swing mini-knob) and **Groove** popover (`Truncate` and probability `Seed`).
  - **Right:** Icon Undo/Redo, draggable export group (`MIDI`, `WAV`, inline `Tail`), **Kit** menu (`Save kit`, `Load kit`, `Export MIDI`, `Import MIDI`, `Relink missing samples`), `Panic`, and Inspector toggle (`I`).
- **Workspace grid:** Collapsible left Sample Browser (`232px`), central Rack + collapsible Step Lanes editor (`168px` open, `34px` collapsed), and collapsible right Lane Inspector (`286px`).
- **Narrow window protection (`<= 920px`):** Side column widths tighten (`--head-w: 196px`, `--browser-w: 204px`, `--inspector-w: 248px`), non-essential toolbar text labels hide behind icons, and opening the Sample Browser automatically collapses the Inspector so the step grid remains visible down to `720 x 420`.
- **Contextual banners:** MIDI import preview renders as a dismissible bar above the rack only when an import preview is active, keeping the Lane Inspector focused on per-lane sound design.

## Canvas and interaction contracts

- **Step grid (`#gridCanvas` & `#rulerCanvas`):**
  - Sticky `30px` ruler renders `BAR N` and beat subdivisions (`1.2`, `1.3`, `1.4`) synced horizontally with `#gridScroll` and `#barsScroll`.
  - Step zoom pills offer `S` (`24px`), `M` (`30px`), and `Fit` (dynamically sized to the visible grid width).
  - Step pads use rounded rectangles (`5px` radius) inside recessed troughs (`8px` radius). Probability hatches (`prob < 100%`) clip inside the pad path. Active hits flash briefly on trigger and peak meters decay smoothly via `requestAnimationFrame`.
  - Vertical scroll sync between `#laneHeaders` and `#gridScroll` is bidirectional so headers and rows stay locked together.
- **Step lane editor (`#barsCanvas`):**
  - All 11 V1 step fields appear in one horizontal pill bar above the bars canvas. MIDI-only tabs (`Release`, `Mod A`, `Mod B`) sit after a divider and dim on sample lanes.
  - Double-clicking a bar resets that step's field to its default value.
- **Inspector cards & direct manipulation:**
  - Controls group into four cards: **Sample** (draggable Start/End trim handles directly on `#waveCanvas`, missing-sample banner when needed), **Envelope & Playback**, **Filter**, and **Mix & Routing**.
  - Filter **Cutoff** uses logarithmic scaling from `20 Hz` to `20,000 Hz`. Filter knobs dim to `32%` opacity and ignore pointer input when `Filter` is set to `Off`.
  - Per-lane actions (`Audition`, `Duplicate`, `Drag lane MIDI/WAV`, `Fill 2/3/4/8`, `Rotate`, `Invert`, `Clear`, `Delete`) live in the lane header `⋯` menu and right-click context menu.

## Live-drag gesture protocol

Continuous drags on knobs, waveform trim handles, Shift-drag step velocity, and step-editor bars update audio in real time without flooding undo history or rebuilding the DOM mid-drag:

1. **First change (`gesture: "start"`):** The C++ bridge records one pre-drag undo snapshot and applies the edit immediately to the audio engine or sampler without bumping `stateRevision()`.
2. **Throttled drag updates (`gesture: "move"`, ~40-45ms):** The bridge applies the live value to audio with `recordHistory = false` and `notifyUi = false`.
3. **Pointer release (`gesture: "end"`):** The bridge applies the final value with `recordHistory = false`, bumps `stateRevision()`, and notifies the host display. Pressing Undo restores the exact pre-drag state in one step.

## Embedded asset pipeline

- `plugin/CMakeLists.txt` compiles `ui/app/index.html`, `ui/app/styles.css`, `ui/app/app.js`, and `ui/app/favicon.svg` into `FurrowUIResources` via `juce_add_binary_data`.
- `plugin/src/PluginEditor.cpp` maps request paths to `BinaryData` symbol names (`/` -> `index_html`, `/styles.css` -> `styles_css`, `/app.js` -> `app_js`, `/favicon.svg` -> `favicon_svg`) and serves exact MIME types (`.html`, `.css`, `.js`, `.svg`, `.woff2`, `.json`).
- `plugin/tests/ui_bridge_test.cjs` verifies offline compliance and UI-to-bridge behavior directly against all UI files in CI without requiring a separate Node bundling step.

## Brand identity (Ploughed Ridge)

Furrow's emblem ("Ploughed Ridge") is a bold geometric `F` built from two chamfered parallel ridges plus a grounded off-white stem and warm amber (`#D99B38`) pulse block on a `#14161B` rounded Graphite tile (`#303542` outer border, `#222630` inner hairline on icons `>= 32px`).

### Source assets (`plugin/assets/brand/`)

| Asset | Purpose |
| --- | --- |
| `furrow-icon.svg` | Master 512px square app icon vector |
| `furrow-mark.svg` | Standalone 24px symbol (Graphite + Amber) used in the plugin top bar |
| `furrow-mark-mono-light.svg` / `furrow-mark-mono-dark.svg` | Single-color stamps for dark and light surfaces |
| `furrow-wordmark-dark.svg` / `furrow-wordmark-light.svg` | Horizontal lockups for docs, web, and release materials |
| `furrow-icon-{16,32,64,128,256,512,1024}.png` | Supersampled rasters (with optical boldness tuning at `<= 24px`) |
| `furrow.ico` | Multi-resolution Windows icon (`16` to `256px`) used by Inno Setup and Windows tooling |
| `furrow.icns` | Multi-resolution macOS icon (`16` to `1024px`) |

### Platform wiring & regeneration

- **JUCE builds (`plugin/CMakeLists.txt`):** `juce_add_plugin(Furrow ...)` wires `ICON_BIG` (`furrow-icon-512.png`) and `ICON_SMALL` (`furrow-icon-32.png`). JUCE compiles the icon resource directly into Windows `Furrow.exe` (taskbar, titlebar, Alt-Tab, Explorer), bundles `Icon.icns` into macOS `Furrow.app`, and embeds the runtime window icon on Linux.
- **Windows installer (`plugin/tools/installer-windows.iss`):** Sets `SetupIconFile` to `furrow.ico`, `UninstallDisplayIcon` to `{app}\Furrow.exe`, and wires icons on Start Menu and optional Desktop shortcuts.
- **Linux (`plugin/tools/install-linux.sh`, `plugin/tools/furrow.desktop`):** Installs scalable SVG and 256px PNG icons into `~/.local/share/icons/hicolor/` alongside a `furrow.desktop` launcher.
- **WebView UI (`plugin/ui/app/index.html` & `favicon.svg`):** Displays the Ploughed Ridge mark in the 48px top bar and links `favicon.svg`.
- **Regenerating rasters:** Run `python3 plugin/tools/generate_brand_assets.py` (standard library Python 3, zero third-party dependencies).
