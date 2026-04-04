#!/bin/bash
set -euo pipefail

# Build pvr.kofin for a given platform and Kodi version.
#
# Usage:
#   ./scripts/build.sh --os <linux|android> --arch <x86_64|armv7|aarch64> --kodi <21|22>
#                      [--kodi-src <path>] [--ndk <path>] [--build-type <Release|Debug>]
#                      [--output <path>] [--jobs <N>]
#
# Prerequisites:
#   All platforms:  cmake, make, autopoint
#   Linux armv7:    gcc-arm-linux-gnueabihf, g++-arm-linux-gnueabihf
#   Linux aarch64:  gcc-aarch64-linux-gnu, g++-aarch64-linux-gnu
#   Android:        Android NDK (pass --ndk <path>)
#
# Examples:
#   ./scripts/build.sh --os linux --arch x86_64 --kodi 21 --kodi-src ~/kodi-omega
#   ./scripts/build.sh --os android --arch aarch64 --kodi 22 --kodi-src ~/kodi-piers --ndk ~/android-ndk-r25c

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ADDON_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ADDON_ID="pvr.kofin"

# Defaults
TARGET_OS=""
TARGET_ARCH=""
KODI_VERSION=""
KODI_SRC=""
NDK_PATH=""
BUILD_TYPE="Release"
OUTPUT_DIR=""
JOBS="$(nproc)"

usage() {
    sed -n '3,14p' "$0" | sed 's/^# \?//'
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --os)       TARGET_OS="$2"; shift 2 ;;
        --arch)     TARGET_ARCH="$2"; shift 2 ;;
        --kodi)     KODI_VERSION="$2"; shift 2 ;;
        --kodi-src) KODI_SRC="$2"; shift 2 ;;
        --ndk)      NDK_PATH="$2"; shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --output)   OUTPUT_DIR="$2"; shift 2 ;;
        --jobs)     JOBS="$2"; shift 2 ;;
        -h|--help)  usage ;;
        *)          echo "Unknown option: $1"; usage ;;
    esac
done

# Validate required args
[[ -z "$TARGET_OS" ]]    && echo "Error: --os required (linux|android)" && exit 1
[[ -z "$TARGET_ARCH" ]]  && echo "Error: --arch required (x86_64|armv7|aarch64)" && exit 1
[[ -z "$KODI_VERSION" ]] && echo "Error: --kodi required (21|22)" && exit 1
[[ -z "$KODI_SRC" ]]     && echo "Error: --kodi-src required (path to Kodi source tree)" && exit 1

[[ "$TARGET_OS" =~ ^(linux|android)$ ]] || { echo "Error: --os must be linux or android"; exit 1; }
[[ "$TARGET_ARCH" =~ ^(x86_64|armv7|aarch64)$ ]] || { echo "Error: --arch must be x86_64, armv7, or aarch64"; exit 1; }
[[ "$KODI_VERSION" =~ ^(21|22)$ ]] || { echo "Error: --kodi must be 21 or 22"; exit 1; }
[[ "$TARGET_OS" == "android" && -z "$NDK_PATH" ]] && { echo "Error: --ndk required for Android builds"; exit 1; }

# Resolve paths
KODI_SRC="$(cd "$KODI_SRC" && pwd)"
[[ -n "$NDK_PATH" ]] && NDK_PATH="$(cd "$NDK_PATH" && pwd)"

# Extract addon version
ADDON_VERSION=$(grep '^ *version=' "$ADDON_DIR/$ADDON_ID/addon.xml.in" | head -1 | sed 's/.*version="\([^"]*\)".*/\1/')
echo "Building $ADDON_ID $ADDON_VERSION"
echo "  Target: $TARGET_OS $TARGET_ARCH (Kodi $KODI_VERSION)"
echo "  Kodi source: $KODI_SRC"
echo "  Build type: $BUILD_TYPE"

# Build directory
BUILD_DIR="$ADDON_DIR/build-ci-${TARGET_OS}-${TARGET_ARCH}-kodi${KODI_VERSION}"
INSTALL_DIR="$BUILD_DIR/install"
TOOLCHAIN_DIR="$BUILD_DIR/toolchain"
mkdir -p "$BUILD_DIR" "$INSTALL_DIR" "$TOOLCHAIN_DIR"

# Output directory
[[ -z "$OUTPUT_DIR" ]] && OUTPUT_DIR="$ADDON_DIR"
mkdir -p "$OUTPUT_DIR"

# Register addon in Kodi source tree
ADDON_DEF_DIR="$KODI_SRC/cmake/addons/addons/$ADDON_ID"
mkdir -p "$ADDON_DEF_DIR"
echo "$ADDON_ID $ADDON_DIR" > "$ADDON_DEF_DIR/$ADDON_ID.txt"

# Build cmake args
CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -DADDONS_TO_BUILD="$ADDON_ID"
    -DADDON_SRC_PREFIX="$(dirname "$ADDON_DIR")"
    -DADDONS_DEFINITION_DIR="$KODI_SRC/cmake/addons/addons"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
    -DPACKAGE_ZIP=1
)

# Generate toolchain and add platform-specific args
case "${TARGET_OS}-${TARGET_ARCH}" in
    linux-x86_64)
        echo "  Toolchain: native"
        ;;
    linux-armv7)
        echo "  Toolchain: arm-linux-gnueabihf"
        TOOLCHAIN_FILE="$TOOLCHAIN_DIR/linux-armv7.cmake"
        cat > "$TOOLCHAIN_FILE" << 'TCEOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TCEOF
        CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE")
        ;;
    linux-aarch64)
        echo "  Toolchain: aarch64-linux-gnu"
        TOOLCHAIN_FILE="$TOOLCHAIN_DIR/linux-aarch64.cmake"
        cat > "$TOOLCHAIN_FILE" << 'TCEOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TCEOF
        CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE")
        ;;
    android-armv7)
        echo "  Toolchain: Android NDK ($NDK_PATH) armeabi-v7a"
        CMAKE_ARGS+=(
            -DCMAKE_TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake"
            -DANDROID_ABI=armeabi-v7a
            -DANDROID_PLATFORM=android-21
            -DCPU=armv7a
        )
        ;;
    android-aarch64)
        echo "  Toolchain: Android NDK ($NDK_PATH) arm64-v8a (wrapper)"
        TOOLCHAIN_FILE="$TOOLCHAIN_DIR/android-aarch64.cmake"
        cat > "$TOOLCHAIN_FILE" << TCEOF
set(ANDROID_ABI arm64-v8a CACHE STRING "" FORCE)
set(ANDROID_PLATFORM android-21 CACHE STRING "" FORCE)
include($NDK_PATH/build/cmake/android.toolchain.cmake)
TCEOF
        CMAKE_ARGS+=(
            -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
            -DCPU=arm64-v8a
        )
        ;;
    *)
        echo "Error: unsupported platform $TARGET_OS-$TARGET_ARCH"
        exit 1
        ;;
esac

# Configure
echo ""
echo "=== Configuring ==="
cmake "${CMAKE_ARGS[@]}" "$KODI_SRC/cmake/addons"

# Build
echo ""
echo "=== Building ==="
make -C "$BUILD_DIR" -j"$JOBS"

# Package
ZIP_NAME="pvr.kofin-${ADDON_VERSION}-${TARGET_OS}-${TARGET_ARCH}-kodi${KODI_VERSION}.zip"
echo ""
echo "=== Packaging ==="
cd "$INSTALL_DIR"
zip -r "$OUTPUT_DIR/$ZIP_NAME" pvr.kofin/
echo ""
echo "Output: $OUTPUT_DIR/$ZIP_NAME"
