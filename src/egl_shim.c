#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "egl_shim.h"
#include "util.h"

/* egl_shim_win_*   = 真窗口 / drawable（SDL 测面板，不写死设备分辨率）
 * egl_shim_screen_* = 报给游戏的逻辑分辨率（引擎原生 640×480 4:3）
 * GetWindowSize / DisplayMode / ANativeWindow 一律走 screen_*，再靠
 * SDL_RenderSetLogicalSize 等比例放大到 win_*（16:9 左右黑边，4:3 铺满）。 */
int egl_shim_screen_w = 0, egl_shim_screen_h = 0;
int egl_shim_win_w = 0, egl_shim_win_h = 0;
#define SCREEN_WIDTH egl_shim_screen_w
#define SCREEN_HEIGHT egl_shim_screen_h

extern int g_summertime_screen_w, g_summertime_screen_h;

static void sync_reported_geometry(int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  egl_shim_screen_w = w;
  egl_shim_screen_h = h;
  g_summertime_screen_w = w;
  g_summertime_screen_h = h;
}

static int query_display_size(int *w, int *h) {
  SDL_DisplayMode dm;
  int dw = 0, dh = 0;
  if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    dw = dm.w;
    dh = dm.h;
    debugPrintf("egl_shim: current display %dx%d\n", dw, dh);
  }
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    debugPrintf("egl_shim: desktop mode %dx%d\n", dm.w, dm.h);
    if (!dw) {
      dw = dm.w;
      dh = dm.h;
    }
  }
  if (dw <= 0 || dh <= 0)
    return 0;
  *w = dw;
  *h = dh;
  return 1;
}

/* A engine (bionic) lê a stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD).
 * Sob glibc esse offset colide com uma TLS var que o Mali/SDL escreve no
 * MakeCurrent/CreateContext -> a canary "muda" no meio da função -> stack smash
 * FALSO-POSITIVO. 进程内钉死一份 canary，SDL/GL 返回后写回去。 */
static unsigned long g_tls_canary;
static int g_tls_canary_set;

void egl_shim_tls_pin(void) {
  unsigned long tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  g_tls_canary = *(unsigned long *)(tp + 0x28);
  g_tls_canary_set = 1;
}

void egl_shim_tls_restore(void) {
  unsigned long tp;
  if (!g_tls_canary_set)
    return;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  *(unsigned long *)(tp + 0x28) = g_tls_canary;
}

static int gl_makecurrent(SDL_Window *w, SDL_GLContext c) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  int (*f)(SDL_Window *, SDL_GLContext) = &SDL_GL_MakeCurrent;
  int r = f(w, c);
  *(unsigned long *)(tp + 0x28) = g_tls_canary_set ? g_tls_canary : g;
  return r;
}
static SDL_GLContext gl_createcontext(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GLContext (*f)(SDL_Window *) = &SDL_GL_CreateContext;
  SDL_GLContext c = f(w);
  *(unsigned long *)(tp + 0x28) = g_tls_canary_set ? g_tls_canary : g;
  return c;
}

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int swapint_applied;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;

static _egl_context *current_context = NULL;
static _egl_context *last_context = NULL;
static int has_real_gl = 0;

extern void summertime_gl_debug_frame(void);
extern void *summertime_gl_lookup(const char *name);
/* sword3 是纯 SDL2 2D 游戏，无自定义光标/GL 调试需求：光标位置置 0（渲染层
 * draw_port_cursor 仍会画一个角标，启动脚本设 SUMMERTIME_CURSOR=0 关闭它）。 */
volatile float summertime_cursor_x = 0, summertime_cursor_y = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

static int egl_window_alive(void) {
  return egl_window && SDL_GetWindowID(egl_window) != 0;
}

/* 游戏会再调 SDL_CreateWindow / SDL_Quit。真 SDL_Quit 会从 SDL 内部拆掉窗口
 * （不走游戏 PLT 的 DestroyWindow），留下悬空 egl_window → GetWindowSize=0x0。 */
SDL_Window *egl_shim_SDL_CreateWindow(const char *title, int x, int y,
                                      int w, int h, Uint32 flags) {
  (void)title; (void)x; (void)y; (void)flags;
  if (!egl_window_alive()) {
    debugPrintf("egl_shim: window dead (game asked %dx%d), recreating %dx%d\n",
                w, h, egl_shim_screen_w, egl_shim_screen_h);
    egl_window = NULL;
    if (!SDL_WasInit(SDL_INIT_VIDEO))
      SDL_InitSubSystem(SDL_INIT_VIDEO);
    egl_shim_create_window();
  } else {
    debugPrintf("egl_shim: reuse window (game asked %dx%d, logical %dx%d)\n",
                w, h, egl_shim_screen_w, egl_shim_screen_h);
  }
  egl_shim_tls_restore();
  return egl_window;
}

