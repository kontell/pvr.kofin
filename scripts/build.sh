#!/bin/bash
set -euo pipefail

# Build pvr.kofin for a given platform and Kodi version.
#
# Usage:
#   ./scripts/build.sh --os <linux|android|osx|ios|tvos> --arch <x86_64|armv7|aarch64|arm64>
#                      --kodi <21|22>
#                      [--kodi-src <path>] [--ndk <path>] [--build-type <Release|Debug>]
#                      [--output <path>] [--jobs <N>]
#
# Targets:
#   linux    x86_64 | armv7 | aarch64
#   android  armv7 | aarch64
#   osx      x86_64 | arm64        (on a Mac of either architecture)
#   ios      aarch64               (on a Mac)
#   tvos     aarch64               (on a Mac)
#
# Prerequisites:
#   All platforms:  cmake, make, autopoint
#   Linux armv7:    gcc-arm-linux-gnueabihf, g++-arm-linux-gnueabihf
#   Linux aarch64:  gcc-aarch64-linux-gnu, g++-aarch64-linux-gnu
#   Android:        Android NDK (pass --ndk <path>)
#   Apple:          Xcode, with the SDK for the target installed
#
# Examples:
#   ./scripts/build.sh --os linux --arch x86_64 --kodi 21 --kodi-src ~/kodi-omega
#   ./scripts/build.sh --os android --arch aarch64 --kodi 22 --kodi-src ~/kodi-piers --ndk ~/android-ndk-r25c
#   ./scripts/build.sh --os osx --arch arm64 --kodi 22 --kodi-src ~/kodi-piers

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
# macOS has no nproc.
JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)"

usage() {
    sed -n '3,25p' "$0" | sed -E 's/^# ?//'
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
[[ -z "$TARGET_OS" ]]    && echo "Error: --os required (linux|android|osx|ios|tvos)" && exit 1
[[ -z "$TARGET_ARCH" ]]  && echo "Error: --arch required (x86_64|armv7|aarch64|arm64)" && exit 1
[[ -z "$KODI_VERSION" ]] && echo "Error: --kodi required (21|22)" && exit 1
[[ -z "$KODI_SRC" ]]     && echo "Error: --kodi-src required (path to Kodi source tree)" && exit 1

[[ "$TARGET_OS" =~ ^(linux|android|osx|ios|tvos)$ ]] || { echo "Error: --os must be linux, android, osx, ios or tvos"; exit 1; }
[[ "$TARGET_ARCH" =~ ^(x86_64|armv7|aarch64|arm64)$ ]] || { echo "Error: --arch must be x86_64, armv7, aarch64 or arm64"; exit 1; }
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
# Absolutise it. Packaging below cds into $INSTALL_DIR before running zip, so a
# relative --output would be resolved against the wrong directory there and zip
# would fail with "Could not create output file". KODI_SRC gets the same treatment
# above; this one was missed, and only never bit because every caller happened to
# pass an absolute path.
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

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
    osx-x86_64|osx-arm64|ios-aarch64|tvos-aarch64)
        # The names are Kodi's own: its repository files these builds under
        # osx-x86_64, osx-arm64, ios-aarch64 and tvos-aarch64, and derives the
        # last two from CORE_PLATFORM_NAME plus a CPU of arm64.
        #
        # The values mirror what Kodi's depends build writes into
        # Toolchain_binaddons.cmake (tools/depends/configure.ac): which SDK, the
        # -arch flag, and the minimum OS version per target. Kodi reaches them by
        # bootstrapping its whole depends tree first; an add-on whose only
        # dependency is jsoncpp does not need the tree, only the same answers.
        #
        # The minimums are those of xbmc's Piers branch. They are not fixed for
        # the life of a release: macOS x86_64 was 10.14 at 22.0 beta 1 and has
        # been 10.15 since beta 2.
        case "${TARGET_OS}-${TARGET_ARCH}" in
            osx-x86_64)   APPLE_SDK=macosx;    APPLE_CPU=x86_64; APPLE_MIN="-mmacosx-version-min=10.15" ;;
            osx-arm64)    APPLE_SDK=macosx;    APPLE_CPU=arm64;  APPLE_MIN="-mmacosx-version-min=11.0" ;;
            ios-aarch64)  APPLE_SDK=iphoneos;  APPLE_CPU=arm64;  APPLE_MIN="-miphoneos-version-min=12.0" ;;
            tvos-aarch64) APPLE_SDK=appletvos; APPLE_CPU=arm64;  APPLE_MIN="-mappletvos-version-min=12.0" ;;
        esac
        if [[ "$TARGET_OS" == "osx" ]]; then
            APPLE_CORE="set(CORE_SYSTEM_NAME osx)"
        else
            APPLE_CORE="set(CORE_SYSTEM_NAME darwin_embedded)
set(CORE_PLATFORM_NAME $TARGET_OS)"
        fi
        command -v xcrun >/dev/null || { echo "Error: $TARGET_OS builds need Xcode (xcrun not found)"; exit 1; }
        APPLE_SDK_PATH="$(xcrun --sdk "$APPLE_SDK" --show-sdk-path)"
        APPLE_FLAGS="-arch $APPLE_CPU $APPLE_MIN -isysroot $APPLE_SDK_PATH"
        echo "  Toolchain: Xcode $APPLE_SDK SDK ($APPLE_SDK_PATH), $APPLE_CPU"
        # CMake turns this into its own -mmacosx-version-min, which would then
        # contradict the iOS or tvOS minimum given above.
        unset MACOSX_DEPLOYMENT_TARGET
        TOOLCHAIN_FILE="$TOOLCHAIN_DIR/${TARGET_OS}-${TARGET_ARCH}.cmake"
        cat > "$TOOLCHAIN_FILE" << TCEOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR $APPLE_CPU)
set(CPU $APPLE_CPU)
$APPLE_CORE
set(CMAKE_OSX_SYSROOT $APPLE_SDK_PATH)
set(CMAKE_C_FLAGS "$APPLE_FLAGS")
set(CMAKE_CXX_FLAGS "$APPLE_FLAGS")
set(CMAKE_FIND_ROOT_PATH $APPLE_SDK_PATH $APPLE_SDK_PATH/usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_FRAMEWORK LAST)
TCEOF
        CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE")
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
# The zip name carries no -kodi<N> suffix: the version's major already states the
# Kodi version (21.x.y / 22.x.y), and two fields that must agree eventually will
# not. repository.kontell derives the channel from the version.
ZIP_NAME="pvr.kofin-${ADDON_VERSION}-${TARGET_OS}-${TARGET_ARCH}.zip"
echo ""
echo "=== Packaging ==="
cd "$INSTALL_DIR"
zip -r "$OUTPUT_DIR/$ZIP_NAME" pvr.kofin/
echo ""
echo "Output: $OUTPUT_DIR/$ZIP_NAME"
