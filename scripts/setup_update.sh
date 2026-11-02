#!/bin/bash
# ============================================================================
#  T3DOTM Switch 移植 - TU 升级档接入脚本（安装期融合方案）
#
#  在 PC 上把 base 镜像与 TU 代码补丁**预先融合**，生成的 patched/default.xex
#  与 game/、update/ 为兄弟目录，由 rexglue 运行时层叠 VFS 加载
#  （update > patched > DLC > game）。运行时**不需要**再 ApplyPatch，
#  因此也不再向 update/ 放置 default.xexp：
#
#   [TU 代码补丁 -> 安装期融合]
#     tools/xex_patcher_cli/xex_patcher \
#         game/default.xex  extracted/TU/default.xexp  patched/default.xexp
#     融合结果写入 patched/default.xex（已解密、已解压、TU 代码生效）。
#     rexglue SDK 的 UserModule 检测到 fused XEX（FILE_FORMAT_INFO 为
#     NONE/NONE）后会跳过运行时 sibling patch 查找。
#
#   [TU 数据覆盖]（均位于 update/，运行时 VFS 命中 update 层）
#     - camera.lxb / medaltimes.lxb / tuning.lxb
#     - text/{eng,fre,ger,ita,spa}/global.lxb
#     - ui/flash/mpui_tu.{apk,gfx}
#     - ui/uiglobal.lxb
#
#  用法：bash scripts/setup_update.sh
#  前置：TU 已用 tools/xenia_live_unpack.py 解包到 extracted/TU
#        （见 recomp/codegen_t3dotm.sh）。
#  重复执行安全（幂等：覆盖同名文件，不删除已有文件）。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXTRACTED="$ROOT/extracted"
TU_SRC="$EXTRACTED/TU"
GAME_DIR="$ROOT/game"
UPDATE_DIR="$ROOT/update"
PATCHED_DIR="$ROOT/patched"
PATCHER="$ROOT/tools/xex_patcher_cli/xex_patcher.exe"

log() { printf '[setup] %s\n' "$*"; }

if [ ! -d "$TU_SRC" ]; then
    echo "ERROR: 未找到 $TU_SRC；请先用 tools/xenia_live_unpack.py 解包 TU 容器。" >&2
    exit 1
fi

if [ ! -f "$GAME_DIR/default.xex" ]; then
    echo "ERROR: 未找到 base 镜像 $GAME_DIR/default.xex。" >&2
    exit 1
fi
if [ ! -f "$PATCHER" ]; then
    echo "ERROR: 未找到 xex_patcher：$PATCHER" >&2
    exit 1
fi

mkdir -p "$UPDATE_DIR" "$PATCHED_DIR"

# ---------------------------------------------------------------------------
# 1. TU 代码补丁：安装期融合 base + TU -> patched/default.xex
# ---------------------------------------------------------------------------
if [ -f "$TU_SRC/default.xexp" ]; then
    log "融合 base+TU -> patched/default.xex ..."
    # xex_patcher 是 Windows 原生程序，参数须用 Windows 路径
    "$PATCHER" "$(cygpath -w "$GAME_DIR/default.xex")" \
               "$(cygpath -w "$TU_SRC/default.xexp")" \
               "$(cygpath -w "$PATCHED_DIR/default.xex")"
    log "TU 代码补丁已融合 -> patched/default.xex"
    # patched/default.xex 由 xex_patcher 生成（覆盖式），删除旧 default.xexp 残留
    rm -f "$PATCHED_DIR/default.xexp"
    # 新方案不再向 update/ 放置 default.xexp（运行时因已融合会跳过二次补丁），
    # 删除旧方案残留，避免 SDK/VFS 层出现"既融合又有 xexp"的歧义
    rm -f "$UPDATE_DIR/default.xexp"
else
    echo "WARN: 缺少 $TU_SRC/default.xexp，跳过代码补丁融合。" >&2
fi

# ---------------------------------------------------------------------------
# 2. TU 数据覆盖（只写 update/，不触碰 game/）
# ---------------------------------------------------------------------------
copy_any() {
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        mkdir -p "$(dirname "$dst")"
        cp -f "$src" "$dst"
        log "TU 数据覆盖 -> ${dst#$ROOT/}"
    fi
}

copy_any "$TU_SRC/camera.lxb"       "$UPDATE_DIR/camera.lxb"
copy_any "$TU_SRC/medaltimes.lxb"   "$UPDATE_DIR/medaltimes.lxb"
copy_any "$TU_SRC/tuning.lxb"       "$UPDATE_DIR/tuning.lxb"
copy_any "$TU_SRC/ui/uiglobal.lxb"  "$UPDATE_DIR/ui/uiglobal.lxb"
copy_any "$TU_SRC/ui/flash/mpui_tu.apk" "$UPDATE_DIR/ui/Flash/mpui_tu.apk"
copy_any "$TU_SRC/ui/flash/mpui_tu.gfx" "$UPDATE_DIR/ui/Flash/mpui_tu.gfx"

# 多语言文本 global.lxb
if [ -d "$TU_SRC/text" ]; then
    for lang in eng fre ger ita spa; do
        if [ -f "$TU_SRC/text/$lang/global.lxb" ]; then
            copy_any "$TU_SRC/text/$lang/global.lxb" "$UPDATE_DIR/text/$lang/global.lxb"
        fi
    done
fi

# ---------------------------------------------------------------------------
# 3. 汇总结果
# ---------------------------------------------------------------------------
echo ""
log "=== 接入完成，结果汇总 ==="
echo "[TU] patched/default.xex:  $([ -f "$PATCHED_DIR/default.xex" ] && echo OK || echo MISSING)"
echo "[TU] camera.lxb:           $([ -f "$UPDATE_DIR/camera.lxb" ] && echo OK || echo MISSING)"
echo "[TU] medaltimes.lxb:       $([ -f "$UPDATE_DIR/medaltimes.lxb" ] && echo OK || echo MISSING)"
echo "[TU] tuning.lxb:           $([ -f "$UPDATE_DIR/tuning.lxb" ] && echo OK || echo MISSING)"
echo "[TU] ui/uiglobal.lxb:      $([ -f "$UPDATE_DIR/ui/uiglobal.lxb" ] && echo OK || echo MISSING)"
echo "[TU] mpui_tu.apk:          $([ -f "$UPDATE_DIR/ui/Flash/mpui_tu.apk" ] && echo OK || echo MISSING)"
echo ""
log "注意：SD 卡上 patched/ 是融合 xex 的唯一来源；若 TU 内容变化，"
log "      需重新执行 codegen（codegen_t3dotm.sh）并重新构建 NRO，"
log "      再重跑本脚本生成新的 patched/default.xex。"