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
#include <stdint.h>
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

/* 设备窗口尺寸（egl_shim 启动时从 SDL 测量，不是写死的 640x480） */
extern int egl_shim_screen_w;
extern int egl_shim_screen_h;
/* 游戏资源/SetMousePos 坐标系，和面板无关 */
#define SW_NATIVE_W 640
#define SW_NATIVE_H 480

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

/* A 确认：主指令原样交给游戏；自动/逃跑由 loader 补一层焦点后再走 IsClick。
 * B：战斗只置 BACK_KEY_CLICK；野外/菜单按住=右键按住、松开=右键松开（原始端口）。
 * X 不拦。Start 只留 SELECT+START 退出。
 * 系统菜单换人：PSV 开前触摸点头像；游戏自己把 L1/R1 映成 key 25/26，
 * Console30 / *IconChg 用这两个键切 fcs_Icon。这边无触摸，只把肩键
 * 写成这两键；方向左右仍交给游戏（道具/法术列表要用来移动）。 */
#define SW_CMD_BTN_STRIDE 0x60
#define SW_BTN_ITEM_ID    88
#define SW_BTN_CLICKED    56
#define SW_BTN_DISABLED   44
#define SW_BTN_CLICKABLE  58
#define SW_N_MAIN_CMD     7
#define SW_N_EX_CMD       4
#define SW_N_OBS_CMD      2
#define SW_EXTRA_NONE     0
#define SW_EXTRA_RETREAT  1
#define SW_EXTRA_AUTO     2

static int g_extra_sel;
static int g_extra_click;
static int g_extra_saved_sel = 1;
static int sw_in_fight(void);

static void *sw_sym(const char *name) {
  return (void *)so_find_addr_safe(name);
}

static int *sw_menu_select(void) {
  static int *p;
  if (!p) p = (int *)sw_sym("MainMenu_selectitem");
  return p;
}

static char *sw_cmd_buttons(void) {
  static char *p;
  if (!p) p = (char *)sw_sym("mainCommandButton");
  return p;
}

static char *sw_retreat_btn(void) {
  static char *p;
  if (!p) p = (char *)sw_sym("RetreatButton");
  return p;
}

static char *sw_auto_btn(void) {
  static char *p;
  if (!p) p = (char *)sw_sym("AutoButton");
  return p;
}

static int sw_ptr_in_range(const void *p, const char *base, int n) {
  return base && (const char *)p >= base &&
         (const char *)p < base + n * SW_CMD_BTN_STRIDE;
}

static int sw_is_extra_button(const void *btn) {
  return btn && (btn == sw_retreat_btn() || btn == sw_auto_btn());
}

static int sw_is_newui_button(const void *btn) {
  if (sw_ptr_in_range(btn, sw_cmd_buttons(), SW_N_MAIN_CMD))
    return 1;
  if (sw_ptr_in_range(btn, sw_sym("ExCommandButton"), SW_N_EX_CMD))
    return 1;
  if (sw_ptr_in_range(btn, sw_sym("ObsoltCommandButton"), SW_N_OBS_CMD))
    return 1;
  if (sw_is_extra_button(btn))
    return 1;
  return 0;
}

static int sw_btn_enabled(const char *btn) {
  return btn && btn[SW_BTN_CLICKABLE] && !btn[SW_BTN_DISABLED];
}

static int sw_btn_is_selected(const void *btn) {
  if (g_extra_sel) {
    if (btn == sw_retreat_btn())
      return g_extra_sel == SW_EXTRA_RETREAT;
    if (btn == sw_auto_btn())
      return g_extra_sel == SW_EXTRA_AUTO;
    return 0;
  }
  unsigned char *force_obs = sw_sym("ForceObsoltMenu");
  int *obs_sel = sw_sym("ObsoltSel");
  char *obs = sw_sym("ObsoltCommandButton");
  if (force_obs && *force_obs == 1 && obs && obs_sel && *obs_sel >= 1)
    return (const char *)btn == obs + (*obs_sel - 1) * SW_CMD_BTN_STRIDE;
  int *sel = sw_menu_select();
  if (!sel)
    return 0;
  return *(int *)((const char *)btn + SW_BTN_ITEM_ID) == *sel;
}

