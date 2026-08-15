#!/bin/bash
# 仙剑奇侠传三 (Sword3 / com.softstar.G.swd3e) — NextOS ARM64 so-loader launcher.
#
# 纯 SDL2 2D 游戏。设备侧 SDL2 自动选后端（Mali-450=fbdev,
# RK3562/Mali-G52=kmsdrm, Mali-G31=wayland）—— 绝不强制 SDL_VIDEODRIVER（项目铁律）。
# 前置（BYO-data，不入库）：游戏 .so 与 assets/ 由用户自备并放在本目录。布局见 README.md。
#
# 数据布局（$GAMEDIR）：
#   sword3            <- loader 二进制（build_docker.sh 产出，git-ignored）
#   libSWD3E.so libSDL2.so libSDL2_image.so libSDL2_mixer.so libSDL2_ttf.so
#   libsmpeg2.so libhidapi.so libc++_shared.so   <- 游戏自带 Android .so
#   assets/           <- 游戏资源（Resource/ Music/ Video/ zh-Hans/ zh-Hant/ ...）
#   sword3-nextos.sh  <- 本脚本

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
else
  controlfolder="/storage/.config/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

CUR_TTY=/dev/tty0
$ESUDO chmod 666 $CUR_TTY 2>/dev/null


GAMEDIR="/$directory/ports/sword3"
cd "$GAMEDIR"

# 缩短绝对路径（修复 isAnySlotExist() 栈 canary 溢出 → abort sig=6）：
# 游戏 isAnySlotExist() 用固定大小栈缓冲拼存档路径（如 /storage/roms/ports/sword3/Setting/save99.sav，
# 47 字符），设备绝对路径过长 → 溢出栈哨兵 tls+0x28 → __stack_chk_fail → abort。
# 资源路径(60+字符)能过，仅存档缓冲偏小。/storage 为 FAT 不支持符号链接，故在 tmpfs(/tmp)
# 建最短【bind mount】指回游戏目录，使所有绝对路径变短（存档路径压到 ~27 字符，远低于缓冲上限）。
# 关键：此前用符号链接(symlink)只能缩短 argv[0]，游戏内部用 /proc/self/exe 取基址时会解析穿透
# symlink 回到真实长路径 /storage/roms/ports/sword3/... → 定长栈缓冲仍被撑爆（生产也 abort）。
# 改用 bind mount：argv[0] 与 /proc/self/exe 都得到短路径 /tmp/s3，彻底覆盖两条取基址来源；
# 同时让 gdb 等不会因 symlink 被 canonicalize 而失真。mount 不可用时回退 symlink。
SHORTLINK="/tmp/s3"
umount "$SHORTLINK" 2>/dev/null; rm -rf "$SHORTLINK"
if ! ( mkdir -p "$SHORTLINK" && mount --bind "$GAMEDIR" "$SHORTLINK" ) 2>/dev/null; then
  rm -rf "$SHORTLINK"; ln -s "$GAMEDIR" "$SHORTLINK"
fi
export ANDROID_APP_PATH="$SHORTLINK/sword3"
GAMEDIR="$SHORTLINK"
cd "$GAMEDIR"
# loging
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
# LOG="$GAMEDIR/nosso.log"; : > "$LOG"
$ESUDO chmod 666 /dev/uinput 2>/dev/null


# 单实例：杀残留 sword3 进程（多实例抢 GPU = 黑屏/卡死；二进制内也已处理，双保险）
pkill -9 -x sword3 2>/dev/null
sleep 1

# 日志（前台运行，退出后 ES/EmuStation 自动 resume；绝不掩码/停 emustation）
LOG="$GAMEDIR/debug.log"; : > "$LOG"
exec > >(tee -a "$LOG") 2>&1

# 调试开关：实时打印游戏每一次文件打开（路径+返回值），用于定位 RoleDataBase init
# 期间的数据源加载失败。定位完成后注释此行即可恢复干净日志。
export SHIM_DUMP_OPEN=1

# 库搜索路径：同目录放游戏自带 .so（liblog.so 桩等）。
# Android .so 的 NEEDED（libSDL2.so / libc.so / libm.so / libdl.so / libz.so）在 glibc
# 设备上无同名文件；而 /storage 是 FAT，不支持符号链接。故在 tmpfs(/tmp) 建同名
# 符号链接指向设备库，并置于 LD_LIBRARY_PATH 最前，让 dlopen 解析走设备库（同一
# inode，不会双加载 glibc）。
STAGING="/tmp/sword3libs"
rm -rf "$STAGING"; mkdir -p "$STAGING"

