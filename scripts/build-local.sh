#!/bin/bash
set -euo pipefail

# Build pvr.kofin locally and create an installable zip.
#
# Usage: ./scripts/build-local.sh [--debug]
#
# Requires: Kodi v21 source at /media/bluecon/docs/dev/ref/kodi-omega-full/
# Output:   /media/bluecon/docs/IT/kofin/builds/pvr.kofin-<ver>-linux-x86_64-omega.zip

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ADDON_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
KODI_SRC="/media/bluecon/docs/dev/ref/kodi-omega-full"
BUILDS_DIR="/media/bluecon/docs/dev/builds"

BUILD_TYPE="Release"
[[ "${1:-}" == "--debug" ]] && BUILD_TYPE="Debug"

ADDON_VERSION=$(grep 'version=' "$ADDON_DIR/pvr.kofin/addon.xml.in" | head -1 | sed 's/.*version="\([^"]*\)".*/\1/')
ZIP_NAME="pvr.kofin-${ADDON_VERSION}-linux-x86_64-omega.zip"
INSTALL_DIR="$KODI_SRC/build/addons"

echo "Building pvr.kofin $ADDON_VERSION ($BUILD_TYPE)"

cd "$ADDON_DIR/build"
cmake -DADDONS_TO_BUILD=pvr.kofin \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
      -DPACKAGE_ZIP=1 \
      "$KODI_SRC/cmake/addons"
make -j"$(nproc)"

echo "Packaging $ZIP_NAME"
mkdir -p "$BUILDS_DIR"
cd "$INSTALL_DIR"
zip -r "$BUILDS_DIR/$ZIP_NAME" pvr.kofin/


echo "Output: $BUILDS_DIR/$ZIP_NAME"