static SDL_Surface *sw_select_surf;
static int sw_select_w, sw_select_h;

static SDL_Surface *sw_ensure_select_surf(int w, int h) {
  if (w < 16) w = 80;
  if (h < 16) h = 80;
  if (sw_select_surf && sw_select_w == w && sw_select_h == h)
    return sw_select_surf;
  if (sw_select_surf)
    SDL_FreeSurface(sw_select_surf);
  sw_select_surf = SDL_CreateRGBSurface(0, w, h, 32,
                                        0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
  if (!sw_select_surf)
    return NULL;
  sw_select_w = w;
  sw_select_h = h;
  SDL_FillRect(sw_select_surf, NULL, SDL_MapRGBA(sw_select_surf->format, 0, 0, 0, 0));
  Uint32 c = SDL_MapRGBA(sw_select_surf->format, 255, 220, 64, 255);
  int t = 3;
  SDL_Rect r;
  r = (SDL_Rect){0, 0, w, t};         SDL_FillRect(sw_select_surf, &r, c);
  r = (SDL_Rect){0, h - t, w, t};     SDL_FillRect(sw_select_surf, &r, c);
  r = (SDL_Rect){0, 0, t, h};         SDL_FillRect(sw_select_surf, &r, c);
  r = (SDL_Rect){w - t, 0, t, h};     SDL_FillRect(sw_select_surf, &r, c);
  return sw_select_surf;
}

static void sw_blit_select_frame(void *btn, int x, int y) {
  void *ss2d = sw_sym("SS2D");
  if (!ss2d)
    return;
  SDL_Surface *dst = *(SDL_Surface **)((char *)ss2d + 8);
  if (!dst)
    return;
  int w = *(int *)((char *)btn + 52);
  int h = *(int *)((char *)btn + 48);
  SDL_Surface *src = sw_ensure_select_surf(w, h);
  if (!src)
    return;
  SDL_Rect dr = {x, y, src->w, src->h};
  SDL_UpperBlit(src, NULL, dst, &dr);
}

static int (*commButton_draw_orig)(void *this, int x, int y);
static int (*commButton_drawText_orig)(void *this, int a2, int a3,
                                       const char *a4, void *color);
static int (*commButton_isclick_orig)(void *this);

static int j_commButton_draw(void *this, int x, int y) {
  char *btn = this;
  int newui = sw_is_newui_button(this);
  if (g_extra_sel && !sw_in_fight()) {
    g_extra_sel = SW_EXTRA_NONE;
    g_extra_click = 0;
  } else if (g_extra_sel) {
    int *sel = sw_menu_select();
    if (sel && *sel)
      *sel = 0;
  }
  unsigned char saved = 0;
  if (newui && btn) {
    saved = (unsigned char)btn[SW_BTN_CLICKED];
    btn[SW_BTN_CLICKED] = 0;
  }
  int ret = commButton_draw_orig(this, x, y);
  if (newui && btn) {
    btn[SW_BTN_CLICKED] = saved;
    if (sw_btn_is_selected(this))
      sw_blit_select_frame(this, x, y);
  }
  return ret;
}

/* PSV: 新 UI 按钮禁用 clicked 文字色，避免 hover 把字刷成确认色。 */
static int j_commButton_drawText(void *this, int a2, int a3, const char *a4,
                                 void *color) {
  if (sw_is_newui_button(this))
    return commButton_drawText_orig(this, a2, a3, a4, NULL);
  return commButton_drawText_orig(this, a2, a3, a4, color);
}

/* TakeKey 里自动/逃跑只认 IsClick（鼠标落在按钮上）。方向键选中后由这里补一次命中。 */
static int j_commButton_isclick(void *this) {
  if (g_extra_click && sw_is_extra_button(this) && sw_btn_is_selected(this)) {
    g_extra_click = 0;
    g_extra_sel = SW_EXTRA_NONE;
    return 1;
  }
  return commButton_isclick_orig(this);
}

static void *sw_make_draw_tramp(uintptr_t func) {
  void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return NULL;
  uint32_t *t = p;
  memcpy(t, (void *)func, 16);
  t[4] = 0x58000051u;
  t[5] = 0xd61f0220u;
  *(uint64_t *)(t + 6) = func + 16;
  __builtin___clear_cache((char *)p, (char *)p + 32);
  return p;
}

static void sw_hook_commbutton_draw(void) {
  uintptr_t draw = so_find_addr_safe("_ZN15commButtonClass4drawEii");
  uintptr_t text = so_find_addr_safe(
      "_ZN15commButtonClass14drawButtonTextEiiPKcP9SDL_Color");
  if (!draw) {
    debugPrintf("[patch] commButtonClass::draw not found\n");
    return;
  }
  commButton_draw_orig = sw_make_draw_tramp(draw);
  if (!commButton_draw_orig) {
    debugPrintf("[patch] commButtonClass::draw tramp mmap failed\n");
    return;
  }
  hook_arm64(draw, (uintptr_t)j_commButton_draw);
  debugPrintf("[patch] commButtonClass::draw -> select-box (PSV style)\n");
  if (text) {
    commButton_drawText_orig = sw_make_draw_tramp(text);
    if (commButton_drawText_orig) {
      hook_arm64(text, (uintptr_t)j_commButton_drawText);
      debugPrintf("[patch] commButtonClass::drawButtonText -> no clicked color\n");
    }
  }
  {
    uintptr_t click = so_find_addr_safe("_ZN15commButtonClass7IsClickEv");
    if (click) {
      commButton_isclick_orig = sw_make_draw_tramp(click);
      if (commButton_isclick_orig) {
        hook_arm64(click, (uintptr_t)j_commButton_isclick);
        debugPrintf("[patch] commButtonClass::IsClick -> extra-row confirm\n");
      }
    }
  }
}

static void sw_hook_uigamepad(void) {
  static const char *const syms[] = {
      "_ZN9UIGamePad4initEv",
      "_ZN9UIGamePad6UpdateEP12SDL_Renderer",
      "_ZN9UIGamePad7SetModeEi",
      "_ZN9UIGamePad17CheckStateReleaseEiii",
      "_ZN9UIGamePad6MotionEiii",
      "_ZN9UIGamePadC2Ev",
      "_ZN9UIGamePad5CloseEv",
  };
  int i;
  for (i = 0; i < (int)(sizeof(syms) / sizeof(syms[0])); i++) {
    uintptr_t a = so_find_addr_safe(syms[i]);
    if (a) {
      hook_arm64(a, (uintptr_t)&ret0);
      debugPrintf("[patch] %s -> ret0\n", syms[i]);
    }
  }
}

/* 原始端口 B=右键。战斗返回是 2023 so 的 BACK_KEY_CLICK，两套不能叠在同一下。 */
static int sw_in_fight(void) {
  unsigned char *f = sw_sym("inFight");
  return f && *f;
}

static int sw_force_submenu(void) {
  unsigned char *a = sw_sym("ForceSpellMenu");
  unsigned char *b = sw_sym("ForceCommandMenu");
  unsigned char *c = sw_sym("ForceObsoltMenu");
  unsigned char *d = sw_sym("ForceChAttrMenu");
  return (a && *a == 1) || (b && *b == 1) || (c && *c == 1) || (d && *d == 1);
}

static int sw_btn_item_id(const char *btn) {
  return btn ? *(int *)(btn + SW_BTN_ITEM_ID) : 0;
}

static int sw_scan_has_item(const char *base, int n, int id) {
  int i;
  if (!base || id <= 0)
    return 0;
  for (i = 0; i < n; i++) {
    const char *b = base + i * SW_CMD_BTN_STRIDE;
    if (sw_btn_enabled(b) && sw_btn_item_id(b) == id)
      return 1;
  }
  return 0;
}

static int sw_cmd_has_item(int id) {
  return sw_scan_has_item(sw_cmd_buttons(), SW_N_MAIN_CMD, id) ||
         sw_scan_has_item(sw_sym("ExCommandButton"), SW_N_EX_CMD, id);
}

static int sw_extra_row_visible(void) {
  char *a = sw_auto_btn();
  char *r = sw_retreat_btn();
  /* +44=disable_draw；有 surface 说明 FightMenu 已 init、这一帧会画。 */
  return (a && !a[SW_BTN_DISABLED] && *(void **)a) ||
         (r && !r[SW_BTN_DISABLED] && *(void **)r);
}

/* 主指令条可见、且底下自动/逃跑也画出来了。炼妖/符鬼子菜单时这两颗不画。 */
static int sw_cmd_extra_active(void) {
  return sw_in_fight() && !sw_force_submenu() && sw_extra_row_visible();
}

static int sw_on_last_cmd_col(void) {
  int *sel = sw_menu_select();
  int id = (sel && *sel > 0) ? *sel : 1;
  return !sw_cmd_has_item(id + 3);
}

static int sw_extra_ok(const char *btn) {
  return btn && !btn[SW_BTN_DISABLED] && *(void **)btn;
}

static void sw_enter_extra(void) {
  int *sel = sw_menu_select();
  int id = (sel && *sel > 0) ? *sel : 1;
  int col = (id - 1) % 3;
  g_extra_saved_sel = id;
  g_extra_sel = (col >= 2) ? SW_EXTRA_RETREAT : SW_EXTRA_AUTO;
  if (g_extra_sel == SW_EXTRA_AUTO && !sw_extra_ok(sw_auto_btn()))
    g_extra_sel = SW_EXTRA_RETREAT;
  if (g_extra_sel == SW_EXTRA_RETREAT && !sw_extra_ok(sw_retreat_btn()))
    g_extra_sel = SW_EXTRA_AUTO;
  if (sel)
    *sel = 0;
}

static void sw_leave_extra(void) {
  int *sel = sw_menu_select();
  if (sel && g_extra_saved_sel > 0)
    *sel = g_extra_saved_sel;
  g_extra_sel = SW_EXTRA_NONE;
  g_extra_click = 0;
}

static int sw_handle_extra_pad(SDL_Event *ev) {
  int down, btn;

  if (!sw_cmd_extra_active()) {
    if (g_extra_sel)
      sw_leave_extra();
    return 0;
  }
  if (ev->type != SDL_CONTROLLERBUTTONDOWN &&
      ev->type != SDL_CONTROLLERBUTTONUP)
    return 0;

  down = ev->type == SDL_CONTROLLERBUTTONDOWN;
  btn = ev->cbutton.button;

  if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
    if (down && !g_extra_sel && sw_on_last_cmd_col()) {
      sw_enter_extra();
      ev->cbutton.button = (Uint8)-1;
      return 1;
    }
    if (g_extra_sel) {
      ev->cbutton.button = (Uint8)-1;
      return 1;
    }
    return 0;
  }
  if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP) {
    if (g_extra_sel) {
      if (down)
        sw_leave_extra();
      ev->cbutton.button = (Uint8)-1;
      return 1;
    }
    return 0;
  }
  if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
      btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
    if (!g_extra_sel)
      return 0;
    if (down) {
      if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        g_extra_sel = sw_extra_ok(sw_auto_btn()) ? SW_EXTRA_AUTO
                                                : SW_EXTRA_RETREAT;
      else
        g_extra_sel = sw_extra_ok(sw_retreat_btn()) ? SW_EXTRA_RETREAT
                                                   : SW_EXTRA_AUTO;
    }
    ev->cbutton.button = (Uint8)-1;
    return 1;
  }
  if (btn == SDL_CONTROLLER_BUTTON_A) {
    if (!g_extra_sel)
      return 0;
    if (down)
      g_extra_click = 1;
    ev->cbutton.button = (Uint8)-1;
    return 1;
  }
  return 0;
}

