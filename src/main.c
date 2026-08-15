/*
 * main.c — 仙剑奇侠传三 (Sword3 / com.softstar.G.swd3e) ARM64 so-loader for NextOS.
 *
 * 纯 SDL2 2D 游戏（入口 SDL_main）。libSWD3E.so 动态链接一组 Android .so：
 *   libc++_shared, libSDL2, libSDL2_image, libSDL2_mixer, libSDL2_ttf,
 *   libsmpeg2, libhidapi（外加 libGLESv1_CM/libGLESv2/libEGL 由设备 Mali 驱动提供）。
 *
 * 启动流程（参考 summertimesaga 的 SDL_main 范式）：
 *   1) 设备侧 SDL2 初始化窗口（egl_shim 自动选后端：Mali-450=fbdev,
 *      RK3562/Mali-G52=kmsdrm, Mali-G31=wayland）——绝不强制 SDL_VIDEODRIVER。
 *   2) 把游戏自带的 Android libSDL2 等 .so 以 RTLD_GLOBAL 载入全局符号域，
 *      使其 SDL_*, Mix_*, TTF_*, IMG_*, SMPEG_* 可被 libSWD3E.so 的导入解析。
 *   3) so_load(libSWD3E.so) 并解析其导入（GLES/EGL/OpenSLES/Android 由
 *      编译进 loader 的 shim + 设备库经 dlsym(RTLD_DEFAULT) 回退覆盖）。
 *   4) 构造假 JavaVM/JNIEnv，调 JNI_OnLoad + nativeSetupJNI，再调 SDL_main。
 *
 * 注：SDL_VIDEODRIVER=android 只设给“游戏内置的静态 SDL2”（它只编译了 android
 * 后端），设备侧 SDL2 已在步骤 1 按系统后端初始化完毕——这**不违反**项目铁律
 * “绝不指定 SDL_VIDEODRIVER”（铁律约束的是设备侧后端自选）。
 *
 * SDL2_image 策略（关键）：必须使用【随包】Android 版 libSDL2_image.so，严禁加载
 * 设备 /usr/lib/libSDL2_image-2.0.so.0（其 IMG_Load 在 glibc 上解码失败 → 黑屏）。
 * 所有随包 Android .so 须在【部署期】经 tools/patch_libs.sh 把 .gnu.version_r 的
 * "LIBC" verneed 标为 WEAK（main.c 不执行 patch，详见 load_secondary_libs 契约）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "libSWD3E.so"
#define HEAP_MB 384

/* 游戏自带、需以 RTLD_GLOBAL 预载的 secondary .so。
 * 顺序铁律：libbionic_shim.so（导出 __sF@LIBC + Android_JNI_*）与 liblog.so
 * 必须排在 libc++_shared.so 之前——libc++_shared 的 std::cout/cerr 静态初始化
 * 在自身 dlopen(RTLD_NOW) 时就解析 &__sF[1]，缺则整个 C++ 运行时加载失败，进而
 * 连累 libsmpeg2/libhidapi（它们 NEEDED libc++_shared）一并失败。
 *
 * 索引约定（Bug A 修复后）：
 *   0 libbionic_shim.so    1 liblog.so    2 libc++_shared.so
 *   3 libSDL2.so           4 libSDL2_image.so   5 libSDL2_mixer.so
 *   6 libSDL2_ttf.so       7 libsmpeg2.so  8 libhidapi.so   9 NULL
 *
 * 重要：第 4 项必须是随包 Android 版 "libSDL2_image.so"（裸 soname），不可用设备
 * /usr/lib/libSDL2_image-2.0.so.0——后者 IMG_Load 在 glibc 上解码失败 → 黑屏。
 * 设备提供 libGLESv2/v1_CM/EGL 不在此表（见 load_device_gles）。 */
static const char *SECONDARY_SOS[] = {
    "libbionic_shim.so",
    "liblog.so",
    "libc++_shared.so",
    "libSDL2.so",
    "libSDL2_image.so", /* 修复点(Bug A)：随包 Android 版，非设备 libSDL2_image-2.0.so.0 */
    "libSDL2_mixer.so",
    "libSDL2_ttf.so",
    "libsmpeg2.so",
    "libhidapi.so",
    NULL,
};