void egl_shim_SDL_DestroyWindow(SDL_Window *w) {
  if (w && w == egl_window) {
    debugPrintf("egl_shim: ignore DestroyWindow on port window\n");
    return;
  }
  SDL_DestroyWindow(w);
}

void egl_shim_SDL_Quit(void) {
  debugPrintf("egl_shim: ignore SDL_Quit (keep video/window)\n");
}

int egl_shim_SDL_Init(Uint32 flags) {
  debugPrintf("egl_shim: SDL_Init(0x%x) already up\n", (unsigned)flags);
  if (flags & SDL_INIT_AUDIO) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
      debugPrintf("egl_shim: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
  }
  if (flags & SDL_INIT_JOYSTICK) {
    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
      SDL_InitSubSystem(SDL_INIT_JOYSTICK);
  }
  if (flags & SDL_INIT_GAMECONTROLLER) {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
      SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
  }
  return 0;
}

void egl_shim_SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
  (void)w;
  if (width) *width = egl_shim_screen_w;
  if (height) *height = egl_shim_screen_h;
  egl_shim_tls_restore();
}

static void fill_device_mode(SDL_DisplayMode *mode) {
  if (!mode)
    return;
  if (!mode->format)
    mode->format = SDL_PIXELFORMAT_RGB888;
  if (!mode->refresh_rate)
    mode->refresh_rate = 60;
  if (egl_shim_screen_w > 0 && egl_shim_screen_h > 0) {
    mode->w = egl_shim_screen_w;
    mode->h = egl_shim_screen_h;
  }
}

int egl_shim_SDL_GetCurrentDisplayMode(int displayIndex, void *mode) {
  SDL_DisplayMode *m = (SDL_DisplayMode *)mode;
  int r = SDL_GetCurrentDisplayMode(displayIndex, m);
  if (r != 0 && m)
    memset(m, 0, sizeof(*m));
  fill_device_mode(m);
  {
    static int logged;
    if (!logged++ && m)
      debugPrintf("egl_shim: GetCurrentDisplayMode -> %dx%d (logical)\n",
                  m->w, m->h);
  }
  return 0;
}

int egl_shim_SDL_GetDesktopDisplayMode(int displayIndex, void *mode) {
  SDL_DisplayMode *m = (SDL_DisplayMode *)mode;
  int r = SDL_GetDesktopDisplayMode(displayIndex, m);
  if (r != 0 && m)
    memset(m, 0, sizeof(*m));
  fill_device_mode(m);
  return 0;
}

int egl_shim_SDL_GetDisplayMode(int displayIndex, int modeIndex, void *mode) {
  SDL_DisplayMode *m = (SDL_DisplayMode *)mode;
  int r = SDL_GetDisplayMode(displayIndex, modeIndex, m);
  if (r != 0 && m)
    memset(m, 0, sizeof(*m));
  fill_device_mode(m);
  return 0;
}

int egl_shim_SDL_GetDisplayBounds(int displayIndex, void *rect) {
  SDL_Rect *r = (SDL_Rect *)rect;
  (void)SDL_GetDisplayBounds(displayIndex, r);
  if (r && egl_shim_screen_w > 0 && egl_shim_screen_h > 0) {
    r->x = 0;
    r->y = 0;
    r->w = egl_shim_screen_w;
    r->h = egl_shim_screen_h;
  }
  return 0;
}

void *egl_shim_SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags) {
  if (!w) w = egl_window;
  SDL_Renderer *r = SDL_CreateRenderer(w, index, flags);
  if (r && egl_shim_screen_w > 0 && egl_shim_screen_h > 0) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(r, egl_shim_screen_w, egl_shim_screen_h);
    debugPrintf("egl_shim: CreateRenderer logical=%dx%d on drawable %dx%d\n",
                egl_shim_screen_w, egl_shim_screen_h, egl_shim_win_w, egl_shim_win_h);
  } else if (!r) {
    debugPrintf("egl_shim: CreateRenderer FAILED: %s\n", SDL_GetError());
  }
  return r;
}

