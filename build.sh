#!/usr/bin/env bash
# Cross-compiles the unlocker to a Windows x64 DLL.
#
# SteamOS has a read-only root, so the mingw-w64 toolchain lives in a container.
# Run this from the host and it re-enters the container automatically; run it
# from inside the container and it builds directly.
#
#   ./build.sh              configure + build
#   ./build.sh clean        wipe the build directory first
#   ./build.sh install      build, then copy the DLL into the game directory
#
# Override the impersonated DLL with PROXY_TARGET=winmm ./build.sh

set -euo pipefail

CONTAINER="${CONTAINER:-mgs4dev}"
PROXY_TARGET="${PROXY_TARGET:-dbghelp}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
GAME_DIR="${GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/METAL GEAR SOLID 4/MGS4}"

# Re-enter the container unless the cross-compiler is already on PATH.
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    if ! command -v distrobox >/dev/null 2>&1; then
        echo "error: no mingw-w64 toolchain and no distrobox to fall back on" >&2
        exit 1
    fi
    echo ">> entering container '$CONTAINER'"
    exec distrobox enter --name "$CONTAINER" -- \
        env PROXY_TARGET="$PROXY_TARGET" BUILD_TYPE="$BUILD_TYPE" GAME_DIR="$GAME_DIR" \
        bash "$ROOT/build.sh" "$@"
fi

if [[ "${1:-}" == "clean" ]]; then
    echo ">> removing $BUILD"
    rm -rf "$BUILD"
    shift || true
fi

echo ">> configuring (PROXY_TARGET=$PROXY_TARGET, $BUILD_TYPE)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DPROXY_TARGET="$PROXY_TARGET" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo ">> building"
cmake --build "$BUILD" --parallel

ARTIFACT="$BUILD/$PROXY_TARGET.dll"
echo ">> built $ARTIFACT"
x86_64-w64-mingw32-objdump -p "$ARTIFACT" | sed -n '/Export Address Table/,/^$/p' | head -20 || true

if [[ "${1:-}" == "install" ]]; then
    if [[ ! -d "$GAME_DIR" ]]; then
        echo "error: game directory not found: $GAME_DIR" >&2
        exit 1
    fi
    cp "$ARTIFACT" "$GAME_DIR/"
    echo ">> installed to $GAME_DIR/$PROXY_TARGET.dll"
    echo
    echo "   Steam launch options:"
    echo "     WINEDLLOVERRIDES=\"$PROXY_TARGET=n,b\" %command%"
fi