/* SDL 窗口尺寸（由 egl_shim 设为设备原生分辨率） */
extern int egl_shim_screen_w;
extern int egl_shim_screen_h;

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;
  debugPrintf("\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
  if (tb && fault >= tb && fault < tb + text_size)
    debugPrintf("  fault = %s+0x%lx\n", SO_NAME, (unsigned long)(fault - tb));
#if defined(__aarch64__)
  ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.pc;
  uintptr_t lr = u->uc_mcontext.regs[30];
  debugPrintf("  PC=%p%s\n", (void *)pc,
              (tb && pc >= tb && pc < tb + text_size) ? "" : " (fora de " SO_NAME ")");
  if (tb && pc >= tb && pc < tb + text_size)
    debugPrintf("  PC = " SO_NAME "+0x%lx\n", (unsigned long)(pc - tb));
  if (tb && lr >= tb && lr < tb + text_size)
    debugPrintf("  LR = " SO_NAME "+0x%lx\n", (unsigned long)(lr - tb));
  else
    debugPrintf("  LR=%p\n", (void *)lr);
#endif
  /* 二进制级追踪（满足“定位具体崩溃点”需求）：
   * 1) backtrace：崩溃瞬间完整调用栈（库名+偏移），定位 fSetFilePointer 的调用方；
   * 2) /proc/self/maps：把 PC 等运行时地址映射到具体库+偏移。
   * 两者经 fd2(stderr) 直写，避免缓冲；sword3.sh 的 tee 会把 stderr 落 debug.log。 */
  {
    void *frames[64];
    int n = backtrace(frames, 64);
    debugPrintf("  [backtrace] %d frames:\n", n);
    backtrace_symbols_fd(frames, n, 2); /* fd 2 = stderr */
  }
  {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd >= 0) {
      char buf[512];
      ssize_t r;
      debugPrintf("  [maps] /proc/self/maps:\n");
      while ((r = read(fd, buf, sizeof buf)) > 0) write(2, buf, (size_t)r);
      close(fd);
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

/* 单实例：杀掉残留 sword3 进程（多实例抢 GPU = 黑屏/卡死） */
static void sw_kill_prior_instances(void) {
  pid_t self = getpid();
  DIR *d = opendir("/proc");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    pid_t pid = (pid_t)atoi(e->d_name);
    if (pid <= 0 || pid == self) continue;
    char path[64], link[512];
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0) continue;
    link[n] = 0;
    const char *b = strrchr(link, '/');
    b = b ? b + 1 : link;
    if (strcmp(b, "sword3") == 0) kill(pid, SIGKILL);
  }
  closedir(d);
}

