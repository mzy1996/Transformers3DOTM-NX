#!/usr/bin/env bash
# T3DOTM - Nintendo Switch build + verification (devkitA64 + libnx)
#
# Prerequisites:
#   - devkitPro with devkitA64 (DEVKITPRO must be set)
#   - cmake >= 3.25, ninja
#
# Usage:
#   bash scripts/build_and_verify.sh [--no-lto] [-j N]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/recomp"
BUILD_DIR="$RECOMP/out/sw8"
VERIFY="$ROOT/tools/verify_nro.py"
TOOLCHAIN="$ROOT/tools/switch/cmake/switch-devkitA64.cmake"
SDK="$ROOT/repos/nfsmw-nx/sdk"
NVK="$ROOT/repos/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch"

LTO="-DT3DOTM_LTO=ON"
JOBS=4
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-lto) LTO="-DT3DOTM_LTO=OFF" ;;
    -j) JOBS="$2"; shift ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
  shift
done

echo "==> Step 1: Re-configuring CMake (required after CMakeLists.txt changes)"
cmake -S "$RECOMP" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DREXSDK_DIR="$SDK" \
  -DREXGLUE_SWITCH_NVK_SDK="$NVK" \
  $LTO

echo "==> Step 2: Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

NRO="$BUILD_DIR/T3DOTM.nro"
if [[ ! -f "$NRO" ]]; then
    echo "ERROR: Build succeeded but $NRO not found" >&2
    exit 1
fi

echo "==> Step 3: Verifying NRO structure"
echo "File: $NRO"
ls -la "$NRO"
echo ""
python "$VERIFY" "$NRO"

echo ""
echo "==> Done. If verification shows PASS, copy $NRO to your SD card and test."
