#!/usr/bin/env bash
#
# build_docker.sh — 轩辕剑3天之痕 (Sword3 / com.softstar.G.swd3e) ARM64 so-loader
# 交叉编译脚本。在 ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest
# (aarch64 交叉工具链 + SDL2 头文件 + Mali GLES 库) 内编译 loader，输出 `sword3`。
#
# 设计要点（与 summertimesaga / gtavc 同范式）：
#   - loader 自身只用“设备侧 SDL2”创建真实窗口（egl_shim_create_window），
#     链接期 -lSDL2 取镜像内 2.0.10，运行期由设备自带 libSDL2-2.0.so.0 按 soname 顶替。
#   - 游戏自带 Android libSDL2.so（soname=libSDL2.so，与设备侧 libSDL2-2.0.so.0
#     不同 soname，互不冲突）由 main.c 以 RTLD_GLOBAL 预载，供 libSWD3E.so 的 SDL_* 导入解析。
#   - gl* 由 -lGLESv2 -lGLESv1_CM 解析；egl/OpenSLES/ANativeWindow 等由编译进 loader 的
#     shim 经 my_dlopen/my_dlsym 运行时拦截（见 imports.c）。
#
# 关于 SDL2_image（重要，2026-07-31 架构清理澄清）：
#   - libSDL2_image.so 是【随包】Android .so，loader 链接期【不】链它；仅运行期由
#     main.c 以 RTLD_GLOBAL 加载（见 src/main.c 的 SECONDARY_SOS[4]），供 libSWD3E.so
#     的 IMG_* 导入解析。严禁使用设备 /usr/lib/libSDL2_image-2.0.so.0（glibc 版 IMG_Load
#     解码失败 → 黑屏）。
#   - 随包 Android .so 在 .gnu.version_r 把 libc/libm 标成 bionic 的 'LIBC' 版本节点，
#     glibc 设备没有 → dlopen 直接失败（"undefined symbol: free, version LIBC"）。
#     该 LIBC->WEAK 标记【不】在构建期做，而是在【部署期】由 tools/patch_libs.sh 完成
#     （main.c 不执行 ELF 改写）。本脚本只产出 loader 二进制与两个 glibc 编译的 shim .so。
#
set -euo pipefail

IMAGE="${IMAGE:-ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest}"
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "==> sword3 loader build (docker: $IMAGE)"
docker run --rm -v "$HERE":/work -w /work "$IMAGE" bash -c '
  set -e
  echo "[diag] LIBC_FWD_V macro as seen by docker (empty expected):"
  grep -n "define LIBC_FWD_V" src/libc_compat_shim.c || true
  echo "[diag] abort line as seen by docker:"
  grep -n "shim_abort\|LIBC_FWD0_V(abort)" src/libc_compat_shim.c || true
  CC=aarch64-linux-gnu-gcc
  # 注意：仅排除 libc_compat_shim.c（它定义 LIBC 版本节点，无 version script 时进主程序
  # 会触发 ld 链接错误/静默降级，故只在 libbionic_shim.so 中链接）。
  # libbionic_shim.c（fopen/open/fread 拦截器 + IMG_LoadPNG_RW 修复）必须链入主程序：
  # 拦截器只有在主程序内才能盖过 libc、被游戏调用并记录 g_last_png_path；PNG shim 在主程序
  # 与 .so 同版（均含递归修复），主程序副本优先运行、并与拦截器共享 g_last_png_path，无碍。
  SRCS=$(ls src/*.c | grep -v libc_compat_shim.c)
  echo "[*] compiling src/*.c -> build/*.o"
  mkdir -p build
  for f in $SRCS; do
    o="build/$(basename "$f" .c).o"
    $CC -c "$f" -I src \
        -D_GNU_SOURCE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
        -O2 -fPIC -fno-omit-frame-pointer \
        -Wno-int-conversion -Wno-incompatible-pointer-types \
        -o "$o"
  done
  echo "[*] linking -> sword3"
  # 链接命令（硬约束：loader 仅链接设备 SDL2/GLES 用于窗口化，不链接任何随包游戏库；
  # SDL2_image 等仅运行期 RTLD_GLOBAL）。-lpng 为 libbionic_shim 的 PNG 解码 shim
  # （shim_png_from_mem 直接调用 libpng API）所需，属 loader 自身依赖，与 SDL2/GLES 同性质。
  $CC -fPIE -pie build/*.o \
      -lSDL2 -lGLESv2 -lGLESv1_CM \
      -ldl -lm -lpthread -lstdc++ -lgcc_s -lpng \
      -rdynamic -Wl,-rpath,"\$ORIGIN" \
      -o sword3
  echo "[*] stripping"
  aarch64-linux-gnu-strip sword3 || true
  echo "[+] built: $(ls -l sword3 | awk "{print \$5}") bytes  ($(file sword3 | cut -d: -f2))"

  echo "[*] building libbionic_shim.so (bionic __sF + Android_JNI_* shim + LIBC version compat)"
  # 新增 src/libc_compat_shim.c：定义 LIBC 版本节点，把 libc++_shared/libsmpeg2/libhidapi
  # 实际依赖的 X@LIBC 符号转发到 glibc/libpthread/libm 真身，规避设备 glibc 不 honor
  # WEAK 版本回退导致的 "undefined symbol: free, version LIBC"。链接 -lpthread -lm 以
  # 解析 pthread_*/exp2/pow 等转发目标。
  # 显式单独编译 libc_compat_shim.o：不进入主程序 build/*.o（无 LIBC version script 时
  # ld 会静默降级 @LIBC 符号，跨工具链脆弱）；它仅在这里链入 libbionic_shim.so。
  $CC -c src/libc_compat_shim.c -I src -D_GNU_SOURCE -O2 -fPIC -o build/libc_compat_shim.o
  $CC -shared -fPIC -O2 -fno-omit-frame-pointer -D_GNU_SOURCE \
      -o libbionic_shim.so src/libbionic_shim.c build/libc_compat_shim.o \
      -Wl,--version-script=src/libbionic_shim.vers -ldl -lpthread -lm -lpng
  aarch64-linux-gnu-strip libbionic_shim.so || true
  echo "[+] built: $(ls -l libbionic_shim.so | awk "{print \$5}") bytes"

  echo "[*] building liblog.so (Android liblog 最小替身，glibc 编译，无 LIBC verneed)"
  # 用 stubs/liblog_stub.c 复现预编译产物，纳入构建更可复现；它不进 LIBC->WEAK 白名单。
  $CC -shared -fPIC -D_GNU_SOURCE -o liblog.so stubs/liblog_stub.c
  aarch64-linux-gnu-strip liblog.so || true
  echo "[+] built: $(ls -l liblog.so | awk "{print \$5}") bytes"
'

echo "==> done. 构建产物（git-ignored，仅提交源码）："
echo "    sword3            <- loader 二进制 (PIE)"
echo "    libbionic_shim.so <- bionic __sF + Android_JNI_* shim"
echo "    liblog.so         <- Android liblog 替身"
echo "    部署期须对 \$GAMEDIR(/storage/roms/ports/sword3) 下随包 Android .so 跑 tools/patch_libs.sh (LIBC->WEAK)。"
echo "    => $HERE/sword3  (git-ignored; 仅提交源码)"