/* governor performance：降低音频/引擎卡顿 */
static void sw_cpu_performance(void) {
  if (getenv("SW_NO_CPUPERF")) return;
  for (int i = 0; i < 8; i++) {
    char p[128];
    snprintf(p, sizeof(p),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
    int fd = open(p, O_WRONLY);
    if (fd >= 0) { (void)!write(fd, "performance", 11); close(fd); }
  }
}

/* #3 + #2 修复：二进制内后台输入线程（参考 gtalcs2 / nfs 的 in-binary exit 范式）。
 *
 * #3 退出：仙剑以鼠标操作为主、自身不轮询手柄，故每帧轮询物理手柄 SELECT+START，
 *         命中即 _exit(0)（不调 teardown、不 kill 外部进程，最可靠）。
 *
 * #2 鼠标模拟：设备无物理鼠标，把游戏手柄映射为鼠标，向【同一 SDL 进程】的事件队列
 *   注入 SDL_MOUSEMOTION / SDL_MOUSEBUTTON / SDL_MOUSEWHEEL，游戏 SDL_main 主循环直接
 *   读到，等价于真实鼠标。映射：
 *     右摇杆 = 精细移动 / 左摇杆 = 慢速移动 / 方向键 = 步进移动
 *     A = 左键 / B = 右键 / L1 = 滚轮上 / R1 = 滚轮下
 *   手柄 evdev 由 SDL 非独占读取，与游戏自身是否开手柄无关。
 *   退出与鼠标模拟合并为单一线程，避免两线程并发调 SDL_GameControllerUpdate 的隐患。 */
static int   g_mx = 0, g_my = 0;          /* 当前光标位置（屏幕坐标） */
static int   g_prevA = 0, g_prevB = 0;    /* 按键边沿检测 */
static int   g_prevL1 = 0, g_prevR1 = 0;

static void sw_push_motion(void) {
  SDL_Event e; memset(&e, 0, sizeof(e));
  e.type = SDL_MOUSEMOTION;
  e.motion.x = g_mx; e.motion.y = g_my;
  e.motion.xrel = 0; e.motion.yrel = 0;
  e.motion.state = SDL_GetMouseState(NULL, NULL);
  e.motion.windowID = 0;
  SDL_PushEvent(&e);
}

static void sw_push_button(Uint8 button, int down) {
  SDL_Event e; memset(&e, 0, sizeof(e));
  e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  e.button.button = button;
  e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  e.button.x = g_mx; e.button.y = g_my;
  e.button.windowID = 0;
  SDL_PushEvent(&e);
}

static void sw_push_wheel(int y) {
  SDL_Event e; memset(&e, 0, sizeof(e));
  e.type = SDL_MOUSEWHEEL;
  e.wheel.y = y;            /* >0 上滚, <0 下滚 */
  e.wheel.windowID = 0;
  SDL_PushEvent(&e);
}

static void *sw_input_thread(void *arg) {
  (void)arg;
  SDL_GameController *pad = NULL;
  g_mx = egl_shim_screen_w / 2;
  g_my = egl_shim_screen_h / 2;
  const int DEAD = 6000;    /* 摇杆死区 */
  const int FAST = 16;      /* 右摇杆满偏每帧像素 */
  const int SLOW = 8;       /* 左摇杆满偏每帧像素 */
  const int STEP = 10;      /* 方向键每帧步进像素 */
  for (;;) {
    SDL_GameControllerUpdate();
    if (!pad) {
      int n = SDL_NumJoysticks();
      for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
          pad = SDL_GameControllerOpen(i);
          if (pad) { debugPrintf("[input] pad opened (idx=%d)\n", i); break; }
        }
      }
    }
    if (pad) {
      /* #3：SELECT + START -> 退出 */
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) &&
          SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        static const char msg[] = "[pad] SELECT+START -> exit\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(0);
      }
      /* #2 鼠标移动：右摇杆(快) + 左摇杆(慢) + 方向键(步进) */
      Sint16 rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
      Sint16 ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
      Sint16 lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
      Sint16 ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      int dx = 0, dy = 0;
      if (abs(rx) > DEAD) dx += (rx * FAST) / 32768;
      if (abs(ry) > DEAD) dy += (ry * FAST) / 32768;
      if (abs(lx) > DEAD) dx += (lx * SLOW) / 32768;
      if (abs(ly) > DEAD) dy += (ly * SLOW) / 32768;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  dx -= STEP;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) dx += STEP;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    dy -= STEP;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  dy += STEP;
      if (dx || dy) {
        g_mx += dx; g_my += dy;
        if (g_mx < 0) g_mx = 0;
        if (g_mx > egl_shim_screen_w - 1) g_mx = egl_shim_screen_w - 1;
        if (g_my < 0) g_my = 0;
        if (g_my > egl_shim_screen_h - 1) g_my = egl_shim_screen_h - 1;
        sw_push_motion();
      }
      /* #2 按键：A=左键 B=右键（边沿触发） */
      int a = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
      int b = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
      if (a && !g_prevA) sw_push_button(SDL_BUTTON_LEFT, 1);
      if (!a && g_prevA) sw_push_button(SDL_BUTTON_LEFT, 0);
      if (b && !g_prevB) sw_push_button(SDL_BUTTON_RIGHT, 1);
      if (!b && g_prevB) sw_push_button(SDL_BUTTON_RIGHT, 0);
      g_prevA = a; g_prevB = b;
      /* #2 滚轮：L1=上滚 R1=下滚（边沿触发） */
      int l1 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
      int r1 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
      if (l1 && !g_prevL1) sw_push_wheel(1);
      if (r1 && !g_prevR1) sw_push_wheel(-1);
      g_prevL1 = l1; g_prevR1 = r1;
    }
    SDL_Delay(16);
  }
  return NULL;
}