/* 系统子菜单换人：只拦 L1/R1（游戏默认 key 25/26）。方向左右不拦。 */
static int sw_in_menu_system(void) {
  int *p = (int *)sw_sym("inMenuSystem");
  return p && *p;
}

static void sw_menu_man_key(int key, int down) {
  void *din = sw_sym("DINPUT");
  void (*upd)(void *, int, int);

  upd = (void (*)(void *, int, int))sw_sym(
      "_ZN8SDLINPUT15UpdateKeyStatusEib");
  if (din && upd)
    upd(din, key, down);
}

static int sw_handle_menu_man(SDL_Event *ev) {
  int down, btn;

  if (!sw_in_menu_system())
    return 0;
  if (ev->type != SDL_CONTROLLERBUTTONDOWN &&
      ev->type != SDL_CONTROLLERBUTTONUP)
    return 0;
  down = ev->type == SDL_CONTROLLERBUTTONDOWN;
  btn = ev->cbutton.button;
  if (btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
    sw_menu_man_key(25, down);
    ev->cbutton.button = (Uint8)-1;
    return 1;
  }
  if (btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
    sw_menu_man_key(26, down);
    ev->cbutton.button = (Uint8)-1;
    return 1;
  }
  return 0;
}

static void sw_push_right_click(int down) {
  SDL_Event e;
  SDL_Window *w;

  memset(&e, 0, sizeof(e));
  e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  e.button.button = SDL_BUTTON_RIGHT;
  e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  e.button.clicks = 1;
  e.button.x = 320;
  e.button.y = 240;
  w = egl_shim_get_window();
  if (w)
    e.button.windowID = SDL_GetWindowID(w);
  SDL_PushEvent(&e);
}

