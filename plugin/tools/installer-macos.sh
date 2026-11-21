#!/usr/bin/env bash
set -euo pipefail
# Build a macOS .pkg installer for Furrow.
# Optional signing and notarization run only when APPLE_SIGN_ID and APPLE_NOTARY_PROFILE are set.
BUILD_DIR=${1:-plugin/build/Furrow_artefacts/Release}
OUT_DIR=${2:-plugin/dist}
VERSION=${FURROW_VERSION:-1.2.0}
mkdir -p "$OUT_DIR"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/Library/Audio/Plug-Ins/VST3" "$STAGE/Library/Audio/Plug-Ins/CLAP" "$STAGE/Applications"
cp -R "$BUILD_DIR/VST3/Furrow.vst3" "$STAGE/Library/Audio/Plug-Ins/VST3/"
cp -R "$BUILD_DIR/CLAP/Furrow.clap" "$STAGE/Library/Audio/Plug-Ins/CLAP/"
cp -R "$BUILD_DIR/Standalone/Furrow.app" "$STAGE/Applications/"

if [[ -n ${APPLE_SIGN_ID:-} ]]; then
  codesign --force --deep --options runtime --sign "$APPLE_SIGN_ID" \
    "$STAGE/Library/Audio/Plug-Ins/VST3/Furrow.vst3" \
    "$STAGE/Library/Audio/Plug-Ins/CLAP/Furrow.clap" \
    "$STAGE/Applications/Furrow.app"
fi

PKG="$OUT_DIR/Furrow-${VERSION}-macOS.pkg"
pkg_args=(--root "$STAGE" --identifier com.nochaserz.furrow --version "$VERSION")
if [[ -n ${APPLE_INSTALLER_SIGN_ID:-} ]]; then
  pkg_args+=(--sign "$APPLE_INSTALLER_SIGN_ID")
fi
pkgbuild "${pkg_args[@]}" "$PKG"

if [[ -n ${APPLE_NOTARY_PROFILE:-} ]]; then
  xcrun notarytool submit "$PKG" --keychain-profile "$APPLE_NOTARY_PROFILE" --wait
  xcrun stapler staple "$PKG"
fi
printf 'Built %s\n' "$PKG"