/* 计算 loader 自身所在目录（secondary .so 与游戏 .so 与其同目录） */
static void sw_dir(char *out, size_t n, const char *argv0) {
  /* 优先用 argv[0]（不解析 symlink），使经短符号链接（如 /tmp/s3/sword3）启动时
   * basedir 也为短路径。游戏 isAnySlotExist() 用固定大小栈缓冲拼存档路径，长绝对
   * 路径会撑爆栈 canary → __stack_chk_fail → abort(sig=6)；缩短 basedir 即可规避。
   * 仅当 argv[0] 不可用才回退 readlink(/proc/self/exe)（会解析到真实长路径）。 */
  if (argv0 && argv0[0]) {
    char tmp[1536];
    if (argv0[0] == '/') {
      snprintf(tmp, sizeof(tmp), "%s", argv0);
    } else {
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)))
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, argv0);
      else
        snprintf(tmp, sizeof(tmp), "%s", argv0);
    }
    char *s = strrchr(tmp, '/');
    if (s) { *s = 0; snprintf(out, n, "%s", tmp); return; }
  }
  char link[1024];
  ssize_t l = readlink("/proc/self/exe", link, sizeof(link) - 1);
  if (l > 0) { link[l] = 0; char *s = strrchr(link, '/'); if (s) *s = 0; snprintf(out, n, "%s", link); }
  else if (!getcwd(out, n)) snprintf(out, n, ".");
}

/* 通用 hook：把游戏内定义的符号（mangled）跳到 shim 的本地实现（unmangled）。
 * 用于"定义在 libSWD3E.so 内部、so_resolve（只处理 SHN_UNDEF 导入）无法拦截、
 * 但 shim 提供了同语义实现"的函数。此时 text 仍为 RWX（so_finalize 尚未执行）。 */
static void sw_hook_game_func(const char *game_sym, const char *shim_sym) {
  uintptr_t ga = (uintptr_t)so_find_addr_safe(game_sym);
  void *sh = dlsym(RTLD_DEFAULT, shim_sym);
  if (ga && sh && ga != (uintptr_t)sh) {
    debugPrintf("[hook] %s %p -> %p\n", game_sym, (void *)ga, sh);
    hook_arm64(ga, (uintptr_t)sh);
  } else {
    debugPrintf("[hook] %s 未 hook (game=%p shim=%p)\n", game_sym, (void *)ga, sh);
  }
}

/* 步骤 1：设备侧 SDL2 窗口初始化。
 * egl_shim 自动选后端（fbdev/kmsdrm/wayland），绝不设 SDL_VIDEODRIVER（项目铁律）。 */
static void setup_device_sdl_video(void) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    fatal_error("device SDL_Init(VIDEO|JOYSTICK|GAMECONTROLLER) failed: %s", SDL_GetError());
  egl_shim_create_window();
  debugPrintf("Screen: %dx%d (via device SDL2)\n", egl_shim_screen_w, egl_shim_screen_h);
}

/* 步骤 2a：把游戏自带的 secondary .so 以 RTLD_GLOBAL 预载入全局符号域，
 * 供 libSWD3E.so 解析 SDL_*, Mix_*, TTF_*, IMG_*, SMPEG_*。加载顺序由 SECONDARY_SOS[]
 * 决定；libbionic_shim.so/liblog.so 必须在 libc++_shared.so 之前（见上方注释）。
 *
 * 部署期契约（非运行时 patch；main.c 不做 ELF 改写）：
 *   本函数假定 SECONDARY_SOS 中所有【随包 Android .so】已在部署期经
 *     tools/patch_libs.sh $GAMEDIR
 *   把 .gnu.version_r 里的 "LIBC" verneed 标为 WEAK（VER_FLG_WEAK）。
 *   若漏跑该 patch，glibc 的 ld.so 会报
 *     "undefined symbol: free, version LIBC" / "version `LIBC' not found"
 *   并直接拒绝 dlopen（启动即崩）。见 deploy.sh / tools/patch_libs.sh 与 README
 *   的 patch 步骤。libbionic_shim.so / liblog.so 由我们在 glibc 下编译，无 LIBC
 *   verneed，不在此 patch 之列。 */
