#!/usr/bin/env bash
#
# patch_libs.sh — 把 GAMEDIR/暂存目录下 Android .so 的 LIBC verneed 标 WEAK
#
# 用法: patch_libs.sh [--verify] [--all] <dir-or-.so> [<dir-or-.so> ...]
#   默认仅对白名单内的 9 个随包 Android .so 打补丁（避免误改设备库）。
#   --all   对任意含 LIBC verneed 的 ELF 打补丁（兜底）。
#   --verify 仅检查不写盘，返回 0/非0 供部署脚本判断。
#
# 依赖: python3（或 python）；实际改写逻辑在 tools/patch_libs.py
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="$(command -v python3 || command -v python || true)"
if [ -z "$PY" ]; then
  echo "error: python3/python not found; cannot run LIBC->WEAK patch" >&2
  exit 1
fi
exec "$PY" "$HERE/patch_libs.py" "$@"
