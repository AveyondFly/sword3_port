#!/usr/bin/env bash
#
# package_portmaster.sh — 把构建产物 + 启动脚本 + 元数据组装成标准 PortMaster 端口，并打包为 zip。
#
# 设计要点：
#   - sword3 / libbionic_shim.so / liblog.so 必须与启动脚本【同目录】：main.c 的
#     load_secondary_libs() 用 dirname(argv[0]) 加载这两个 shim，不能放进 libs/ 子目录。
#   - PortMaster 约定启动脚本名需与 control.txt 的 portname 一致（=sword3 → sword3.sh），
#     而本仓库启动脚本为 swd3de.sh（游戏名澄清重命名）。故在包内额外生成 sword3.sh 包装，
#     仅 exec swd3de.sh，文件夹仍叫 sword3（与 swd3de.sh 内 GAMEDIR=/$directory/ports/sword3 一致）。
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
PORT="$DIST/sword3"
PY="$(command -v python3 || command -v python || true)"
rm -rf "$DIST"
mkdir -p "$PORT"

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

echo "[*] 对端口根目录内随包 Android .so 做 LIBC->WEAK patch（幂等，不改动 libs/ 源文件）"
if [ -z "$PY" ]; then
  echo "ERROR: 未找到 python3/python，无法执行 LIBC->WEAK patch" >&2
  exit 1
fi
"$PY" "$HERE/tools/patch_libs.py" "$PORT" || true

echo "[*] 收集启动脚本与元数据"
cp -f "$HERE/swd3de.sh"       "$PORT/swd3de.sh"; chmod +x "$PORT/swd3de.sh"
cp -f "$HERE/control.txt"     "$PORT/control.txt"
cp -f "$HERE/swd3de.gptk"     "$PORT/swd3de.gptk"
cp -f "$HERE/PORT_README.txt" "$PORT/readme.txt"

echo "[*] 生成 PortMaster 启动包装 sword3.sh -> swd3de.sh"
cat > "$PORT/sword3.sh" <<'WRAP'
#!/bin/bash
# 由 package_portmaster.sh 生成：PortMaster 约定启动脚本名需与 portname 一致。
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/swd3de.sh" "$@"
WRAP
chmod +x "$PORT/sword3.sh"

echo "[*] 打包 $PORT -> $DIST/sword3.zip"
"$PY" - "$PORT" "$DIST/sword3.zip" <<'PY'
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
            if rel == "sword3" or rel.endswith(".sh"):
                info.external_attr = 0o755 << 16
            else:
                info.external_attr = 0o644 << 16
            with open(full, "rb") as fh:
                z.writestr(info, fh.read())
            n += 1
print("zipped %d files -> %s" % (n, out))
PY

echo "==> 完成: $DIST/sword3.zip"