static void load_secondary_libs(const char *basedir) {
  for (int i = 0; SECONDARY_SOS[i]; i++) {
    const char *name = SECONDARY_SOS[i];
    char path[1536];
    /* 先试同目录的绝对路径，再试裸名（靠 LD_LIBRARY_PATH） */
    snprintf(path, sizeof(path), "%s/%s", basedir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
      h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!h) {
      debugPrintf("  [warn] dlopen %s falhou: %s (非致命，若启动报缺符号再排查)\n",
                  name, dlerror());
    } else {
      debugPrintf("  loaded secondary: %s\n", name);
      /* 高亮审计日志：确认 Bug A 修复生效——确实加载了随包 Android 版
       * libSDL2_image.so（而非设备 libSDL2_image-2.0.so.0），且部署期 LIBC->WEAK
       * 已就绪（否则上面 dlopen 已失败）。 */
      if (strcmp(name, "libSDL2_image.so") == 0) {
        debugPrintf("  [ok] SDL2_image = bundled libSDL2_image.so (LIBC->WEAK patched at deploy)\n");
      }
    }
  }
}

/* 步骤 2b：把设备 Mali 驱动（裸 soname）拉入全局域，供 so_resolve 的 dlsym 回退。
 * 设备 libGLESv2/v1_CM/EGL 由系统提供，不随包，故不在此 loader 链接，仅运行期预载。 */
static void load_device_gles(void) {
  const char *globlibs[] = {"libGLESv2.so", "libGLESv1_CM.so", "libEGL.so", NULL};
  for (int i = 0; globlibs[i]; i++)
    if (!dlopen(globlibs[i], RTLD_NOW | RTLD_GLOBAL))
      debugPrintf("  [warn] dlopen %s (device): %s\n", globlibs[i], dlerror());
}