# 游戏设置目录 + env2.dat 占位。
# 游戏自带文件抽象层（fSetFilePointer 等）对 fopen 返回的 NULL 句柄不做判空，
# 直接 fseek/fread 之 → NULL 解引用崩溃（见 libbionic_shim.c v15 的 NULL 安全守卫）。
# 首个触发点：Setting/env2.dat 缺失 → fopen 返回 NULL → 写设置后回头 fSetFilePointer(NULL)
# → libc fseek(NULL) SIGSEGV（返回地址 libSWD3E.so+0x1428c4）。
# 故必须确保 Setting/ 存在且 env2.dat 占位（空文件即可，游戏首次存档会覆盖）。
# 另：libbionic_shim.so v15 已在代码层对 fseek/fread/ftell/fwrite/rewind/fclose 做 NULL
# 安全守卫，作为兜底；此处占位保证设置能正常读写持久化。
mkdir -p "$GAMEDIR/Setting"
# 仅在缺失时占位：绝不每次启动都清空已存在的真实设置（如把 env.dat 改名而来的 env2.dat）。
# 覆盖会废掉用户/游戏写回的持久化配置；缺失占位则兜底原始 NULL-fseek 崩溃场景。
if [ ! -f "$GAMEDIR/Setting/env2.dat" ]; then
  # 缺失：优先用游戏自带 env.dat 做种子（真实默认设置），否则建空占位兜底 NULL-fseek 崩溃。
  if [ -f "$GAMEDIR/Setting/env.dat" ]; then
    cp -f "$GAMEDIR/Setting/env.dat" "$GAMEDIR/Setting/env2.dat"
  else
    : > "$GAMEDIR/Setting/env2.dat"
  fi
fi
# 设备原生 SDL2 全家桶（glibc 原生、ABI 兼容；比 Android 版更稳）。
# 注意：Android libSDL2.so 在设备上已改名 .android，故 libSDL2.so 只能走此符号链接。
ln -sf /usr/lib/libSDL2-2.0.so.0      "$STAGING/libSDL2.so"
ln -sf /usr/lib/libSDL2_image-2.0.so.0 "$STAGING/libSDL2_image.so"
ln -sf /usr/lib/libSDL2_mixer-2.0.so.0 "$STAGING/libSDL2_mixer.so"
ln -sf /usr/lib/libSDL2_ttf-2.0.so.0   "$STAGING/libSDL2_ttf.so"
ln -sf /usr/lib/libpng16.so.16        "$STAGING/libpng16.so"
# Android 版 .so 的 NEEDED（libc/libm/libdl/libz）在 glibc 设备上以同名 SONAME 顶替。
ln -sf /usr/lib/libdl.so.2       "$STAGING/libdl.so"
ln -sf /usr/lib/libm.so.6        "$STAGING/libm.so"
ln -sf /usr/lib/libc.so.6        "$STAGING/libc.so"
ln -sf /usr/lib/libz.so.1        "$STAGING/libz.so"
export LD_LIBRARY_PATH="$STAGING:$GAMEDIR:/usr/lib:/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# 中文 locale：资源内含 zh-Hans / zh-Hant 两套。默认简体；繁体改 zh_TW.UTF-8。
# 需系统已生成对应 locale（locale -a 可见）；否则 C.UTF-8 兜底。
if locale -a 2>/dev/null | grep -qi '^zh_CN'; then
  export LANG="${LANG:-zh_CN.UTF-8}"
elif locale -a 2>/dev/null | grep -qi '^zh_TW'; then
  export LANG="${LANG:-zh_TW.UTF-8}"
else
  export LANG="${LANG:-C.UTF-8}"
fi
export LC_ALL="$LANG"

# 关闭调试光标角标（egl_shim 默认画一个十字；2D 游戏自带光标，关掉）。
export SUMMERTIME_CURSOR=1

# Governor performance：降低音频/引擎卡顿。
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -f "$g" ] && echo performance > "$g" 2>/dev/null
done

# 绝不给设备 SDL2 设 SDL_VIDEODRIVER（铁律）。本端口已用设备 libSDL2-2.0.so.0
# 顶替游戏自带的 Android libSDL2.so（见上方 STAGING 符号链接），视频/音频由设备
# SDL2 按其系统后端与 ALSA 处理；main.c 不再设 android 后端。
chmod +x "$GAMEDIR/sword3" 2>/dev/null

echo "=== Sword3 loader $(date -Is) ==="

# ── 退出热键（修复"无法按 Select+Start 退出" #3）─────────────────────────────
# 不再依赖 gptokeyb（本设备 control.txt 未暴露 $GPTOKEYB 变量；且仙剑为鼠标游戏、
# 手柄 evdev 空闲）。退出逻辑已下沉到 loader 二进制内部：main.c 启动一个后台线程，
# 用设备侧 SDL2 实例每帧轮询物理手柄的 SELECT+START，命中即 _exit(0)（参考
# gtalcs2 / nfs 的 in-binary exit 范式，不调用 teardown、不 kill 外部进程）。
# 脚本只负责把游戏跑起来，不需要任何退出监控。
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/sword3" >/dev/null

# 行缓冲输出，确保崩溃日志不丢（管道经 tee 时默认全缓冲会丢末尾）
stdbuf -oL -eL "$GAMEDIR/sword3"

# 退出清理：优先 PortMaster 标准收尾（pm_finish），兜底强杀残留 sword3 进程。
command -v pm_finish >/dev/null 2>&1 && pm_finish
pkill -9 -x sword3 2>/dev/null
