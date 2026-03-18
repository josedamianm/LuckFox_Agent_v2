#!/bin/bash
# Build camera_daemon for LuckFox Pico Max
#
# Run this INSIDE the luckfox-crossdev container (after git pull):
#
#   cd board/executables
#   rm -rf build && mkdir build && cd build
#   cmake -DCMAKE_TOOLCHAIN_FILE=../../lvgl_gui/toolchain-rv1106.cmake .. && make -j$(nproc)
#   ls -ltrh
#
# Then SCP the binary back to Mac:
#   scp nlighten_gpu_server:/home/josemanco/luckfox-crossdev/projects/LuckFox_Agent_v2/board/executables/build/camera_daemon camera_daemon
#
# Then sync to board:
#   ./sync.sh push board/executables/build/camera_daemon

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TOOLCHAIN="$SCRIPT_DIR/../../lvgl_gui/toolchain-rv1106.cmake"

echo "=== Building camera_daemon ==="
echo "Build dir : $BUILD_DIR"
echo "Toolchain : $TOOLCHAIN"
echo ""

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
      -DCMAKE_BUILD_TYPE=Release \
      ..

make -j"$(nproc)"

echo ""
echo "=== Build complete ==="
ls -lh "$BUILD_DIR/camera_daemon"
echo ""
echo "SCP to Mac:"
echo "  scp nlighten_gpu_server:/home/josemanco/luckfox-crossdev/projects/LuckFox_Agent_v2/board/executables/build/camera_daemon board/executables/build/camera_daemon"
echo ""
echo "Then sync to board:"
echo "  ./sync.sh push board/executables/build/camera_daemon"