int egl_shim_SDL_RenderSetLogicalSize(void *renderer, int w, int h) {
  int use_w = egl_shim_screen_w > 0 ? egl_shim_screen_w : w;
  int use_h = egl_shim_screen_h > 0 ? egl_shim_screen_h : h;
  if (w != use_w || h != use_h)
    debugPrintf("egl_shim: RenderSetLogicalSize %dx%d -> logical %dx%d\n",
                w, h, use_w, use_h);
  return SDL_RenderSetLogicalSize((SDL_Renderer *)renderer, use_w, use_h);
}

/* ReanderUpdate：Clear → RenderCopy(bg, dst=NULL) → RenderCopy(game, render_rect)。
 * LogicalSize 把 dst=NULL 也锁在 4:3 里，Background.png 铺不到两侧，只剩黑边。
 * 无 dest 的 copy 临时关掉 logical size，让遮罩铺满真窗口。 */
int egl_shim_SDL_RenderCopy(void *renderer, void *texture,
                            const void *srcrect, const void *dstrect) {
  SDL_Renderer *r = (SDL_Renderer *)renderer;
  const SDL_Rect *src = (const SDL_Rect *)srcrect;
  const SDL_Rect *dst = (const SDL_Rect *)dstrect;
  if (!dst && egl_shim_win_w > 0 && egl_shim_screen_w > 0 &&
      (egl_shim_win_w != egl_shim_screen_w ||
       egl_shim_win_h != egl_shim_screen_h)) {
    SDL_RenderSetLogicalSize(r, 0, 0);
    int ret = SDL_RenderCopy(r, (SDL_Texture *)texture, src, NULL);
    SDL_RenderSetLogicalSize(r, egl_shim_screen_w, egl_shim_screen_h);
    return ret;
  }
  return SDL_RenderCopy(r, (SDL_Texture *)texture, src, dst);
}

void egl_shim_map_logical_viewport(int *x, int *y, int *w, int *h) {
  int dw = egl_shim_win_w, dh = egl_shim_win_h;
  int lw = egl_shim_screen_w, lh = egl_shim_screen_h;
  if (egl_window)
    SDL_GL_GetDrawableSize(egl_window, &dw, &dh);
  if (lw <= 0 || lh <= 0 || dw <= 0 || dh <= 0)
    return;
  float sx = (float)dw / (float)lw;
  float sy = (float)dh / (float)lh;
  float s = sx < sy ? sx : sy;
  int vw = (int)((float)lw * s + 0.5f);
  int vh = (int)((float)lh * s + 0.5f);
  int ox = (dw - vw) / 2;
  int oy = (dh - vh) / 2;
  int ix = x ? *x : 0, iy = y ? *y : 0, iw = w ? *w : lw, ih = h ? *h : lh;
  if (x) *x = ox + (int)((float)ix * s + 0.5f);
  if (y) *y = oy + (int)((float)iy * s + 0.5f);
  if (w) *w = (int)((float)iw * s + 0.5f);
  if (h) *h = (int)((float)ih * s + 0.5f);
}

int egl_shim_SDL_SetWindowFullscreen(SDL_Window *w, Uint32 flags) {
  (void)w; (void)flags;
  debugPrintf("egl_shim: ignore SetWindowFullscreen(0x%x)\n", (unsigned)flags);
  return 0;
}

int egl_shim_SDL_SetWindowDisplayMode(SDL_Window *w, const void *mode) {
  (void)w;
  const SDL_DisplayMode *dm = (const SDL_DisplayMode *)mode;
  if (dm)
    debugPrintf("egl_shim: ignore SetWindowDisplayMode %dx%d\n", dm->w, dm->h);
  return 0;
}

int egl_shim_SDL_VideoInit(const char *name) {
  debugPrintf("egl_shim: ignore SDL_VideoInit('%s')\n", name ? name : "(null)");
  return 0;
}

int egl_shim_SDL_AudioInit(const char *name) {
  if (SDL_WasInit(SDL_INIT_AUDIO)) {
    debugPrintf("egl_shim: SDL_AudioInit('%s') already %s\n",
                name ? name : "(null)",
                SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return 0;
  }
  int r = SDL_AudioInit(name);
  debugPrintf("egl_shim: SDL_AudioInit('%s') -> %s r=%d err=%s\n",
              name ? name : "(null)",
              SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
              r, r ? SDL_GetError() : "-");
  return r;
}

void egl_shim_SDL_AudioQuit(void) {
  debugPrintf("egl_shim: ignore SDL_AudioQuit\n");
}

static void parse_logical_res(int *lw, int *lh) {
  /* 引擎原生 640×480（4:3）。SWORD3_RES 只改逻辑分辨率，不改真窗口。 */
  *lw = 640;
  *lh = 480;
  const char *e = getenv("SWORD3_RES");
  if (!e || !*e) e = getenv("SWORD3_LOGICAL");
  if (!e || !*e) e = getenv("SUMMERTIME_RES");
  int w, h;
  if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
    *lw = w;
    *lh = h;
    debugPrintf("egl_shim: logical override %dx%d\n", w, h);
  }
}

