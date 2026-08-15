#!/usr/bin/env bash
#
# deploy.sh — 仙剑奇侠传三 (Sword3 / com.softstar.G.swd3e) ARM64 so-loader 部署编排器
#
# 职责：build → 暂存交付物 → 部署期 patch(LIBC->WEAK) → 打印推送指引。
# 游戏 .so 与 assets/ 为 BYO-data（不入库），由用户自备放入暂存目录后再整体推送。
#
# 用法：
#   bash deploy.sh [--verify] [<staging-dir>]
#     <staging-dir>  暂存目录（默认 ./deploy）
#     --verify        仅对暂存目录做 LIBC->WEAK 校验，不重建/不拷贝
#
# 步骤顺序：
#   1) build_docker.sh 产出 sword3 / libbionic_shim.so / liblog.so
#   2) 拷贝交付物 + tools/ 进暂存目录
#   3) 提示用户放入自备游戏 .so + assets/
#   4) 对暂存目录跑 tools/patch_libs.sh（白名单 + 幂等）
#   5) 打印 rsync/scp 推送指引到设备 $GAMEDIR
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

VERIFY=0
STAGING="./deploy"
for a in "$@"; do
  case "$a" in
    --verify) VERIFY=1 ;;
    --*) echo "unknown flag: $a" >&2; exit 1 ;;
    *) STAGING="$a" ;;
  esac
done

GAMEDIR="/storage/roms/ports/sword3"

if [ "$VERIFY" -eq 1 ]; then
  echo "==> verify LIBC->WEAK in $STAGING"
  bash tools/patch_libs.sh --verify "$STAGING"
  exit $?
fi

echo "==> [1/4] build loader (docker cross-compile)"
bash build_docker.sh

echo "==> [2/4] stage deliverables into $STAGING"
mkdir -p "$STAGING"
for f in sword3 libbionic_shim.so liblog.so sword3.sh README.md; do
  if [ -f "$f" ]; then
    cp -f "$f" "$STAGING/"
  else
    echo "  [warn] missing $f (build may have failed)" >&2
  fi
done
# tools/（patch 脚本）随包，便于设备侧幂等补 patch
mkdir -p "$STAGING/tools"
cp -f tools/patch_libs.py tools/patch_libs.sh "$STAGING/tools/"

echo ""
echo "==> [3/4] 部署期 patch：把暂存目录下随包 Android .so 的 LIBC 标 WEAK"
echo "    ★ 请先把【自备的游戏 Android .so】(libSWD3E.so libSDL2.so libSDL2_image.so"
echo "      libSDL2_mixer.so libSDL2_ttf.so libsmpeg2.so libmpg123.so libhidapi.so"
echo "      libc++_shared.so) 与 assets/ 放入：$STAGING"
echo "    然后执行（幂等，可重复）："
echo "      bash tools/patch_libs.sh \"$STAGING\""
echo "    现在先对当前暂存内容跑一次（仅作用于白名单内已存在的 .so）："
bash tools/patch_libs.sh "$STAGING"
echo "    （游戏 .so 放入后请再跑一次上面的命令，确保全部 LIBC->WEAK）"

echo ""
echo "==> [4/4] 推送指引：把 $STAGING 整体同步到设备 $GAMEDIR"
echo "    # 方式 A：rsync（推荐，增量）"
echo "    rsync -avz --progress \"$STAGING/\" root@<device>:$GAMEDIR/"
echo "    # 方式 B：scp -r"
echo "    scp -r \"$STAGING/.\" root@<device>:$GAMEDIR/"
echo ""
echo "    设备侧首次部署只需一次（幂等，FAT 可写文件，无 symlink 需求）："
echo "      ssh root@<device> 'bash $GAMEDIR/tools/patch_libs.sh $GAMEDIR'"
echo "    然后启动："
echo "      ssh root@<device> 'bash $GAMEDIR/sword3.sh'"
echo ""
echo "==> 完成。注意：游戏 .so 与 assets/ 须自备放入 $STAGING（BYO-data，不入库）。"