int main(int argc, char *argv[]) {
  (void)argc;
  install_crash_handler();
  sw_kill_prior_instances();
  sw_cpu_performance();
  debugPrintf("=== 仙剑奇侠传三 (Sword3) ARM64 so-loader (NextOS) ===\n");
  /* 启动横幅加策略标识：随包 SDL2_image + 部署期 LIBC->WEAK（便于现场日志核对）。 */
  debugPrintf("[build] %s %s (8-hook) (bundled SDL2_image + LIBC->WEAK)\n", __DATE__, __TIME__);

  /* 1) 设备侧 SDL2 初始化窗口：egl_shim 自动选后端（fbdev/kmsdrm/wayland），
   *    不给设备 SDL2 设 SDL_VIDEODRIVER（铁律）。 */
  setup_device_sdl_video();

  /* #3 + #2：启动二进制内后台输入线程（退出热键 + 鼠标模拟，单一线程避免并发
   * SDL_GameControllerUpdate）。仙剑以鼠标操作为主、自身不轮询手柄，故用后台线程
   * 读物理手柄：SELECT+START 即 _exit(0)；同时把手柄映射为鼠标事件注入事件队列，
   * 让无鼠标的掌机也能操控。不依赖 gptokeyb / 外部 kill。 */
  {
    pthread_t tid;
    if (pthread_create(&tid, NULL, sw_input_thread, NULL) == 0)
      pthread_detach(tid);
  }

  /* SDL2 加载策略说明（澄清旧版误导性注释）：本端口【随包】Android
   * libSDL2.so（裸 soname=libSDL2.so，与设备 libSDL2-2.0.so.0 带版本 soname 互不
   * 冲突）原样加载，承担 EGL→egl_shim→原生 SDL2、GLES→设备 Mali 的桥接。设备
   * libSDL2-2.0.so.0 仅在【链接期】进入 loader 二进制（用于 egl_shim_create_window
   * 创窗），并不进入游戏 secondary 集合（SECONDARY_SOS[3] 仍是 "libSDL2.so"）。
   * 运行时行为保持一致——不要改动 SDL2 的现有加载策略，仅修复了 Bug A 的
   * SDL2_image 那一项（改回随包 Android 版），避免范围蔓延。 */

  /* 2) 预载 secondary .so 到全局符号域（RTLD_GLOBAL）+ 设备 GLES 驱动。 */
  char basedir[1024];
  sw_dir(basedir, sizeof(basedir), argv[0]);
  debugPrintf("Loader dir: %s\n", basedir);
  load_secondary_libs(basedir);
  load_device_gles();

  /* 3) 载入主模块 libSWD3E.so */
  size_t heap_size = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %d MB failed", HEAP_MB);

  char so_path[1536];
  snprintf(so_path, sizeof(so_path), "%s/%s", basedir, SO_NAME);
  if (so_load(so_path, heap, heap_size) < 0)
    fatal_error("so_load(%s) failed", so_path);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base,
              text_size, data_base, data_size);

  if (so_relocate() < 0)
    fatal_error("so_relocate failed");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0)
    fatal_error("so_resolve failed");

  /* Hook 资源相关函数：so_resolve 仅处理 SHN_UNDEF 导入，无法拦截"定义在
   * libSWD3E.so 内部"的库内直接调用，故用 hook_arm64 改写函数入口，跳到 shim 的
   * 本地实现。这些函数承担 Android→glibc 的路径重定向（资源根=$GAMEDIR/assets、
   * 存档根=$GAMEDIR）。此时 text 仍为 RWX（so_finalize 尚未执行），可安全改写
   * prologue。game_sym 用 mangled 名（so_find_addr_safe 精确匹配），shim_sym 用
   * shim 内的 unmangled 名（dlsym(RTLD_DEFAULT) 命中本 .so 的导出）。 */
  sw_hook_game_func("APKXAndroid_JNI_FileOpen", "APKXAndroid_JNI_FileOpen");
  sw_hook_game_func("_Z11GetAPKXPathPc",        "GetAPKXPath");
  sw_hook_game_func("_Z15GetAPKXreadpathPc",    "GetAPKXreadpath");
  sw_hook_game_func("_Z13GetSDCardPathPc",      "GetSDCardPath");
  sw_hook_game_func("_Z13GetTargetPathPc",      "GetTargetPath");
  sw_hook_game_func("_Z12GetInterPathPc",       "GetInterPath");
  sw_hook_game_func("_Z15GetGAMESAVEPathPc",    "GetGAMESAVEPath");
  sw_hook_game_func("_ZN6fileIO15GetResourcePathEPKc", "fileIO_GetResourcePath");

  so_finalize();
  so_flush_caches();
  so_execute_init_array();

  /* 4) 解析入口与 JNI 钩子 */
  int (*p_JNI_OnLoad)(void *vm, void *reserved) =
      (void *)so_find_addr_safe("JNI_OnLoad");
  void (*p_nativeSetupJNI)(void *env, void *cls) =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_nativeSetupJNI");
  int (*p_SDL_main)(int, char **) = (void *)so_find_addr_safe("SDL_main");
  if (!p_SDL_main)
    fatal_error("SDL_main 未找到于 " SO_NAME);

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  static int fake_activity_class;
  void *cls = &fake_activity_class;

  /* 资源路径带 assets/ 前缀：设 ANDROID_APP_PATH 为 assets 目录，
   * 使 SDL_RWFromFile("Resource/...",...) 命中 ./assets/Resource/... */
  char assets_path[1536];
  snprintf(assets_path, sizeof(assets_path), "%s/assets", basedir);
  setenv("ANDROID_APP_PATH", assets_path, 1);
  setenv("ANDROID_ARGUMENT", assets_path, 1);
  setenv("ANDROID_PRIVATE", assets_path, 1);

  debugPrintf("JNI_OnLoad...\n");
  if (p_JNI_OnLoad) p_JNI_OnLoad(fake_vm, NULL);
  debugPrintf("nativeSetupJNI...\n");
  if (p_nativeSetupJNI) p_nativeSetupJNI(fake_env, cls);

  debugPrintf("SDL_main ...\n");
  char *main_argv[] = {"sword3", NULL};
  int rc = p_SDL_main(1, main_argv);
  debugPrintf("SDL_main returned %d\n", rc);

  return 0;
}