int sw_SDL_PollEvent(SDL_Event *ev) {
  int r = SDL_PollEvent(ev);
  if (r > 0 &&
      (ev->type == SDL_CONTROLLERBUTTONDOWN ||
       ev->type == SDL_CONTROLLERBUTTONUP)) {
    if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
      int down = ev->type == SDL_CONTROLLERBUTTONDOWN;
      if (sw_in_fight() && g_extra_sel) {
        if (down)
          sw_leave_extra();
      } else if (sw_in_fight()) {
        if (down) {
          unsigned char *bk = sw_sym("BACK_KEY_CLICK");
          if (bk)
            *bk = 1;
        }
      } else {
        /* 与原始端口相同：B 按下=右键按下，松开=右键松开，不能同一帧脉冲。 */
        sw_push_right_click(down);
      }
      ev->cbutton.button = (Uint8)-1;
    } else if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
      ev->cbutton.button = (Uint8)-1;
    } else if (!sw_handle_menu_man(ev)) {
      sw_handle_extra_pad(ev);
    }
  }
  egl_shim_tls_restore();
  return r;
}

static void *sw_input_thread(void *arg) {
  (void)arg;
  SDL_GameController *pad = NULL;
  for (;;) {
    SDL_GameControllerUpdate();
    if (!pad) {
      int n = SDL_NumJoysticks();
      for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
          pad = SDL_GameControllerOpen(i);
          if (pad) {
            debugPrintf("[input] pad opened (idx=%d) %s\n", i,
                        SDL_GameControllerName(pad));
            break;
          }
        }
      }
    }
    if (pad) {
      int start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
      int back = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
      if (start && back) {
        static const char msg[] = "[pad] SELECT+START -> exit\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(0);
      }
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
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
               SDL_INIT_GAMECONTROLLER) != 0)
    fatal_error("device SDL_Init(VIDEO|AUDIO|JOYSTICK|GAMECONTROLLER) failed: %s",
                SDL_GetError());
  SDL_JoystickEventState(SDL_ENABLE);
  SDL_GameControllerEventState(SDL_ENABLE);
  egl_shim_create_window();
  {
    SDL_Window *w = egl_shim_get_window();
    if (w) {
      SDL_ShowWindow(w);
      SDL_RaiseWindow(w);
      SDL_SetWindowInputFocus(w);
    }
  }
  debugPrintf("Screen: %dx%d (via device SDL2) audio=%s\n",
              egl_shim_screen_w, egl_shim_screen_h,
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
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
    void *h = NULL;
    /* Android 版 libSDL2.so 在本机缺 OpenSLES/android，且要 glFramebufferTexture2DOES，
     * E5Plus 上 dlopen 必失败；失败还会按 soname 污染后续设备版回退。
     * 核心 SDL2 固定走设备带版本 soname（与启动脚本 /tmp/sword3libs 软链一致）。 */
    if (strcmp(name, "libSDL2.so") == 0) {
      h = dlopen("/usr/lib/libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
      if (!h)
        h = dlopen("libSDL2.so", RTLD_NOW | RTLD_GLOBAL);
      if (h)
        debugPrintf("  loaded secondary: libSDL2.so (device)\n");
    } else {
      snprintf(path, sizeof(path), "%s/%s", basedir, name);
      h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
      if (!h) {
        /* Android 版常因 LIBC 版本失败，且会污染裸 soname。改走设备带版本路径。 */
        const char *alt = NULL;
        if (strcmp(name, "libSDL2_image.so") == 0)
          alt = "/usr/lib/libSDL2_image-2.0.so.0";
        else if (strcmp(name, "libSDL2_mixer.so") == 0)
          alt = "/usr/lib/libSDL2_mixer-2.0.so.0";
        else if (strcmp(name, "libSDL2_ttf.so") == 0)
          alt = "/usr/lib/libSDL2_ttf-2.0.so.0";
        if (alt)
          h = dlopen(alt, RTLD_NOW | RTLD_GLOBAL);
        if (!h)
          h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      }
      if (h)
        debugPrintf("  loaded secondary: %s\n", name);
    }
    if (!h)
      debugPrintf("  [warn] dlopen %s falhou: %s (非致命，若启动报缺符号再排查)\n",
                  name, dlerror());
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
  debugPrintf("[build] %s %s (A=confirm B=hold-right / fight-back)\n",
              __DATE__, __TIME__);

  /* 1) 设备侧 SDL2 初始化窗口：egl_shim 自动选后端（fbdev/kmsdrm/wayland），
   *    不给设备 SDL2 设 SDL_VIDEODRIVER（铁律）。 */
  setup_device_sdl_video();

  /* 后台线程只做 SELECT+START 退出。手柄事件交给 2023 版 libSWD3E.so。 */
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
  load_device_gles();
  load_secondary_libs(basedir);

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
  /* 开场/过场：原 GetVideoPath 拼 $GAMEDIR/Video/<file>，片源在 assets/Video/。 */
  sw_hook_game_func("_ZN6fileIO12GetVideoPathEPKc", "fileIO_GetVideoPath");
  /* RoleDataBase / PathDataBase / StoryDataBase：OpenDataFiles() 走 GetACTPath
   * （maps.dat / path.dat / talk1.dat），不是 GetResourcePath。原函数拼
   * $GAMEDIR/<name>，找不到再走 JNI APKX；JNI 全空 → 返回空路径 → fopen("")
   * → "RoleDataBase init Failed." → SIGSEGV。 */
  sw_hook_game_func("_ZN6fileIO10GetACTPathEPKc", "fileIO_GetACTPath");
  sw_hook_game_func("_Z22GetAndroidFileIsExistsPKc", "GetAndroidFileIsExists");
  sw_hook_game_func("_Z20Android_APKX_SetFilePKcS0_", "Android_APKX_SetFile");
  sw_hook_game_func("_Z14GetAPKXFileLenv", "GetAPKXFileLenv");
  sw_hook_game_func("_Z17GetAPKXFileOffsetv", "GetAPKXFileOffsetv");
  sw_hook_commbutton_draw();
  sw_hook_uigamepad();

  /* 2023 SDL_SS2D::Init 结尾用 tpidr+0x28 做 canary。glibc 上该槽会被 Mali/PNG
   * 改掉 → 误走 "Couldn't create window" 并和第二次 canary 检查死循环刷屏。
   * 把两处条件跳转改成成功返回，不换 Android SDL2。 */
  {
    uintptr_t init = so_find_addr_safe("_ZN8SDL_SS2D4InitEPKcj");
    if (init) {
      uint32_t *eq = (uint32_t *)(init + 0x15b8); /* 89fa4: b.eq success */
      uint32_t *ne = (uint32_t *)(init + 0x1608); /* 89ff4: b.ne fail-loop */
      if (*eq == 0x540002a0u && *ne == 0x54fffda1u) {
        *eq = 0x14000015u; /* b success */
        *ne = 0xd503201fu; /* nop */
        debugPrintf("[patch] SDL_SS2D::Init skip false stack_chk\n");
      } else {
        debugPrintf("[patch] SDL_SS2D::Init canary site mismatch %08x %08x\n",
                    *eq, *ne);
      }
    }
  }

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
  /* Init 入口前钉死 bionic canary。2023 SO 的 SDL_SS2D::Init 结尾会校验
   * tpidr+0x28；Mali/PNG 会改掉这个槽，误走 "Couldn't create window" 分支刷屏。 */
  egl_shim_tls_pin();
  char *main_argv[] = {"sword3", NULL};
  int rc = p_SDL_main(1, main_argv);
  debugPrintf("SDL_main returned %d\n", rc);

  return 0;
}
