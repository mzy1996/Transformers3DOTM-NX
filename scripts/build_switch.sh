#!/usr/bin/env bash
# T3DOTM - Nintendo Switch build (devkitA64 + libnx + NVK/Mesa)
#
# Produces recomp/out/sw8/T3DOTM.nro
#
# Prerequisites:
#   - devkitPro with devkitA64 (DEVKITPRO must be set, e.g. /opt/devkitpro)
#   - cmake >= 3.25, ninja
#   - repos/nfsmw-nx/sdk        (Switch-capable rexglue-sdk base)
#   - repos/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch
#                               (built NVK Horizon SDK)
#   - recomp/generated/default  (codegen output; already committed in this tree)
#
# Usage:
#   bash scripts/build_switch.sh [--no-lto] [-j N]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/recomp"
SDK="$ROOT/repos/nfsmw-nx/sdk"
NVK="$ROOT/repos/mesa-switch/mesa-unified-install/opt/devkitpro/portlibs/switch"
TOOLCHAIN="$ROOT/tools/switch/cmake/switch-devkitA64.cmake"
BUILD_DIR="$RECOMP/out/sw8"

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

if [[ ! -f "$SDK/cmake/rexglue_switch.cmake" ]]; then
  echo "ERROR: Switch-capable SDK missing at $SDK" >&2
  exit 1
fi
if [[ ! -f "$NVK/lib/libvulkan.a" ]]; then
  echo "ERROR: NVK Horizon SDK missing at $NVK" >&2
  exit 1
fi
if [[ ! -f "$TOOLCHAIN" ]]; then
  echo "ERROR: toolchain file missing at $TOOLCHAIN" >&2
  exit 1
fi

echo "==> Configuring Switch build (LTO=$LTO)"
cmake -S "$RECOMP" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DREXSDK_DIR="$SDK" \
  -DREXGLUE_SWITCH_NVK_SDK="$NVK" \
  $LTO

echo "==> Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "==> Done: $BUILD_DIR/T3DOTM.nro"