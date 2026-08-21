#!/usr/bin/env bash
#
# package_portmaster.sh — 组装标准 PortMaster 端口并打包为 swd3de.zip。
#
# 标准 PortMaster 包结构（解压后整体落设备的 ports/）：
#   swd3de.zip
#   ├── swd3de.sh        <- 启动脚本（在 ports/ 根，与端口目录同名）
#   └── swd3de/          <- 端口目录（即 GAMEDIR）
#       ├── sword3              loader 二进制（build_docker.sh 产出，git-ignored）
#       ├── libbionic_shim.so   bionic -> glibc 兼容垫片
#       ├── liblog.so           Android liblog 最小替身
#       ├── lib*.so             游戏自带 Android .so（随包，已 LIBC->WEAK patch）
#       ├── assets/             游戏资源目录（BYO-data，包内仅占位 .gitkeep）
#       ├── control.txt         PortMaster 元数据
#       ├── swd3de.gptk         手柄映射（占位）
#       └── readme.txt          部署说明
#
# 约定：
#   - sword3 / libbionic_shim.so / liblog.so 必须与 swd3de.sh 启动的 loader 同目录：
#     main.c 的 load_secondary_libs() 用 dirname(argv[0]) 加载这两个 shim，不能放进 libs/ 子目录。
#   - swd3de.sh 在包根（swd3de/ 之外），其内 GAMEDIR="/$directory/ports/swd3de" 指向下方 swd3de/。
#   - portname=swd3de（control.txt），故启动入口即 swd3de.sh，无需额外 sword3.sh 包装。
#   - zip 用 python zipfile 生成，跨平台（runner=ubuntu 有 python3，本地 Windows 可用 python）。
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Windows(Git Bash/MSYS) 下把 POSIX 路径转混合路径(E:/...)，否则传给 Windows 版
# python3.exe 时路径翻译异常；Linux(CI runner) 下保持 POSIX，不做转换。
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) HERE="$(cygpath -m "$HERE" 2>/dev/null || echo "$HERE")" ;;
esac
DIST="$HERE/dist"
STAGE="$DIST/stage"
PORT="$STAGE/swd3de"
PY="$(command -v python3 || command -v python || true)"
rm -rf "$DIST"
mkdir -p "$PORT/assets"

echo "[*] 收集构建产物（git-ignored，由 build_docker.sh 产出）"
for f in sword3 libbionic_shim.so liblog.so; do
  if [ ! -f "$HERE/$f" ]; then
    echo "ERROR: 缺少构建产物 $f，请先运行 build_docker.sh" >&2
    exit 1
  fi
  cp -f "$HERE/$f" "$PORT/$f"
  chmod +x "$PORT/$f"
done

echo "[*] 收集游戏自带 Android .so（入库，libs/；与 loader 同目录，main.c 按 dirname 加载）"
shopt -s nullglob
game_libs=("$HERE"/libs/*.so)
shopt -u nullglob
if [ ${#game_libs[@]} -eq 0 ]; then
  echo "ERROR: libs/ 下未找到任何游戏 .so，请将 9 个随包 Android .so 放入 libs/（详见 libs/README.md）" >&2
  exit 1
fi
for f in "${game_libs[@]}"; do
  cp -f "$f" "$PORT/$(basename "$f")"
done

echo "[*] 对端口目录内游戏 Android .so 做 LIBC->WEAK patch（幂等，不改动 libs/ 源文件）"
if [ -z "$PY" ]; then
  echo "ERROR: 未找到 python3/python，无法执行 LIBC->WEAK patch" >&2
  exit 1
fi
"$PY" "$HERE/tools/patch_libs.py" "$PORT" || true

echo "[*] 收集端口目录内元数据"
cp -f "$HERE/control.txt"     "$PORT/control.txt"
cp -f "$HERE/swd3de.gptk"     "$PORT/swd3de.gptk"
cp -f "$HERE/PORT_README.txt" "$PORT/readme.txt"

echo "[*] assets/ 占位（BYO-data，包内仅放空目录标记 .gitkeep）"
: > "$PORT/assets/.gitkeep"

echo "[*] 复制启动脚本 swd3de.sh 到包根（在 swd3de/ 之外）"
cp -f "$HERE/swd3de.sh" "$STAGE/swd3de.sh"
chmod +x "$STAGE/swd3de.sh"

echo "[*] 打包 $STAGE -> $DIST/swd3de.zip"
"$PY" - "$STAGE" "$DIST/swd3de.zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
n = 0
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            info = zipfile.ZipInfo(rel)
            # 强制可执行位：loader 二进制与启动脚本在设备上必须可运行，
            # 与源文件系统是否能记录 +x 解耦（Windows NTFS / 某些镜像无 +x）。
            if rel == "swd3de/sword3" or rel.endswith(".sh"):
                info.external_attr = 0o755 << 16
            else:
                info.external_attr = 0o644 << 16
            with open(full, "rb") as fh:
                z.writestr(info, fh.read())
            n += 1
print("zipped %d files -> %s" % (n, out))
PY

echo "==> 完成: $DIST/swd3de.zip"
