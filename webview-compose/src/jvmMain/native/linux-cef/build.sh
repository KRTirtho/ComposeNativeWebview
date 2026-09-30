#!/usr/bin/env bash
# Builds the direct-CEF Linux backend for webview-compose.
#
# Downloads the official CEF binary SDK at build time (~292 MiB compressed),
# builds libcompose_cef_linux.so + cef_subprocess, and stages the full CEF
# runtime into src/jvmMain/resources/nucleus/native/linux-<arch>/ so it is
# packaged into the library jar (loaded at runtime by CefLinuxBridge via
# NativeLibraryLoader).
#
# Prerequisites: cmake, ninja, pkg-config gtk+-3.0, curl, JAVA_HOME.
# Usage: ./build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CEF_VERSION="146.0.10+g8219561+chromium-146.0.7680.179"
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)
        CEF_PLATFORM="linux64"
        RESOURCE_ARCH="linux-x64"
        ;;
    aarch64|arm64)
        CEF_PLATFORM="linuxarm64"
        RESOURCE_ARCH="linux-aarch64"
        ;;
    *) echo "Unsupported Linux architecture: $HOST_ARCH" >&2; exit 1 ;;
esac

RESOURCE_DIR="$SCRIPT_DIR/../../resources/nucleus/native/$RESOURCE_ARCH"
CEF_DIST_NAME="cef_binary_${CEF_VERSION}_${CEF_PLATFORM}_minimal"
CEF_CACHE_DIR="${CEF_CACHE_DIR:-$SCRIPT_DIR/../../../../../../build/cef-sdk}"
CEF_ARCHIVE="${CEF_ARCHIVE:-$CEF_CACHE_DIR/${CEF_DIST_NAME}.tar.bz2}"
CEF_ROOT="$CEF_CACHE_DIR/$CEF_DIST_NAME"
BUILD_DIR="$CEF_CACHE_DIR/cmake-build-webview-compose"
CEF_OUTPUT_DIR="$CEF_CACHE_DIR/staging"

mkdir -p "$CEF_CACHE_DIR" "$RESOURCE_DIR" "$CEF_OUTPUT_DIR"

if [ ! -f "$CEF_ARCHIVE" ]; then
    mkdir -p "$(dirname "$CEF_ARCHIVE")"
    DOWNLOAD_URL="https://cef-builds.spotifycdn.com/${CEF_DIST_NAME}.tar.bz2"
    DOWNLOAD_URL="${DOWNLOAD_URL//+/%2B}"
    echo "Downloading CEF SDK: $DOWNLOAD_URL"
    curl --fail --location --retry 3 "$DOWNLOAD_URL" -o "$CEF_ARCHIVE"
fi

if [ ! -f "$CEF_ROOT/CMakeLists.txt" ]; then
    echo "Extracting CEF SDK to $CEF_ROOT"
    tar -xjf "$CEF_ARCHIVE" -C "$CEF_CACHE_DIR"
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCEF_ROOT="$CEF_ROOT" \
    -DCEF_OUTPUT_DIR="$CEF_OUTPUT_DIR"
cmake --build "$BUILD_DIR" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"

# Stage the CEF runtime payload into the jar resources. Everything must be a
# flat file — NativeLibraryLoader sidecars do not support nested paths.
cp -a "$CEF_ROOT/Release/." "$RESOURCE_DIR/"
cp -a "$CEF_ROOT/Resources/." "$RESOURCE_DIR/"
rm -rf "$RESOURCE_DIR/locales"
rm -f "$RESOURCE_DIR/chrome-sandbox" "$RESOURCE_DIR/libvk_swiftshader.so" \
      "$RESOURCE_DIR/libvulkan.so.1" "$RESOURCE_DIR/vk_swiftshader_icd.json" \
      "$RESOURCE_DIR/libEGL.so" "$RESOURCE_DIR/libGLESv2.so" \
      "$RESOURCE_DIR/libcompose_webview_linux.so" "$RESOURCE_DIR/libcompose_webview_bootstrap.so"
cp -a "$CEF_ROOT/Resources/locales/en-US.pak" "$RESOURCE_DIR/en-US.pak"
cp -a "$CEF_OUTPUT_DIR/libcompose_cef_linux.so" "$RESOURCE_DIR/libcompose_cef_linux.so"
cp -a "$CEF_OUTPUT_DIR/cef_subprocess" "$RESOURCE_DIR/cef_subprocess"
chmod 755 "$RESOURCE_DIR/cef_subprocess"

# NativeLibraryLoader extracts sidecars next to the bridge; make sure the
# loader's stale cache is cleared so the new build is picked up.
for CACHE_DIR in "$HOME/.cache/nucleus/native"; do
    if [ -d "$CACHE_DIR" ]; then
        rm -rf "$CACHE_DIR"
        echo "Cleared NativeLibraryLoader cache: $CACHE_DIR"
    fi
done

echo "Built and staged the CEF Linux backend into $RESOURCE_DIR"
ls -lh "$RESOURCE_DIR/libcompose_cef_linux.so" "$RESOURCE_DIR/libcef.so" "$RESOURCE_DIR/cef_subprocess"
