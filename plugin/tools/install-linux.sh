#!/usr/bin/env bash
set -euo pipefail
# Install Furrow VST3, CLAP, Standalone, desktop entry, and icons for the current Linux user.
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$HOME/.vst3" "$HOME/.clap" "$HOME/.local/bin" \
  "$HOME/.local/share/applications" \
  "$HOME/.local/share/icons/hicolor/scalable/apps" \
  "$HOME/.local/share/icons/hicolor/256x256/apps"
cp -R "$SRC/VST3/Furrow.vst3" "$HOME/.vst3/"
cp "$SRC/CLAP/Furrow.clap" "$HOME/.clap/"
install -m 755 "$SRC/Standalone/Furrow" "$HOME/.local/bin/furrow"
if [[ -f "$SRC/assets/brand/furrow-icon.svg" ]]; then
  cp "$SRC/assets/brand/furrow-icon.svg" "$HOME/.local/share/icons/hicolor/scalable/apps/furrow.svg"
fi
if [[ -f "$SRC/assets/brand/furrow-icon-256.png" ]]; then
  cp "$SRC/assets/brand/furrow-icon-256.png" "$HOME/.local/share/icons/hicolor/256x256/apps/furrow.png"
fi
if [[ -f "$SRC/furrow.desktop" ]]; then
  cp "$SRC/furrow.desktop" "$HOME/.local/share/applications/furrow.desktop"
elif [[ -f "$SRC/tools/furrow.desktop" ]]; then
  cp "$SRC/tools/furrow.desktop" "$HOME/.local/share/applications/furrow.desktop"
fi
printf 'Installed Furrow to %s/.vst3, %s/.clap, and %s/.local/bin/furrow\n' "$HOME" "$HOME" "$HOME"