void egl_shim_create_window(void) {
  int disp_w = 0, disp_h = 0;
  if (!query_display_size(&disp_w, &disp_h))
    debugPrintf("egl_shim: SDL display query failed, wait for CreateWindow fallback\n");
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  /* contexto ES casa com o SUMMERTIME_GLVER passado pro engine (default 2.0);
   * em GPU ES3 real (Mali-G310) GLVER=3.0 -> contexto ES 3.0 de verdade */
  { const char *gv = getenv("SUMMERTIME_GLVER");
    int major = (gv && gv[0] == '3') ? 3 : 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    if (major == 3) debugPrintf("egl_shim: pedindo contexto ES 3.0\n"); }
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  /* 真窗口跟当前面板走 FULLSCREEN_DESKTOP，尺寸以 SDL 测到的为准。 */
  {
    int win_w = (disp_w > 0) ? disp_w : 640;
    int win_h = (disp_h > 0) ? disp_h : 480;
    egl_window = SDL_CreateWindow(
        PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
  }
  if (!egl_window && disp_w > 0 && disp_h > 0) {
    debugPrintf("egl_shim: desktop fullscreen failed (%s), try %dx%d FULLSCREEN\n",
                SDL_GetError(), disp_w, disp_h);
    egl_window = SDL_CreateWindow(
        PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        disp_w, disp_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
  }
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  {
    int aw = 0, ah = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(egl_window, &aw, &ah);
    SDL_GL_GetDrawableSize(egl_window, &dw, &dh);
    egl_shim_win_w = dw > 0 ? dw : aw;
    egl_shim_win_h = dh > 0 ? dh : ah;
    if (egl_shim_win_w <= 0) egl_shim_win_w = disp_w;
    if (egl_shim_win_h <= 0) egl_shim_win_h = disp_h;
    {
      int lw = 640, lh = 480;
      parse_logical_res(&lw, &lh);
      sync_reported_geometry(lw, lh);
    }
    debugPrintf("egl_shim: Window created logical %dx%d  SDL %dx%d  drawable %dx%d  display %dx%d\n",
                egl_shim_screen_w, egl_shim_screen_h, aw, ah, dw, dh, disp_w, disp_h);
  }

  egl_share_root = gl_createcontext(egl_window);
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL share-root context created\n");
  /* SUMMERTIME_SWAPINT no contexto novo (a engine pode nunca chamar
   * eglSwapInterval; default SDL=vsync 1 + limiter da engine = 30fps). */
  {
    const char *f = getenv("SUMMERTIME_SWAPINT");
    if (f) {
      SDL_GL_SetSwapInterval(atoi(f));
      debugPrintf("egl_shim: swap interval forçado=%d\n", atoi(f));
    }
  }

  gl_makecurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret = gl_makecurrent(egl_window, ctx->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    SDL_GL_DeleteContext(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;

  pthread_mutex_lock(&egl_context_create_mutex);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root)
    gl_makecurrent(egl_window, egl_share_root);
  c->sdl_context = gl_createcontext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  gl_makecurrent(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (egl_window) {
      gl_makecurrent(egl_window, NULL);
      /* debugPrintf("egl_shim: GL released [tid=%lx] reason=eglMakeCurrent(NULL)\n",
                    (unsigned long)pthread_self()); */
    }
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  int ret = gl_makecurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    /* SUMMERTIME_SWAPINT: intervalo é estado por-contexto; aplica 1x em cada */
    {
      static const char *si = (const char *)-1;
      if (si == (const char *)-1) si = getenv("SUMMERTIME_SWAPINT");
      if (si && !context->swapint_applied) {
        context->swapint_applied = 1;
        SDL_GL_SetSwapInterval(atoi(si));
      }
    }
    static int acq_log = 0;
    if (acq_log < 20 || mc % 500 == 0) {
      //debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] ACQUIRED [ctx_id=%d]\n",
      //            mc, is_window ? "WINDOW" : "PBUFFER",
      //            (unsigned long)pthread_self(), context->id);
      acq_log++;
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

/* screenshot sob demanda (receita Bully): `touch /dev/shm/dys_shot` ->
 * RGBA cru do backbuffer em /dev/shm/dys_shot.raw + .txt WxH (flip vertical
 * na conversao). Roda na thread de render, custo zero sem o trigger. */
static void dys_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/dys_shot", F_OK) != 0) return;
  unlink("/dev/shm/dys_shot");
  GLint vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/dys_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/dys_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  debugPrintf("[shot] %dx%d salvo\n", w, h);
}

static void cursor_clear_rect(int x, int y, int w, int h, int max_w, int max_h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > max_w) w = max_w - x;
  if (y + h > max_h) h = max_h - y;
  if (w <= 0 || h <= 0)
    return;
  glScissor(x, y, w, h);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_port_cursor(void) {
  const char *cursor = getenv("SUMMERTIME_CURSOR");
  if (cursor && strcmp(cursor, "0") == 0)
    return;

  int dw = egl_shim_win_w, dh = egl_shim_win_h;
  if (egl_window)
    SDL_GL_GetDrawableSize(egl_window, &dw, &dh);
  if (dw <= 0 || dh <= 0)
    return;
  int vx = 0, vy = 0, vw = egl_shim_screen_w, vh = egl_shim_screen_h;
  egl_shim_map_logical_viewport(&vx, &vy, &vw, &vh);
  int cx = vx + (int)(summertime_cursor_x * (float)vw);
  int cy = vy + (int)(summertime_cursor_y * (float)vh);
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;
  if (cx >= dw) cx = dw - 1;
  if (cy >= dh) cy = dh - 1;

  glViewport(0, 0, dw, dh);
  glEnable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  int gy = dh - cy;
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  cursor_clear_rect(cx - 9, gy - 1, 19, 3, dw, dh);
  cursor_clear_rect(cx - 1, gy - 9, 3, 19, dw, dh);

  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  cursor_clear_rect(cx - 7, gy, 15, 1, dw, dh);
  cursor_clear_rect(cx, gy - 7, 1, 15, dw, dh);

  glDisable(GL_SCISSOR_TEST);
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    if (getenv("SUMMERTIME_GLDBG"))
      summertime_gl_debug_frame();
    draw_port_cursor();
    dys_maybe_screenshot();
    SDL_GL_SwapWindow(egl_window);
    /* [PERF] frame-time entre swaps; relatório a cada ~5s (diagnóstico do lag;
     * custo: 1 clock_gettime/frame + 1 fprintf/5s). */
    {
      static struct timespec last = {0, 0};
      static double sum = 0, mx = 0;
      static unsigned n = 0, s20 = 0, s40 = 0;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (last.tv_sec) {
        double ms = (now.tv_sec - last.tv_sec) * 1e3 +
                    (now.tv_nsec - last.tv_nsec) / 1e6;
        sum += ms; n++;
        if (ms > mx) mx = ms;
        if (ms > 20) s20++;
        if (ms > 40) s40++;
        if (getenv("SUMMERTIME_PERF") && sum >= 5000) {
          fprintf(stderr, "[PERF] fps=%.1f avg=%.1fms max=%.0fms >20ms=%u >40ms=%u\n",
                  n * 1000.0 / sum, sum / n, mx, s20, s40);
          sum = 0; n = 0; mx = 0; s20 = 0; s40 = 0;
        } else if (sum >= 5000) {
          sum = 0; n = 0; mx = 0; s20 = 0; s40 = 0;
        }
      }
      last = now;
    }
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      //debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
      //            fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  debugPrintf("egl_shim: eglGetConfigAttrib(attr=0x%x)\n", attribute);
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 8; break;
  case 0x3021: *value = 8; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 0; break;
  case 0x3025: *value = 24; break;
  case 0x3026: *value = 8; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = summertime_gl_lookup(procname);
  if (ptr) return ptr;

  ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = summertime_gl_lookup(stripped);
      if (ptr) return ptr;
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "NextOS";      /* EGL_VENDOR */
  case 0x3054: return "1.4 NextOS";  /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  /* SUMMERTIME_SWAPINT força o intervalo (teste do double-pacing: engine dorme
   * ~16ms + vsync = 2 períodos = trava em 30fps; =0 deixa a engine ditar). */
  const char *f = getenv("SUMMERTIME_SWAPINT");
  if (f) interval = atoi(f);
  debugPrintf("egl_shim: SwapInterval(%d)%s\n", (int)interval, f ? " [forçado]" : "");
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)"window";
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
