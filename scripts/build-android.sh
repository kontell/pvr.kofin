#!/bin/bash
# Build pvr.kofin for Android ARM32, targeting Kodi v22 "Piers"
# Output: installable zip at ~/kofin/builds/
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_LOG="$PROJECT_DIR/build-android.log"
OUTPUT_DIR="${OUTPUT_DIR:-$HOME/kofin/builds}"
DOCKER_IMAGE="pvr-kofin-android-builder:latest"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; }

# Get version
get_version() {
    if [ -f "$PROJECT_DIR/VERSION" ]; then
        cat "$PROJECT_DIR/VERSION" | tr -d '[:space:]'
    else
        echo "0.1.0"
    fi
}

# Check Docker is available
check_docker() {
    if ! command -v docker &>/dev/null; then
        error "Docker is not installed"
        exit 1
    fi
    if ! docker info &>/dev/null; then
        error "Docker daemon is not running"
        exit 1
    fi
}

# Build the Docker image
build_docker_image() {
    cd "$PROJECT_DIR"

    if docker image inspect "$DOCKER_IMAGE" &>/dev/null; then
        info "Docker image exists, reusing cached image"
        return 0
    fi

    info "Building Docker image (first build takes 30-60 minutes)..."
    export DOCKER_BUILDKIT=1

    set -o pipefail
    docker build \
        --progress=plain \
        --tag "$DOCKER_IMAGE" \
        --file Dockerfile.android-build \
        . 2>&1 | tee "$BUILD_LOG"
    set +o pipefail

    success "Docker image built"
}

# Build the addon inside Docker
build_addon() {
    local version="$1"
    cd "$PROJECT_DIR"

    mkdir -p "$PROJECT_DIR/build-android" "$OUTPUT_DIR"

    info "Building pvr.kofin for Android ARM32..."

    set -o pipefail
    docker run --rm \
        -v "$PROJECT_DIR:/workspace" \
        -w /workspace \
        "$DOCKER_IMAGE" \
        bash -c "
            set -e
            export ANDROID_HOME=/opt/android-sdk
            export ANDROID_NDK_HOME=/opt/android-sdk/ndk/25.2.9519653
            export KODI_SOURCE=/opt/kodi
            export KODI_INCLUDE=\"\$KODI_SOURCE/xbmc/addons/kodi-dev-kit/include\"
            export DEPENDS_ROOT=\"/opt/xbmc-depends/arm-linux-androideabi-21-release\"

            echo '--- Environment ---'
            echo \"Kodi headers: \$KODI_INCLUDE\"
            echo \"Depends root: \$DEPENDS_ROOT\"
            echo \"NDK: \$ANDROID_NDK_HOME\"

            # Build
            mkdir -p /workspace/build-android
            cd /workspace/build-android
            rm -f CMakeCache.txt

            cmake /workspace \
                -DCMAKE_TOOLCHAIN_FILE=\$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
                -DANDROID_ABI=armeabi-v7a \
                -DANDROID_PLATFORM=android-21 \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_PREFIX_PATH=\"\$DEPENDS_ROOT\" \
                -DKODI_INCLUDE_DIR=\"\$KODI_INCLUDE\" \
                -DCMAKE_FIND_ROOT_PATH=\"\$DEPENDS_ROOT\" \
                -DCMAKE_CXX_STANDARD=17

            make -j\$(nproc) VERBOSE=1

            echo '--- Build complete ---'
            ls -la pvr.kofin.so

            # Package
            echo '--- Packaging ---'
            STAGING=/tmp/pvr.kofin
            rm -rf \$STAGING
            mkdir -p \$STAGING

            # addon.xml from template
            VERSION=\"$version\"
            sed -e 's|@ADDON_DEPENDS@|<import addon=\"kodi.binary.global.main\" version=\"2.0.3\"/><import addon=\"kodi.binary.instance.pvr\" version=\"9.2.0\"/>|' \
                -e 's|@PLATFORM@|android|g' \
                -e 's|library_@PLATFORM@|library_android|g' \
                -e \"s|version=\\\"0.1.0\\\"|version=\\\"\$VERSION\\\"|\" \
                -e 's|@LIBRARY_FILENAME@|pvr.kofin.so|g' \
                /workspace/pvr.kofin/addon.xml.in > \$STAGING/addon.xml

            # Copy files
            cp pvr.kofin.so \$STAGING/
            cp -r /workspace/pvr.kofin/resources \$STAGING/
            cp /workspace/pvr.kofin/icon.png \$STAGING/ 2>/dev/null || true
            cp /workspace/pvr.kofin/changelog.txt \$STAGING/ 2>/dev/null || true
            cp /workspace/LICENSE.md \$STAGING/ 2>/dev/null || true

            # Create zip
            cd /tmp
            zip -r /workspace/build-android/pvr.kofin-\${VERSION}-android-arm32.zip pvr.kofin/

            echo '--- Done ---'
            ls -la /workspace/build-android/pvr.kofin-\${VERSION}-android-arm32.zip
        " 2>&1 | tee -a "$BUILD_LOG"
    set +o pipefail

    # Copy zip to output
    local zipfile="$PROJECT_DIR/build-android/pvr.kofin-${version}-android-arm32.zip"
    if [ -f "$zipfile" ]; then
        cp "$zipfile" "$OUTPUT_DIR/"
        success "Zip created: $OUTPUT_DIR/pvr.kofin-${version}-android-arm32.zip"
    else
        error "Build completed but zip not found"
        exit 1
    fi
}

# Main
main() {
    echo ""
    echo "========================================"
    echo "  pvr.kofin Android ARM32 Build"
    echo "  Kodi v22 Piers"
    echo "========================================"
    echo ""

    > "$BUILD_LOG"

    check_docker

    VERSION=$(get_version)
    info "Version: $VERSION"

    build_docker_image
    build_addon "$VERSION"

    echo ""
    echo "========================================"
    success "Build complete!"
    echo "========================================"
    echo ""
    echo "  Output: $OUTPUT_DIR/pvr.kofin-${VERSION}-android-arm32.zip"
    echo ""
    echo "  Install in Kodi:"
    echo "    Settings > Add-ons > Install from zip file"
    echo ""
}

main "$@"
