#!/usr/bin/env bash
# T3DOTM codegen wrapper.
#
# Re-runs the host codegen for T3DOTM. The patched/default.xex has already
# been pre-patched with the TU (tools/xex_patcher_cli); codegen works
# directly on this image. Do NOT pass --update_data_root here: it is a
# runtime VFS parameter, not a codegen parameter, and would risk double-
# patching the already-merged XEX.
#
# Usage:
#   REXGLUE=/path/to/rexglue(.exe) bash recomp/codegen_t3dotm.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RECOMP="$ROOT/recomp"
REXGLUE="${REXGLUE:-$ROOT/repos/nfsmw-nx/sdk/out/win-amd64/rexglue.exe}"
cd "$RECOMP"
echo "== codegen T3DOTM (patched default.xex, no update_data_root) =="
# max_file_size_bytes=4MB: UE3 games have very large functions; the default
# 1MB splits them and can make later linking slower.
"$REXGLUE" codegen t3dotm_manifest.toml --max_file_size_bytes=4194304
echo "codegen ok"
