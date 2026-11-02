#!/usr/bin/env bash
# Build xex_patcher.exe - standalone Windows binary (no MSYS runtime dependency).
#
# Usage:
#   bash tools/xex_patcher_cli/build.sh
#
# Produces tools/xex_patcher_cli/xex_patcher.exe.
# Requires a x86_64-w64-mingw32 C++ toolchain. The devkitPro/msys2 mingw64 g++
# at /c/devkitPro/msys2/mingw64/bin/g++.exe is used by default; override with
# MINGW_GXX=/path/to/g++.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GXX="${MINGW_GXX:-/c/devkitPro/msys2/mingw64/bin/g++.exe}"

if [[ ! -x "$GXX" ]]; then
  echo "ERROR: mingw64 g++ not found at ${GXX}. Set MINGW_GXX to your toolchain." >&2
  exit 1
fi

# The mingw64 runtime DLLs (libisl/libmpc/libmpfr, ...) are loaded by cc1plus
# from PATH. Expose the mingw64 bin directory to Windows child processes in
# native form, otherwise compilation fails silently with "exit 1, no output".
MINGW64_BIN="$(cd "$(dirname "$GXX")" && pwd)"
export PATH="$MINGW64_BIN:$PATH"

"$GXX" --version | head -1

"$GXX" -std=c++20 -O2 -s -DNOMINMAX -DCBC=1 -DECB=1 \
  -fpermissive -w \
  -I"$ROOT/src" -I"$ROOT/thirdparty" -I"$ROOT/thirdparty/mspack" \
  "$ROOT/src/xex_patcher.cpp" \
  "$ROOT/src/memory_mapped_file.cpp" \
  "$ROOT/src/main.cpp" \
  -x c \
  "$ROOT/thirdparty/aes.c" \
  "$ROOT/thirdparty/mspack/lzxd.c" \
  -x none \
  -o "$ROOT/xex_patcher.exe"

echo "Built: $ROOT/xex_patcher.exe"
ls -la "$ROOT/xex_patcher.exe"