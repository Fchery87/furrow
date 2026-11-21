#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=${FURROW_BUILD_DIR:-"$root/plugin/build"}
artifacts="$build/Furrow_artefacts/Release"
cmake -S "$root/plugin" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFURROW_ENABLE_WEBVIEW=ON
cmake --build "$build" --parallel "${FURROW_BUILD_JOBS:-2}"
ctest --test-dir "$build" --output-on-failure
missing=0
if [[ -n ${PLUGINVAL:-} ]] && [[ -x $PLUGINVAL ]]; then
    "$PLUGINVAL" --strictness-level 10 --timeout-ms 120000 --validate "$artifacts/VST3/Furrow.vst3"
else
    printf '%s\n' 'MISSING: set PLUGINVAL to the pinned pluginval executable.' >&2
    missing=1
fi
if [[ -n ${VST3_VALIDATOR:-} ]] && [[ -x $VST3_VALIDATOR ]]; then
    "$VST3_VALIDATOR" "$artifacts/VST3/Furrow.vst3"
else
    printf '%s\n' 'MISSING: set VST3_VALIDATOR to the Steinberg validator executable.' >&2
    missing=1
fi
if [[ -n ${CLAP_VALIDATOR:-} ]] && [[ -x $CLAP_VALIDATOR ]]; then
    "$CLAP_VALIDATOR" validate "$artifacts/CLAP/Furrow.clap"
else
    printf '%s\n' 'MISSING: set CLAP_VALIDATOR to the pinned clap-validator executable.' >&2
    missing=1
fi
if ((missing)); then
    printf '%s\n' 'Local validation is INCOMPLETE. Build/tests passing is not a release pass.' >&2
    exit 2
fi
printf '%s\n' 'Local build, tests and format validators passed. Host/platform acceptance remains separate.'
