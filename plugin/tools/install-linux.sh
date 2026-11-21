#!/usr/bin/env bash
set -euo pipefail
# Install Furrow VST3, CLAP, and Standalone for the current Linux user.
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$HOME/.vst3" "$HOME/.clap" "$HOME/.local/bin"
cp -R "$SRC/VST3/Furrow.vst3" "$HOME/.vst3/"
cp "$SRC/CLAP/Furrow.clap" "$HOME/.clap/"
install -m 755 "$SRC/Standalone/Furrow" "$HOME/.local/bin/furrow"
printf 'Installed Furrow to %s/.vst3, %s/.clap, and %s/.local/bin/furrow\n' "$HOME" "$HOME" "$HOME"
