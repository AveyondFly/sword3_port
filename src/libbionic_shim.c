/*
 * libbionic_shim.c — 在 glibc 设备上顶替 Android/bionic 特有符号。
 *
 * 解决的问题：
 *   1) __sF  —— bionic 的 stdin/stdout/stderr 数组（glibc 没有，只有 _IO_2_1_*）。
 *              libc++_shared（NDK C++ 运行时）的 std::cout/cerr 静态初始化会引用
 *              &__sF[1]/&__sF[2]。导出为版本 LIBC 以满足其重定位。
 *   2) Android_JNI_* —— 游戏资源加载经 Android 文件 API（原由 Android 版 libSDL2.so
 *              提供，换设备侧 SDL2 后消失）。这里用 glibc stdio 直接实现。
 *   3) SMPEG_new_rwops —— 过场走随包 libsmpeg2.so（op.bik 实为 MPEG-1）。
 *              这里只包一层：跳过 nil.bik 一类过小片源，其余转发真 SMPEG。
 *              其余 SMPEG_* 由 so_resolve 的 dlsym 落到 libsmpeg2，不再做“立刻 STOPPED”桩。
 *
 * 注意：bionic 与 glibc 的 FILE 布局不同。__sF 仅在“按下标取地址后传给 glibc 的
 * fprintf/fwrite”场景下被使用，且 Sword3 这类游戏基本不向 std::cout/cerr 输出，
 * 故用 3 个真实 glibc 标准流对象顶替通常足够让 dlopen 通过并正常运行。
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>   /* stat() for APKX file length */
#include <signal.h>
#include <execinfo.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <iconv.h>
#include <stdint.h>
#include <png.h>
#include <ucontext.h>   /* ucontext_t / mcontext_t / REG_* (aarch64 glibc) */

static const char *sw_gamedir(void);
static int sw_file_exists(const char *path);
static const char *sw_find_data_file(const char *name);
static void shim_note_audio_path(const char *path);
static char g_apkx_current[4096];
SDL_Surface *IMG_LoadPNG_RW(SDL_RWops *src, int freesrc);

/* 资源路径过滤：仅对“资源相关”路径打印诊断，避免刷屏。 */
static int shim_filt(const char *p) {
    if (!p || !*p) return 1;
    return strstr(p, "Resource") || strstr(p, ".png") || strstr(p, ".bmp") ||
           strstr(p, ".jpg") || strstr(p, ".dat") || strstr(p, ".txt") ||
           strstr(p, "asset") || strstr(p, "Asset") || strstr(p, "SWD3E");
}

/* 注意：故意【不】拦截 IMG_Load_RW / SDL_RWFromMem / SDL_RWFromFP / SDL_LoadBMP_RW。
 * 但【必须】拦截 fopen / open：游戏自带的 Android 版 libSDL2.so 在 RTLD_GLOBAL 下导出了
 * 它自己的 fopen/open（bionic 兼容包装）。游戏 libSDL2_image 经 PLT 调用 fopen 时，动态
 * 链接器把 GOT 填成游戏 libSDL2 的坏 fopen（在 glibc 上返回伪造/固定的 FILE* → 读不到真
 * 数据 → libpng 报 unknown chunk → 空指针解引用崩溃）。故在此用全局符号遮挡 fopen/open，
 * 但【不】用 dlsym(RTLD_NEXT)（会解析回游戏坏版），而是 dlopen 真 libc 取真实实现转发，
 * 保证游戏所有 fopen/open 都走 glibc 真函数。IMG_* 因走内存 RWops / 真实 SDL_RWFromFile
 * （其内部 fopen 已被本拦截修正）而自然可用，无需单独拦截。 */

/* 转发 fopen/open 到 libc 真版。
 * 用 dlsym(RTLD_NEXT)：从 shim 跳过自身后，下一个定义 fopen/open 的是 libc
 * （游戏 libSDL2.so 经 readelf 确认不导出 fopen/open，故 RTLD_NEXT 直接命中 libc 真版）。
 * 若用 dlopen("libc.so.6") 在本环境会返回 NULL（ROCKNIX glibc 行为），故不采用。 */
static int shim_is_png(const char *p) {
    if (!p) return 0;
    const char *dot = strrchr(p, '.');
    return dot && (strcasecmp(dot, ".png") == 0);
}

static int shim_is_ttf(const char *p) {
    if (!p) return 0;
    const char *dot = strrchr(p, '.');
    return dot && (strcasecmp(dot, ".ttf") == 0);
}

/* 诊断日志开关：png/.ttf/Resource/.dat/.bmp/.jpg 都记录 open/fopen，用于定位
 * CT.ttf 字体加载崩溃（之前只记录 png，导致字体路径完全不可见）。 */
static int shim_want_log(const char *p) {
    if (!p) return 0;
    return shim_is_png(p) || strstr(p, ".ttf") || strstr(p, "Resource") ||
           strstr(p, ".dat") || strstr(p, ".bmp") || strstr(p, ".jpg");
}

/* 登记游戏打开的 png FILE*，供 fread 诊断使用（仅小集合，环形覆盖） */
#define SHIM_PNG_FP_MAX 64
static FILE *g_png_fp[SHIM_PNG_FP_MAX];
static int g_png_fp_n = 0;
static char g_last_png_path[4096];   /* fopen/open 拦截器记录的最近一次 png 路径，供 IMG_LoadPNG_RW 回退重开 */
static char g_last_ttf_path[4096];   /* fopen/open 拦截器记录的最近一次 ttf 路径，供 TTF_OpenFontRW 回退重开 */
static const char *g_ttf_enc = "BIG5";  /* 文字编码：SDL_RWFromFile 打开字体时按文件名推断
                                           CT.ttf/CHT->BIG5(繁体) / CS.ttf/CHS->GBK(简体)。
                                           GBK 与 BIG5 字节区间重叠但映射不同，转码必须匹配字体实际模式，
                                           否则会"解码成功却出乱码"。本端口游戏默认繁体，故默认 BIG5。 */
static int shim_fp_is_png(FILE *f) {
    for (int i = 0; i < g_png_fp_n; i++)
        if (g_png_fp[i] == f) return 1;
    return 0;
}

/* === 文件打开全量日志（RoleDataBase init 诊断）===
 * 把每次 fopen/open/open64/openat/SDL_RWFromFile/APKX/Android_JNI_FileOpen 的
 * 打开事件写入环形缓冲（最近 OPEN_EVT_MAX 条）。RoleDataBase init 失败后游戏会立即
 * SIGSEGV，崩溃处理器（shim_segv_handler）随后转储该缓冲——即"RoleDataBase init
 * 打印前后"实际打开过的每个文件及其返回值（NULL/-1 即打开失败，是重点排查对象）。
 * 缓冲单线程写入（加载/初始化期），崩溃时由 handler 只读转储，信号安全。 */
#define OPEN_EVT_MAX 2048
#define OPEN_EVT_LEN 256
static char   g_open_evt[OPEN_EVT_MAX][OPEN_EVT_LEN];
static int    g_open_evt_head = 0;   /* 下一个写入槽 */
static int    g_open_evt_count = 0;  /* 累计条数（序列号） */
static int    g_role_window = 0;     /* 预留：检测到 RoleDataBase init 后置 1 */
static int    g_dump_open = 0;       /* SHIM_DUMP_OPEN=1 时，每次打开实时 fprintf 落地（绕开缓冲双副本问题） */

static void shim_open_push(const char *line) {
    char *slot = g_open_evt[g_open_evt_head];
    int n = snprintf(slot, OPEN_EVT_LEN, "[op#%d] %s",
                     g_open_evt_count, line ? line : "");
    if (n < 0) slot[0] = '\0';
    slot[OPEN_EVT_LEN - 1] = '\0';
    g_open_evt_head = (g_open_evt_head + 1) % OPEN_EVT_MAX;
    g_open_evt_count++;
    if (g_role_window)
        fprintf(stderr, "[ROLEWIN]%s\n", slot);
    /* SHIM_DUMP_OPEN=1：每次打开实时落地，绕开环形缓冲双副本（预载.so vs 主二进制嵌入）问题 */
    if (g_dump_open)
        fprintf(stderr, "[shim:OPEN] %s\n", slot);
}

static void shim_dump_open_ring(const char *why) {
    int n = (g_open_evt_count < OPEN_EVT_MAX) ? g_open_evt_count : OPEN_EVT_MAX;
    int start = (g_open_evt_head - n + OPEN_EVT_MAX) % OPEN_EVT_MAX;
    fprintf(stderr, "[shim:OPEN-DUMP] === %s : 最近文件打开（旧->新，末尾即崩溃前） 共 %d 条 ===\n",
            why ? why : "", n);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % OPEN_EVT_MAX;
        fprintf(stderr, "%s\n", g_open_evt[idx]);
    }
    fprintf(stderr, "[shim:OPEN-DUMP] === end ===\n");
}

/* === 帧指针回溯（fp-walk）：独立可复用诊断函数 ===
 * 从给定帧指针出发，沿帧指针链表（aarch64: x29 指向 [saved_fp, saved_lr]）向上逐帧
 * 取返回地址，对每帧用 dladdr 解析模块+偏移，打印完整调用链。
 *   - 在钩子函数体内传 __builtin_frame_address(0)：拿到钩子自身帧，回溯会向上包含
 *     调用者（fopen → fCreateFile → RoleDataBase ...），符合预期。
 *   - 在崩溃处理器传 ucontext 的 x29（故障帧）：回溯从故障函数的调用者开始。
 * 打印格式保持 [shim:FPWALK] fpNN ... 风格，便于日志 grep / 与静态反汇编偏移对照。
 * 命中非 16 字节对齐 / 无效返回地址 / 非向上推进帧时终止，避免在坏栈上无限回溯。 */
#define FPWALK_MAX 48
static void shim_fpwalk(void *start_fp) {
    if (!start_fp) return;
    fprintf(stderr, "[shim:FPWALK] from cfa=%p:\n", start_fp);
    Dl_info di;
    void *cfa = start_fp;
    for (int i = 0; i < FPWALK_MAX; i++) {
        if (!cfa || ((uintptr_t)cfa & 0xf) != 0) break;   /* 必须 16 字节对齐 */
        unsigned long *f = (unsigned long *)cfa;
        void *saved_fp = (void *)f[0];
        void *ret_addr = (void *)f[1];
        if (!ret_addr) break;
        if (dladdr(ret_addr, &di) && di.dli_fbase) {
            fprintf(stderr, "[shim:FPWALK] fp%-2d %p -> ret %p  %s +0x%llx\n", i, cfa, ret_addr,
                    di.dli_fname,
                    (unsigned long long)((char *)ret_addr - (char *)di.dli_fbase));
        } else {
            fprintf(stderr, "[shim:FPWALK] fp%-2d %p -> ret %p (no-symbol)\n", i, cfa, ret_addr);
        }
        if (saved_fp <= cfa) break;   /* 不是向上推进的有效帧 */
        cfa = saved_fp;
    }
}

/* 空路径打开（fopen("")/open("")）是 RoleDataBase init 失败的直接症状：游戏从某个
 * 未初始化/未填充的结构取到空文件名。此处趁栈完好打印调用链（非信号上下文，安全），
 * 定位究竟是哪一层（引擎/RoleDataBase/资源解析）传了空路径。 */
static void shim_report_empty(const char *api) {
    void *frs[32];
    int nn = backtrace(frs, 32);
    fprintf(stderr, "[shim:EMPTY-PATH] %s('') called -> 后续必 FAIL; backtrace:\n",
            api ? api : "?");
    backtrace_symbols_fd(frs, nn, 2);
    /* 完整调用链（fp-walk）：看清空路径来自哪一层
     * （libSWD3E+0x142110 → sword3(+0x189f4)[fCreateFile] → fopen("")）。 */
    fprintf(stderr, "[shim:EMPTY-PATH] full call chain (fp-walk):\n");
    shim_fpwalk(__builtin_frame_address(0));
}

static const char *shim_resolve_open_path(const char *path) {
    if (!path || !*path) {
        if (g_apkx_current[0] && sw_file_exists(g_apkx_current))
            return g_apkx_current;
        const char *db = sw_find_data_file("StringDB.txt");
        if (db && sw_file_exists(db))
            return db;
        return path;
    }
    /* 绝对路径也要 remap：游戏 fCreateFile("/tmp/s3/Music/1a-04.mp3")，
     * 文件实际在 assets/Music/。原先绝对路径原样返回 → 存在性检查过了、打开失败。 */
    if (path[0] == '/' && sw_file_exists(path))
        return path;
    const char *found = sw_find_data_file(path);
    if (found && sw_file_exists(found))
        return found;
    /* 资源包缺 FBack/BackIconClick，用已有 BackIcon 顶上，否则战斗只有确认没有返回。 */
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (!strcmp(base, "BackIconClick.png") ||
            !strcmp(base, "FBackIcon.png") ||
            !strcmp(base, "FBackIconClick.png")) {
            found = sw_find_data_file("BackIcon.png");
            if (found && sw_file_exists(found))
                return found;
        }
    }
    return path;
}

static FILE *(*real_fopen)(const char *, const char *) = NULL;
FILE *fopen(const char *path, const char *mode) {
    if (!real_fopen) {
        real_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
        if (!real_fopen)
            fprintf(stderr, "[shim] real_fopen NULL dlerr=%s\n",
                    dlerror() ? dlerror() : "?");
    }
    const char *resolved = shim_resolve_open_path(path);
    if (resolved && path && resolved != path && strcmp(resolved, path) != 0)
        fprintf(stderr, "[shim:fopen] remap '%s' -> '%s'\n", path, resolved);
    path = resolved;
    shim_note_audio_path(path);
    FILE *f = real_fopen ? real_fopen(path, mode) : NULL;
    if (f && strstr(path, ".ttf")) {  /* 直接 fopen 字体的备用路径也推断编码 */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (strstr(base, "CS") || strstr(base, "CHS"))
            g_ttf_enc = "GBK";
        else if (strstr(base, "CT") || strstr(base, "CHT"))
            g_ttf_enc = "BIG5";
    }
    int is_png = shim_is_png(path);
    if (is_png && f) {
        if (g_png_fp_n < SHIM_PNG_FP_MAX) g_png_fp[g_png_fp_n++] = f;
        if (path) {
            strncpy(g_last_png_path, path, sizeof(g_last_png_path) - 1);
            g_last_png_path[sizeof(g_last_png_path) - 1] = '\0';
        }
    }
    if (shim_is_ttf(path) && f && path) {
        strncpy(g_last_ttf_path, path, sizeof(g_last_ttf_path) - 1);
        g_last_ttf_path[sizeof(g_last_ttf_path) - 1] = '\0';
    }
    if (g_dump_open) {
        fprintf(stderr, "[shim:fopen] '%s' -> %p%s\n", path ? path : "(null)",
                (void *)f, f ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    } else if (shim_want_log(path))
        fprintf(stderr, "[shim:fopen] '%s' -> %p%s\n", path ? path : "(null)",
                (void *)f, f ? "" : " (FAIL)");
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "fopen '%s' -> %p%s",
        path ? path : "(null)", (void *)f, f ? "" : " FAIL"); shim_open_push(_b); }
    if (!path || !*path) shim_report_empty("fopen");
    return f;
}

static int (*real_open)(const char *, int, ...) = NULL;
int open(const char *path, int flags, ...) {
    if (!real_open) {
        real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
        if (!real_open)
            fprintf(stderr, "[shim] real_open NULL dlerr=%s\n",
                    dlerror() ? dlerror() : "?");
    }
    if (!real_open) return -1;
    {
        const char *resolved = shim_resolve_open_path(path);
        if (resolved && path && resolved != path && strcmp(resolved, path) != 0 &&
            (!(flags & O_CREAT) || sw_file_exists(resolved))) {
            fprintf(stderr, "[shim:open] remap '%s' -> '%s'\n", path, resolved);
            path = resolved;
        }
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    int fd = real_open(path, flags, mode);
    int is_png = shim_is_png(path);
    if (is_png) {
        if (path) {
            strncpy(g_last_png_path, path, sizeof(g_last_png_path) - 1);
            g_last_png_path[sizeof(g_last_png_path) - 1] = '\0';
        }
    }
    if (shim_is_ttf(path) && path) {
        strncpy(g_last_ttf_path, path, sizeof(g_last_ttf_path) - 1);
        g_last_ttf_path[sizeof(g_last_ttf_path) - 1] = '\0';
    }
    if (g_dump_open) {
        fprintf(stderr, "[shim:open] '%s' -> fd=%d%s\n", path ? path : "(null)",
                fd, fd >= 0 ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    } else if (shim_want_log(path))
        fprintf(stderr, "[shim:open] '%s' -> fd=%d%s\n", path ? path : "(null)",
                fd, fd >= 0 ? "" : " (FAIL)");
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "open '%s' -> fd=%d%s",
        path ? path : "(null)", fd, fd >= 0 ? "" : " FAIL"); shim_open_push(_b); }
    if (!path || !*path) shim_report_empty("open");
    return fd;
}

/* 64 位文件接口拦截器：游戏在 aarch64 glibc 下常以 open64/fopen64 打开资源，
 * 若只拦截 open/fopen 会漏掉 png 路径记录，导致 fallback 重开无路径可用、解码失败。
 * 二者与 open/fopen 同逻辑，仅符号名不同。 */
static int (*real_open64)(const char *, int, ...) = NULL;
int open64(const char *path, int flags, ...) {
    if (!real_open64) {
        real_open64 = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open64");
        if (!real_open64 && real_open) real_open64 = real_open;
    }
    if (!real_open64) return -1;
    {
        const char *resolved = shim_resolve_open_path(path);
        if (resolved && path && resolved != path && strcmp(resolved, path) != 0 &&
            (!(flags & O_CREAT) || sw_file_exists(resolved))) {
            fprintf(stderr, "[shim:open64] remap '%s' -> '%s'\n", path, resolved);
            path = resolved;
        }
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    int fd = real_open64(path, flags, mode);
    int is_png = shim_is_png(path);
    if (is_png) {
        if (path) {
            strncpy(g_last_png_path, path, sizeof(g_last_png_path) - 1);
            g_last_png_path[sizeof(g_last_png_path) - 1] = '\0';
        }
    }
    if (shim_is_ttf(path) && path) {
        strncpy(g_last_ttf_path, path, sizeof(g_last_ttf_path) - 1);
        g_last_ttf_path[sizeof(g_last_ttf_path) - 1] = '\0';
    }
    if (g_dump_open) {
        fprintf(stderr, "[shim:open64] '%s' -> fd=%d%s\n", path ? path : "(null)",
                fd, fd >= 0 ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    } else if (shim_want_log(path))
        fprintf(stderr, "[shim:open64] '%s' -> fd=%d%s\n", path ? path : "(null)",
                fd, fd >= 0 ? "" : " (FAIL)");
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "open64 '%s' -> fd=%d%s",
        path ? path : "(null)", fd, fd >= 0 ? "" : " FAIL"); shim_open_push(_b); }
    if (!path || !*path) shim_report_empty("open64");
    return fd;
}

static FILE *(*real_fopen64)(const char *, const char *) = NULL;
FILE *fopen64(const char *path, const char *mode) {
    if (!real_fopen64)
        real_fopen64 = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen64");
    path = shim_resolve_open_path(path);
    shim_note_audio_path(path);
    FILE *f = real_fopen64 ? real_fopen64(path, mode) : NULL;
    int is_png = shim_is_png(path);
    if (is_png && f) {
        if (g_png_fp_n < SHIM_PNG_FP_MAX) g_png_fp[g_png_fp_n++] = f;
        if (path) {
            strncpy(g_last_png_path, path, sizeof(g_last_png_path) - 1);
            g_last_png_path[sizeof(g_last_png_path) - 1] = '\0';
        }
    }
    if (shim_is_ttf(path) && f && path) {
        strncpy(g_last_ttf_path, path, sizeof(g_last_ttf_path) - 1);
        g_last_ttf_path[sizeof(g_last_ttf_path) - 1] = '\0';
    }
    if (g_dump_open) {
        fprintf(stderr, "[shim:fopen64] '%s' -> %p%s\n", path ? path : "(null)",
                (void *)f, f ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    } else if (shim_want_log(path))
        fprintf(stderr, "[shim:fopen64] '%s' -> %p%s\n", path ? path : "(null)",
                (void *)f, f ? "" : " (FAIL)");
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "fopen64 '%s' -> %p%s",
        path ? path : "(null)", (void *)f, f ? "" : " FAIL"); shim_open_push(_b); }
    if (!path || !*path) shim_report_empty("fopen64");
    return f;
}

/* openat 拦截：glibc 下部分代码库直接调用 openat（而非 open），原 shim 未覆盖会漏记。
 * 转发到 libc 真 openat 并登记到打开环形缓冲。 */
static int (*real_openat)(int, const char *, int, ...) = NULL;
int openat(int dirfd, const char *path, int flags, ...) {
    if (!real_openat)
        real_openat = (int (*)(int, const char *, int, ...))dlsym(RTLD_NEXT, "openat");
    if (!real_openat) return -1;
    va_list ap; va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    int fd = real_openat(dirfd, path, flags, mode);
    if (g_dump_open) {
        fprintf(stderr, "[shim:openat] '%s' -> fd=%d%s\n", path ? path : "(null)",
                fd, fd >= 0 ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    }
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "openat '%s' -> fd=%d%s",
        path ? path : "(null)", fd, fd >= 0 ? "" : " FAIL"); shim_open_push(_b); }
    if (!path || !*path) shim_report_empty("openat");
    return fd;
}

/* === NULL 安全 stdio 守卫（v15）===
 * 游戏自带的文件抽象层（fSetFilePointer 等）对 fopen 返回的 NULL 句柄不做判空，
 * 直接 fseek/fread/ftell/fwrite/rewind/fclose 之 → NULL 解引用崩溃。首个触发点是
 * Setting/env2.dat 缺失（fopen 返回 NULL → fSetFilePointer(NULL) → libc fseek(NULL)
 * SIGSEGV，返回地址 libSWD3E.so+0x1428c4，见 v14 崩溃 backtrace）。
 * 这些函数在 ISO C 下本就不接受 NULL stream，故统一加守卫：NULL 时返回安全的
 * 失败/零值，绝不解引用。仅对 NULL 生效，对合法句柄完全透传，零副作用。
 * 这样端口即便再遇到其他缺失的只读文件也不会崩，只会读到空数据→走默认逻辑。 */
static int (*real_fseek)(FILE *, long, int) = NULL;
int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return 0;  /* 游戏 fSetFilePointer 检查 cmp w0,#0，0=成功，安全 */
    if (!real_fseek)
        real_fseek = (int (*)(FILE *, long, int))dlsym(RTLD_NEXT, "fseek");
    return real_fseek ? real_fseek(stream, offset, whence) : -1;
}

static long (*real_ftell)(FILE *) = NULL;
long ftell(FILE *stream) {
    if (!stream) return -1L;
    if (!real_ftell)
        real_ftell = (long (*)(FILE *))dlsym(RTLD_NEXT, "ftell");
    return real_ftell ? real_ftell(stream) : -1L;
}

static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *) = NULL;
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) return 0;
    if (!real_fwrite)
        real_fwrite = (size_t (*)(const void *, size_t, size_t, FILE *))dlsym(RTLD_NEXT, "fwrite");
    return real_fwrite ? real_fwrite(ptr, size, nmemb, stream) : 0;
}

static void (*real_rewind)(FILE *) = NULL;
void rewind(FILE *stream) {
    if (!stream) return;
    if (!real_rewind)
        real_rewind = (void (*)(FILE *))dlsym(RTLD_NEXT, "rewind");
    if (real_rewind) real_rewind(stream);
}

static int (*real_fclose)(FILE *) = NULL;
int fclose(FILE *stream) {
    if (!stream) return 0;
    if (!real_fclose)
        real_fclose = (int (*)(FILE *))dlsym(RTLD_NEXT, "fclose");
    return real_fclose ? real_fclose(stream) : 0;
}

/* Bug1 修复（open 的版本化）：
 * 既有 open interpose 保持 unversioned（供游戏库的 unversioned open 调用，
 * 其内部 dlsym(RTLD_NEXT,"open") 转发到 glibc 真 open，无递归）。
 * libc++_shared 需要的 open@LIBC 由 src/libc_compat_shim.c 里的 shim_open
 * （.symver shim_open, open@LIBC）单独提供——它转发到 glibc 真 open64，
 * 与 interpose 同名但版本标签不同，互不冲突。
 * 注意：此处绝不能对 interpose open 用 .symver，否则同一 .o 内 unversioned open
 * 与版本化 open 同名，链接报 "multiple definition of open"。 */

/* fread 诊断：仅当读取目标 FILE* 是我们登记的 png 文件且本次读取量较大时，
 * 打印实际读到的字节数，确认游戏是否真的从磁盘读到了完整 PNG 数据。 */
static size_t (*real_fread)(void *, size_t, size_t, FILE *) = NULL;
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream) return 0;  /* NULL 安全：缺失文件句柄返回 0 字节，游戏走默认逻辑 */
    if (!real_fread)
        real_fread = (size_t (*)(void *, size_t, size_t, FILE *))dlsym(RTLD_NEXT, "fread");
    size_t got = real_fread ? real_fread(ptr, size, nmemb, stream) : 0;
    size_t want = size * nmemb;
    if (shim_fp_is_png(stream) && want >= 1024)
        fprintf(stderr, "[shim:fread] png want=%zu got=%zu%s\n",
                want, got, (got == 0 && want > 0) ? " (EMPTY!)" : "");
    return got;
}

/* SDL_RWFromFile 拦截：游戏自带的 Android 版 libSDL2.so 的 SDL_RWFromFile 在 glibc
 * 上无法正确从文件系统读取（假设 Android asset 环境），导致 IMG_Load(filename) 拿到的
 * 数据损坏 → libpng 报 unknown chunk。故拦截后，用【真 libc】fopen/fread 自行读取文件
 * 全文，再用游戏自带的 SDL_RWFromFP 包装一个 libc FILE*（FILE* 包装只读 fread/seek，
 * 在 glibc 上安全，且 autoclose 由它负责 fclose，无内存泄漏）。完全不依赖 dlopen。 */
static SDL_RWops *(*real_SDL_RWFromFP)(FILE *, SDL_bool) = NULL;
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    if (!file) return NULL;
    if (!real_fopen)
        real_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    const char *resolved = shim_resolve_open_path(file);
    if (resolved && file && resolved != file && strcmp(resolved, file) != 0)
        fprintf(stderr, "[shim:RW] remap '%s' -> '%s'\n", file, resolved);
    file = resolved;
    FILE *f = real_fopen ? real_fopen(file, mode ? mode : "rb") : NULL;
    if (!f) {
        fprintf(stderr, "[shim:RW] fopen FAIL '%s'\n", file);
        { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b,
            "SDL_RWFromFile '%s' -> NULL (fopen FAIL)", file ? file : "(null)");
          shim_open_push(_b); }
        return NULL;
    }
    if (!real_SDL_RWFromFP)
        real_SDL_RWFromFP =
            (SDL_RWops *(*)(FILE *, SDL_bool))dlsym(RTLD_NEXT, "SDL_RWFromFP");
    SDL_RWops *rw = real_SDL_RWFromFP ? real_SDL_RWFromFP(f, SDL_TRUE) : NULL;
    if (!rw) {
        fclose(f);
        fprintf(stderr, "[shim:RW] SDL_RWFromFP NULL '%s'\n", file);
        { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b,
            "SDL_RWFromFile '%s' -> NULL (RWFromFP NULL)", file ? file : "(null)");
          shim_open_push(_b); }
    } else {
        const char *tag = (strstr(file, ".ttf") ? "ttf" : "file");
        if (tag[0] == 't') {  /* .ttf 字体：按文件名推断文字编码 */
            const char *base = strrchr(file, '/');
            base = base ? base + 1 : file;
            if (strstr(base, "CS") || strstr(base, "CHS"))
                g_ttf_enc = "GBK";        /* 简体 */
            else if (strstr(base, "CT") || strstr(base, "CHT"))
                g_ttf_enc = "BIG5";       /* 繁体 */
        }
        fprintf(stderr, "[shim:RW:%s] '%s' -> OK (enc=%s)\n", tag, file, g_ttf_enc);
        { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b,
            "SDL_RWFromFile '%s' -> OK (enc=%s)", file ? file : "(null)", g_ttf_enc);
          shim_open_push(_b); }
    }
    return rw;
}

/* IMG_Load 诊断：游戏 SDL_SS2D_LoadImage 传什么 filename、文件是否可读、系统版
 * IMG_Load 返回什么。转发到系统 libSDL2_image 的真实 IMG_Load（dlsym RTLD_NEXT）。 */
static SDL_Surface *(*real_IMG_Load)(const char *) = NULL;
SDL_Surface *IMG_Load(const char *file) {
    if (!real_IMG_Load)
        real_IMG_Load = (SDL_Surface *(*)(const char *))dlsym(RTLD_NEXT, "IMG_Load");
    int ex = (file && access(file, R_OK) == 0) ? 1 : 0;
    fprintf(stderr, "[shim:IMG_Load] '%s' exists=%d\n", file ? file : "(null)", ex);
    SDL_Surface *s = real_IMG_Load ? real_IMG_Load(file) : NULL;
    fprintf(stderr, "[shim:IMG_Load] -> %p%s\n", (void *)s, s ? "" : " (FAIL)");
    return s;
}

/* IMG_LoadPNG_RW 修复：
 * 游戏自构造的 FILE-backed RWops 交给系统 libSDL2_image 解码时静默失败——
 * 实机诊断证明数据 100% 合法（magic=89504e47 的 PNG + 正确 size，332 次 PNG-OK），
 * 但 real 仍返回 NULL。根因是游戏 RWops 的 seek 回调在完整 PNG 解码过程中失效
 * （或系统库未默认导出 IMG_LoadPNG_RW）。无论哪种，统一改为：把 RWops 从当前
 * 位置 slurp 到 EOF 进内存（不 seek-to-end，避免游戏 RWops 的 SEEK_END 回调崩溃），
 * 用天生可 seek 的 SDL_RWFromMem 调真正的 IMG_LoadPNG_RW 解码器（叶子函数，不递归）。 */
/* 注意：必须解析到真正的 PNG 叶子解码器 IMG_LoadPNG_RW，绝不能解析 IMG_Load_RW
 * （调度器）。IMG_Load_RW 识别到 PNG 后会再调 IMG_LoadPNG_RW —— 若这里解析到调度器，
 * 就会无限递归到本 shim 自身，导致加载阶段卡死（海量重入、无崩溃）。
 * 解析策略：先 dlsym(RTLD_NEXT)；若随包 libSDL2_image.so 以 RTLD_LOCAL 加载（其符号不在
 * 全局查找域，RTLD_NEXT 不可见），则显式 dlopen("libSDL2_image.so")（LD_LIBRARY_PATH 含
 * $GAMEDIR，必能命中随包/系统那份，两者都导出 IMG_LoadPNG_RW）取其真解码器句柄。 */
static SDL_Surface *(*real_IMG_LoadPNG_RW)(SDL_RWops *, int) = NULL;
static SDL_Surface *(*real_IMG_LoadTyped_RW)(SDL_RWops *, int, const char *) = NULL;
static int g_decoder_ready = 0;
static int g_png_ok_n, g_png_fail_n;

/* 本进程里 sword3 与 libbionic_shim.so 各有一份 IMG_LoadPNG_RW。
 * dlsym(RTLD_NEXT) 会命中另一份 shim 而不是真正的解码器，必须跳过自身地址。 */
static void *png_dlsym_leaf(void *h, const char *sym) {
    if (!h) return NULL;
    void *p = dlsym(h, sym);
    if (!p || p == (void *)IMG_LoadPNG_RW) return NULL;
    return p;
}

static void *png_try_open(const char *path) {
    void *h = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen(path, RTLD_LAZY);
    return h;
}

static SDL_Surface *shim_png_from_mem(const unsigned char *data, size_t len) {
    if (!data || len < 8 || png_sig_cmp((png_const_bytep)data, 0, 8) != 0)
        return NULL;
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, len))
        return NULL;
    img.format = PNG_FORMAT_RGBA;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(
        0, (int)img.width, (int)img.height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) {
        png_image_free(&img);
        return NULL;
    }
    if (!png_image_finish_read(&img, NULL, s->pixels, 0, NULL)) {
        SDL_FreeSurface(s);
        png_image_free(&img);
        return NULL;
    }
    png_image_free(&img);
    return s;
}

static int g_png_reenter;
static SDL_Surface *shim_png_decode_buf(const unsigned char *data, size_t len) {
    if (!data || !len) return NULL;
    if (g_png_reenter)
        return shim_png_from_mem(data, len);
    SDL_Surface *s = NULL;
    g_png_reenter = 1;
    if (real_IMG_LoadPNG_RW) {
        SDL_RWops *mem = SDL_RWFromMem((void *)data, (int)len);
        if (mem) s = real_IMG_LoadPNG_RW(mem, 1);
    }
    if (!s && real_IMG_LoadTyped_RW) {
        SDL_RWops *mem = SDL_RWFromMem((void *)data, (int)len);
        if (mem) s = real_IMG_LoadTyped_RW(mem, 1, "PNG");
    }
    g_png_reenter = 0;
    if (!s)
        s = shim_png_from_mem(data, len);
    return s;
}

static void shim_resolve_png_decoder(void) {
    if (g_decoder_ready) return;
    g_decoder_ready = 1;
    const char *cands[] = {
        "/usr/lib/libSDL2_image-2.0.so.0",
        "libSDL2_image-2.0.so.0",
        "libSDL2_image.so.android",
        "libSDL2_image.so",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        void *h = png_try_open(cands[i]);
        if (!h) continue;
        if (!real_IMG_LoadPNG_RW)
            real_IMG_LoadPNG_RW =
                (SDL_Surface *(*)(SDL_RWops *, int))png_dlsym_leaf(h, "IMG_LoadPNG_RW");
        if (!real_IMG_LoadTyped_RW)
            real_IMG_LoadTyped_RW =
                (SDL_Surface *(*)(SDL_RWops *, int, const char *))png_dlsym_leaf(h, "IMG_LoadTyped_RW");
        if (real_IMG_LoadPNG_RW || real_IMG_LoadTyped_RW)
            break;
    }
    fprintf(stderr, "[shim:PNG_RW] decoder png=%s typed=%s libpng=yes\n",
            real_IMG_LoadPNG_RW ? "yes" : "NO",
            real_IMG_LoadTyped_RW ? "yes" : "NO");
}
static SDL_Surface *shim_png_from_path(const char *path) {
    if (!path || !path[0])
        return NULL;
    if (!real_fopen)
        real_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    if (!real_fopen)
        return NULL;
    FILE *ff = real_fopen(path, "rb");
    if (!ff)
        return NULL;
    if (fseek(ff, 0, SEEK_END) != 0) {
        fclose(ff);
        return NULL;
    }
    long sz = ftell(ff);
    if (sz <= 0) {
        fclose(ff);
        return NULL;
    }
    if (fseek(ff, 0, SEEK_SET) != 0) {
        fclose(ff);
        return NULL;
    }
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    if (!b) {
        fclose(ff);
        return NULL;
    }
    size_t got = fread(b, 1, (size_t)sz, ff);
    fclose(ff);
    SDL_Surface *s = (got == (size_t)sz) ? shim_png_decode_buf(b, got) : NULL;
    free(b);
    return s;
}

SDL_Surface *IMG_LoadPNG_RW(SDL_RWops *src, int freesrc) {
    shim_resolve_png_decoder();
    if (!src) return NULL;

    /* 游戏 RWops 的 read 在短读（最后一块 < 64KB）时返回 0，dpad 等 >64KB 的图
     * 会被截成正好 65536 字节。优先按 fopen 记下的路径整读。内存 RWops 除外。 */
    if (g_last_png_path[0] && src->type != 4 && src->type != 5) {
        SDL_Surface *from_file = shim_png_from_path(g_last_png_path);
        if (from_file) {
            g_png_ok_n++;
            if (g_png_ok_n + g_png_fail_n <= 3)
                fprintf(stderr, "[shim:PNG_RW] file '%s' OK\n", g_last_png_path);
            return from_file;
        }
    }

    /* Bug2 修复（稳健版）：游戏在调用 IMG_LoadPNG_RW 前已把 RWops 读空停在 EOF，
     * 且该 RWops 的 seek 回调在 glibc 上失效（SDL_RWseek 返回 -1）。单纯 seek 复位
     * 无效。策略：先尽力 seek+slurp；若 slurp 得不到数据，则用 fopen/open 拦截器记录的
     * "最近一次打开的 png 路径" 通过 glibc real_fopen 重新整读并解码，彻底绕开坏 RWops。 */
    (void)SDL_RWseek(src, 0, RW_SEEK_SET);

    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { if (freesrc) SDL_RWclose(src); return NULL; }
    for (;;) {
        if (len + (1 << 16) > cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap);
            if (!nb) { free(buf); if (freesrc) SDL_RWclose(src); return NULL; }
            buf = nb;
        }
        Sint64 n = SDL_RWread(src, buf + len, 1, 1 << 16);
        if (n <= 0) break;
        len += (size_t)n;
    }

    SDL_Surface *s = shim_png_decode_buf(buf, len);
    free(buf);

    /* 回退：slurp 为空（RWops 已读空且不可 seek）时，用记录的 png 路径重新打开整读。
     * 仅对文件型 RWops 生效；内存 RWops(type 4/5) 用 stale 路径会解码错文件，跳过。 */
    if (!s && g_last_png_path[0] && real_fopen &&
        src->type != 4 && src->type != 5) {
        FILE *ff = real_fopen(g_last_png_path, "rb");
        if (ff) {
            size_t c2 = 1 << 16, l2 = 0;
            unsigned char *b2 = (unsigned char *)malloc(c2);
            if (b2) {
                for (;;) {
                    if (l2 + (1 << 16) > c2) {
                        c2 *= 2;
                        unsigned char *nb2 = (unsigned char *)realloc(b2, c2);
                        if (!nb2) { free(b2); b2 = NULL; break; }
                        b2 = nb2;
                    }
                    Sint64 r = (Sint64)fread(b2 + l2, 1, 1 << 16, ff);
                    if (r <= 0) break;
                    l2 += (size_t)r;
                }
                fclose(ff);
                if (b2 && l2 > 0)
                    s = shim_png_decode_buf(b2, l2);
                free(b2);
            } else {
                fclose(ff);
            }
        }
    }

    /* 关键：src 的所有权始终交还游戏，本 shim 绝不关闭 src。
     * 经验证该游戏在 IMG_LoadPNG_RW 返回后无论成功与否都会自行 SDL_RWclose(src)
     * （不遵守 freesrc 契约）；若我们在此关闭，成功路径会触发 double free。 */
    if (s) g_png_ok_n++;
    else g_png_fail_n++;
    if (!s || g_png_ok_n + g_png_fail_n <= 3 || (g_png_fail_n <= 6 && !s))
        fprintf(stderr, "[shim:PNG_RW] decode %s (len=%zu path='%s' ok=%d fail=%d)\n",
                s ? "OK" : "FAIL", len, g_last_png_path, g_png_ok_n, g_png_fail_n);
    return s;
}

/* 游戏 BGM 不走 Mix_LoadMUS(路径)，而是 Get_RWops → 自建 RWops → Mix_LoadMUS_RW。
 * 自建 seek 返回的是 fseek 成功/失败（0/-1），不是文件位置，SDL_RWtell 恒为 0，
 * 设备 mixer（minimp3）解不出。这里改走 mixer 默认的 Mix_LoadMUS(路径)。 */
typedef struct _Mix_Music Mix_Music;
typedef struct Mix_Chunk Mix_Chunk;
static Mix_Music *(*real_Mix_LoadMUS)(const char *) = NULL;
static Mix_Music *(*real_Mix_LoadMUS_RW)(SDL_RWops *, int) = NULL;
static Mix_Chunk *(*real_Mix_LoadWAV_RW)(SDL_RWops *, int) = NULL;
static int (*real_Mix_OpenAudio)(int, Uint16, int, int) = NULL;
static int (*real_Mix_Init)(int) = NULL;
static int (*real_Mix_Volume)(int, int) = NULL;
static int (*real_Mix_VolumeMusic)(int) = NULL;
static char g_last_mus_path[4096];

static int shim_is_music_path(const char *path) {
    const char *e;
    if (!path || !(e = strrchr(path, '.')))
        return 0;
    return !strcasecmp(e, ".mp3") || !strcasecmp(e, ".ogg") ||
           !strcasecmp(e, ".wav") || !strcasecmp(e, ".mid") ||
           !strcasecmp(e, ".flac");
}

static void shim_note_audio_path(const char *path) {
    if (!shim_is_music_path(path))
        return;
    strncpy(g_last_mus_path, path, sizeof(g_last_mus_path) - 1);
    g_last_mus_path[sizeof(g_last_mus_path) - 1] = '\0';
}

static void shim_resolve_mix(void) {
    if (real_Mix_LoadMUS && real_Mix_OpenAudio) return;
    void *h = dlopen("/usr/lib/libSDL2_mixer-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen("libSDL2_mixer.so", RTLD_LAZY);
    if (h) {
        real_Mix_LoadMUS = (Mix_Music *(*)(const char *))dlsym(h, "Mix_LoadMUS");
        real_Mix_LoadMUS_RW = (Mix_Music *(*)(SDL_RWops *, int))dlsym(h, "Mix_LoadMUS_RW");
        real_Mix_LoadWAV_RW = (Mix_Chunk *(*)(SDL_RWops *, int))dlsym(h, "Mix_LoadWAV_RW");
        real_Mix_OpenAudio = (int (*)(int, Uint16, int, int))dlsym(h, "Mix_OpenAudio");
        real_Mix_Init = (int (*)(int))dlsym(h, "Mix_Init");
        real_Mix_Volume = (int (*)(int, int))dlsym(h, "Mix_Volume");
        real_Mix_VolumeMusic = (int (*)(int))dlsym(h, "Mix_VolumeMusic");
    }
    if (!real_Mix_LoadMUS)
        real_Mix_LoadMUS = (Mix_Music *(*)(const char *))dlsym(RTLD_NEXT, "Mix_LoadMUS");
    if (!real_Mix_LoadMUS_RW)
        real_Mix_LoadMUS_RW = (Mix_Music *(*)(SDL_RWops *, int))dlsym(RTLD_NEXT, "Mix_LoadMUS_RW");
    if (!real_Mix_LoadWAV_RW)
        real_Mix_LoadWAV_RW = (Mix_Chunk *(*)(SDL_RWops *, int))dlsym(RTLD_NEXT, "Mix_LoadWAV_RW");
    if (!real_Mix_OpenAudio)
        real_Mix_OpenAudio = (int (*)(int, Uint16, int, int))dlsym(RTLD_NEXT, "Mix_OpenAudio");
}

Mix_Music *Mix_LoadMUS(const char *file) {
    shim_resolve_mix();
    const char *p = file;
    if (file && !sw_file_exists(file)) {
        const char *found = sw_find_data_file(file);
        if (found && sw_file_exists(found))
            p = found;
    }
    Mix_Music *m = real_Mix_LoadMUS ? real_Mix_LoadMUS(p) : NULL;
    fprintf(stderr, "[shim:MUS] '%s' -> '%s' %s err=%s\n",
            file ? file : "", p ? p : "", m ? "OK" : "FAIL", SDL_GetError());
    return m;
}

Mix_Music *Mix_LoadMUS_RW(SDL_RWops *src, int freesrc) {
    shim_resolve_mix();
    Mix_Music *m = NULL;
    if (g_last_mus_path[0] && sw_file_exists(g_last_mus_path) && real_Mix_LoadMUS) {
        m = real_Mix_LoadMUS(g_last_mus_path);
        fprintf(stderr, "[shim:MUS_RW] default LoadMUS('%s') -> %s err=%s\n",
                g_last_mus_path, m ? "OK" : "FAIL", SDL_GetError());
    }
    if (!m && real_Mix_LoadMUS_RW) {
        m = real_Mix_LoadMUS_RW(src, 0);
        fprintf(stderr, "[shim:MUS_RW] native RW src=%p -> %s err=%s\n",
                (void *)src, m ? "OK" : "FAIL", SDL_GetError());
    }
    if (src && freesrc)
        SDL_RWclose(src);
    return m;
}

Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
    shim_resolve_mix();
    Mix_Chunk *c = real_Mix_LoadWAV_RW ? real_Mix_LoadWAV_RW(src, freesrc) : NULL;
    fprintf(stderr, "[shim:WAV_RW] src=%p freesrc=%d -> %s err=%s\n",
            (void *)src, freesrc, c ? "OK" : "FAIL",
            c ? "-" : SDL_GetError());
    return c;
}

int Mix_OpenAudio(int freq, Uint16 format, int channels, int chunksize) {
    shim_resolve_mix();
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            fprintf(stderr, "[shim:MIX] SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
    }
    if (real_Mix_Init) {
        int flags = real_Mix_Init(0x00000008); /* MIX_INIT_MP3 */
        fprintf(stderr, "[shim:MIX] Mix_Init(MP3) -> 0x%x err=%s\n",
                flags, SDL_GetError());
    }
    int r = real_Mix_OpenAudio ? real_Mix_OpenAudio(freq, format, channels, chunksize) : -1;
    fprintf(stderr, "[shim:MIX] OpenAudio(%d,0x%x,ch=%d,chunk=%d) -> %d driver=%s err=%s\n",
            freq, (unsigned)format, channels, chunksize, r,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            r != 0 ? SDL_GetError() : "-");
    if (r == 0 && real_Mix_VolumeMusic && real_Mix_VolumeMusic(-1) == 0)
        real_Mix_VolumeMusic(128);
    if (r == 0 && real_Mix_Volume && real_Mix_Volume(-1, -1) == 0)
        real_Mix_Volume(-1, 128);
    return r;
}

/* ---- TTF 字体加载诊断 + 兜底（v10/v11）：定位并修复 CT.ttf 崩溃 ----
 * 实机证据（v10run）：CT.ttf 被 fopen 成功（指针有效），但游戏自建 RWops 后直接调
 * TTF_OpenFontRW(rwops, 30) 返回 NULL → 游戏未判空解引用字体指针 → sig=11 addr=(nil)。
 * 根因与 PNG 同类：游戏自建 RWops 的 read 回调在 glibc 上失效（首个 64KB 块后返回 0 或
 * 定位错误），SDL_ttf 只拿到截断/空的字体数据 → 解析失败 → NULL。
 * 修复（与 PNG 同思路）：TTF_OpenFontRW 返回 NULL 时，用 fopen 拦截器登记的"最近一次
 * 打开的 .ttf 路径"经 glibc real_fopen 整读进内存，SDL_RWFromMem 重建可正常 seek 的内存
 * RWops，再调真 TTF_OpenFontRW 解码。仅对 NULL 触发，不破坏成功路径。
 * 解析策略同 PNG：先 dlsym(RTLD_NEXT)；若设备 libSDL2_ttf 以 RTLD_LOCAL 加载（RTLD_NEXT
 * 不可见其符号），则显式 dlopen("libSDL2_ttf.so", RTLD_LAZY|RTLD_GLOBAL) 取真函数句柄。 */
typedef struct _TTF_Font TTF_Font;
static TTF_Font *(*real_TTF_OpenFont)(const char *, int) = NULL;
static TTF_Font *(*real_TTF_OpenFontRW)(SDL_RWops *, int, int) = NULL;
static const char *(*real_TTF_GetError)(void) = NULL;
static int g_ttf_ready = 0;

static int shim_ttf_verbose(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SHIM_DUMP_TTF");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

static void shim_resolve_ttf(void) {
    if (g_ttf_ready) return;
    g_ttf_ready = 1;
    real_TTF_OpenFont   = (TTF_Font *(*)(const char *, int))dlsym(RTLD_NEXT, "TTF_OpenFont");
    real_TTF_OpenFontRW = (TTF_Font *(*)(SDL_RWops *, int, int))dlsym(RTLD_NEXT, "TTF_OpenFontRW");
    real_TTF_GetError   = (const char *(*)(void))dlsym(RTLD_NEXT, "TTF_GetError");
    if (!real_TTF_OpenFontRW) {
        void *h = dlopen("libSDL2_ttf.so", RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            real_TTF_OpenFontRW = (TTF_Font *(*)(SDL_RWops *, int, int))dlsym(h, "TTF_OpenFontRW");
            if (!real_TTF_GetError) real_TTF_GetError = (const char *(*)(void))dlsym(h, "TTF_GetError");
            if (!real_TTF_OpenFont) real_TTF_OpenFont = (TTF_Font *(*)(const char *, int))dlsym(h, "TTF_OpenFont");
        }
    }
    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] resolve OpenFont=%s OpenFontRW=%s GetError=%s\n",
                real_TTF_OpenFont ? "yes" : "NO",
                real_TTF_OpenFontRW ? "yes" : "NO",
                real_TTF_GetError ? "yes" : "NO");
}

static const char *shim_ttf_err(void) {
    if (!real_TTF_GetError)
        real_TTF_GetError = (const char *(*)(void))dlsym(RTLD_NEXT, "TTF_GetError");
    return real_TTF_GetError ? real_TTF_GetError() : "?";
}

/* 整读文件进内存，返回 SDL_RWFromMem 包装（成功）或 NULL。
 * 调用方须以 freesrc=0 调 TTF/IMG，再手动 SDL_RWclose(mem) + free(*out_buf) 释放，
 * 避免 SDL 内部 close 不释放用户缓冲导致泄漏/重复释放歧义。 */
static SDL_RWops *shim_load_file_mem(const char *path, unsigned char **out_buf) {
    if (!path || !real_fopen || !out_buf) return NULL;
    FILE *ff = real_fopen(path, "rb");
    if (!ff) {
        if (shim_ttf_verbose())
            fprintf(stderr, "[shim:TTF] load_file_mem fopen FAIL '%s'\n", path);
        return NULL;
    }
    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { fclose(ff); return NULL; }
    for (;;) {
        if (len + (1 << 16) > cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap);
            if (!nb) { free(buf); fclose(ff); return NULL; }
            buf = nb;
        }
        size_t r = (size_t)fread(buf + len, 1, 1 << 16, ff);
        if (r == 0) break;
        len += r;
    }
    fclose(ff);
    if (len == 0) { free(buf); return NULL; }
    SDL_RWops *mem = SDL_RWFromMem(buf, (int)len);
    if (!mem) { free(buf); return NULL; }
    *out_buf = buf;
    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] load_file_mem '%s' -> %zu bytes\n", path, len);
    return mem;
}

TTF_Font *TTF_OpenFont(const char *file, int ptsize) {
    shim_resolve_ttf();
    TTF_Font *f = real_TTF_OpenFont ? real_TTF_OpenFont(file, ptsize) : NULL;

    /* 兜底：filename 版本失败 → 整读进内存用 mem RWops 重开（绕过可能损坏的文件型 RWops）。 */
    if (!f && file && real_fopen && real_TTF_OpenFontRW) {
        if (shim_ttf_verbose())
            fprintf(stderr, "[shim:TTF] OpenFont fallback reopen '%s'\n", file);
        unsigned char *buf = NULL;
        SDL_RWops *mem = shim_load_file_mem(file, &buf);
        if (mem) {
            f = real_TTF_OpenFontRW(mem, 0, ptsize);   /* freesrc=0：我们持有 mem/buf */
            SDL_RWclose(mem);
            free(buf);
            if (shim_ttf_verbose())
                fprintf(stderr, "[shim:TTF] OpenFont fallback '%s' -> %p%s\n",
                        file, (void *)f, f ? "" : " (STILL NULL)");
        }
    }

    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] OpenFont '%s' pt=%d -> %p%s\n",
                file ? file : "(null)", ptsize,
                (void *)f, f ? "" : (real_TTF_OpenFont ? "" : " (no-real) "));
    if (!f && shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF]   last error: %s\n", shim_ttf_err());
    return f;
}

TTF_Font *TTF_OpenFontRW(SDL_RWops *src, int freesrc, int ptsize) {
    shim_resolve_ttf();
    TTF_Font *f = real_TTF_OpenFontRW ? real_TTF_OpenFontRW(src, freesrc, ptsize) : NULL;
    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] OpenFontRW pt=%d -> %p%s\n", ptsize, (void *)f, f ? "" : "");

    /* 兜底：RWops 版本失败 → 用 fopen 拦截器登记的"最近一次 .ttf 路径"整读重开。
     * 仅对文件型字体（有路径）生效；内存/资源 RWops 无路径则跳过，不改其行为。 */
    if (!f && g_last_ttf_path[0] && real_fopen && real_TTF_OpenFontRW) {
        if (shim_ttf_verbose())
            fprintf(stderr, "[shim:TTF] OpenFontRW fallback reopen '%s'\n", g_last_ttf_path);
        unsigned char *buf = NULL;
        SDL_RWops *mem = shim_load_file_mem(g_last_ttf_path, &buf);
        if (mem) {
            f = real_TTF_OpenFontRW(mem, 0, ptsize);   /* freesrc=0：我们持有 mem/buf */
            SDL_RWclose(mem);
            free(buf);
            if (shim_ttf_verbose())
                fprintf(stderr, "[shim:TTF] OpenFontRW fallback -> %p%s\n",
                        (void *)f, f ? "" : " (STILL NULL)");
        } else if (shim_ttf_verbose()) {
            fprintf(stderr, "[shim:TTF] OpenFontRW fallback mem NULL (read fail?)\n");
        }
    }

    if (!f && shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF]   last error: %s\n", shim_ttf_err());
    return f;
}

/* ---- TTF 渲染拦截诊断（v12）：定位字体渲染崩溃 ----
 * CT.ttf / CS.ttf 两个字体都加载成功（OpenFontRW 返回有效指针），但渲染时
 * sig=11 addr=(nil)（PC 在 freetype 内，LR=libSWD3E.so+0x1428c4）。
 * 在此拦截所有 TTF 渲染函数，打印入参（font 指针、text 首字符），
 * 通过"enter 出现但 return 不出现"确定哪个函数在什么参数下崩溃。 */
static SDL_Surface *(*real_TTF_RenderUTF8_Blended)(TTF_Font *, const char *, SDL_Color) = NULL;
static SDL_Surface *(*real_TTF_RenderUTF8_Solid)(TTF_Font *, const char *, SDL_Color) = NULL;
static SDL_Surface *(*real_TTF_RenderUTF8_Shaded)(TTF_Font *, const char *, SDL_Color, SDL_Color) = NULL;
static SDL_Surface *(*real_TTF_RenderUNICODE_Blended)(TTF_Font *, const Uint16 *, SDL_Color) = NULL;
static int (*real_TTF_SizeUTF8)(TTF_Font *, const char *, int *, int *) = NULL;
static SDL_Surface *(*real_TTF_RenderUNICODE_Solid)(TTF_Font *, const Uint16 *, SDL_Color) = NULL;

/* 十六进制 dump：判定游戏传入 TTF 渲染的文本是 UTF-8 还是 GBK/Big5 等遗留编码
 * （"进游戏不显示文字"的首要怀疑点 —— 游戏把 GBK/Big5 字节直接喂 TTF_RenderUTF8，
 *  SDL_ttf 按 UTF-8 解析失败 → 返回 NULL surface → 无文字）。 */
static void shim_hexdump(const char *label, const void *p, int n) {
    if (!p) { fprintf(stderr, "    %s: (null)\n", label); return; }
    const unsigned char *b = (const unsigned char *)p;
    fprintf(stderr, "    %s[%d]:", label, n);
    for (int i = 0; i < n && i < 48; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

/* ---- 中文编码转码（#1 不显示文字的根因修复）----
 * 轩辕剑3天之痕等中文 Windows 移植游戏把 GBK / Big5 字节直接喂给 TTF_RenderUTF8_*，
 * 而 SDL_ttf 按 UTF-8 解析 —— 多字节序列非法 → 渲染空面 → 屏幕无文字。
 * 这里在 wrapper 内检测：若入参非合法 UTF-8，则按 GBK→Big5→GB18030 顺序尝试
 * 转码为 UTF-8 再交给真函数。iconv 任一编码失败则回退原串（不劣化现状）。 */
static int shim_is_valid_utf8(const unsigned char *s, int n) {
    int i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int extra;
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return 0;
        if (i + extra >= n) return 0;
        for (int k = 1; k <= extra; k++)
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += extra + 1;
    }
    return 1;
}

static char *shim_gbk_to_utf8(const char *src) {
    /* 首选编码由游戏实际打开的字体推断（SDL_RWFromFile 写入 g_ttf_enc）：
     *   CT.ttf / CHT -> BIG5（繁体）；CS.ttf / CHS -> GBK（简体）。
     * 再回退其它遗留中文编码。GBK 与 BIG5 字节区间重叠但映射不同，
     * 故必须按字体实际模式选对首选编码，否则会"解码成功却出乱码"。 */
    const char *encs[4];
    int k = 0;
    if (g_ttf_enc) encs[k++] = g_ttf_enc;   /* 首选：字体推断的编码 */
    encs[k++] = "GBK";
    encs[k++] = "BIG5";
    encs[k++] = "GB18030";
    for (int i = 0; i < k; i++) {
        const char *enc = encs[i];
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (strcmp(encs[j], enc) == 0) { dup = 1; break; }
        if (dup) continue;
        iconv_t cd = iconv_open("UTF-8", enc);
        if (cd == (iconv_t)-1) continue;
        size_t inlen = strlen(src);
        size_t outcap = inlen * 4 + 16;
        char *out = (char *)malloc(outcap);
        if (!out) { iconv_close(cd); return NULL; }
        char *inbuf = (char *)(uintptr_t)src;
        size_t ileft = inlen;
        char *outbuf = out;
        size_t oleft = outcap;
        memset(out, 0, outcap);
        size_t r = iconv(cd, &inbuf, &ileft, &outbuf, &oleft);
        iconv_close(cd);
        if (r != (size_t)-1) { *outbuf = '\0'; return out; }
        free(out);
    }
    return NULL;
}

/* 准备文本：非法 UTF-8 视为遗留中文编码并转码；返回需 free 的缓冲（*conv）或 NULL。
 * 返回 *out 指向最终要渲染的字符串（转码后或原串）。 */
static const char *shim_ttf_prep_text(const char *text, char **conv) {
    *conv = NULL;
    if (!text || !*text) return text;
    int n = (int)strlen(text);
    if (!shim_is_valid_utf8((const unsigned char *)text, n)) {
        *conv = shim_gbk_to_utf8(text);
        if (*conv) return *conv;
    }
    return text;
}

static void shim_resolve_ttf_render(void) {
    shim_resolve_ttf();   /* 确保 TTF 基础符号已解析（含 dlopen 回退） */
    if (!real_TTF_RenderUTF8_Blended)
        real_TTF_RenderUTF8_Blended = (SDL_Surface *(*)(TTF_Font*,const char*,SDL_Color))
            dlsym(RTLD_NEXT, "TTF_RenderUTF8_Blended");
    if (!real_TTF_RenderUTF8_Solid)
        real_TTF_RenderUTF8_Solid = (SDL_Surface *(*)(TTF_Font*,const char*,SDL_Color))
            dlsym(RTLD_NEXT, "TTF_RenderUTF8_Solid");
    if (!real_TTF_RenderUTF8_Shaded)
        real_TTF_RenderUTF8_Shaded = (SDL_Surface *(*)(TTF_Font*,const char*,SDL_Color,SDL_Color))
            dlsym(RTLD_NEXT, "TTF_RenderUTF8_Shaded");
    if (!real_TTF_RenderUNICODE_Blended)
        real_TTF_RenderUNICODE_Blended = (SDL_Surface *(*)(TTF_Font*,const Uint16*,SDL_Color))
            dlsym(RTLD_NEXT, "TTF_RenderUNICODE_Blended");
    if (!real_TTF_RenderUNICODE_Solid)
        real_TTF_RenderUNICODE_Solid = (SDL_Surface *(*)(TTF_Font*,const Uint16*,SDL_Color))
            dlsym(RTLD_NEXT, "TTF_RenderUNICODE_Solid");
    if (!real_TTF_SizeUTF8)
        real_TTF_SizeUTF8 = (int (*)(TTF_Font*,const char*,int*,int*))
            dlsym(RTLD_NEXT, "TTF_SizeUTF8");
    /* 兜底：若渲染符号仍 NULL（设备 libSDL2_ttf 以 RTLD_LOCAL 加载，RTLD_NEXT 不可见），
     * 显式 dlopen 并重新解析，避免拦截器因 real 为 NULL 而 return NULL → 无文字。 */
    if (!real_TTF_RenderUTF8_Blended || !real_TTF_RenderUNICODE_Blended || !real_TTF_SizeUTF8) {
        void *h = dlopen("libSDL2_ttf.so", RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            if (!real_TTF_RenderUTF8_Blended)    real_TTF_RenderUTF8_Blended = (SDL_Surface*(*)(TTF_Font*,const char*,SDL_Color))dlsym(h,"TTF_RenderUTF8_Blended");
            if (!real_TTF_RenderUTF8_Solid)      real_TTF_RenderUTF8_Solid   = (SDL_Surface*(*)(TTF_Font*,const char*,SDL_Color))dlsym(h,"TTF_RenderUTF8_Solid");
            if (!real_TTF_RenderUTF8_Shaded)     real_TTF_RenderUTF8_Shaded  = (SDL_Surface*(*)(TTF_Font*,const char*,SDL_Color,SDL_Color))dlsym(h,"TTF_RenderUTF8_Shaded");
            if (!real_TTF_RenderUNICODE_Blended) real_TTF_RenderUNICODE_Blended = (SDL_Surface*(*)(TTF_Font*,const Uint16*,SDL_Color))dlsym(h,"TTF_RenderUNICODE_Blended");
            if (!real_TTF_RenderUNICODE_Solid)   real_TTF_RenderUNICODE_Solid = (SDL_Surface*(*)(TTF_Font*,const Uint16*,SDL_Color))dlsym(h,"TTF_RenderUNICODE_Solid");
            if (!real_TTF_SizeUTF8)              real_TTF_SizeUTF8 = (int(*)(TTF_Font*,const char*,int*,int*))dlsym(h,"TTF_SizeUTF8");
        }
        if (shim_ttf_verbose())
            fprintf(stderr, "[shim:TTF] resolve-render fallback: Blended=%s Solid=%s Shaded=%s UniBlend=%s UniSolid=%s Size=%s\n",
                    real_TTF_RenderUTF8_Blended?"yes":"NO",
                    real_TTF_RenderUTF8_Solid?"yes":"NO",
                    real_TTF_RenderUTF8_Shaded?"yes":"NO",
                    real_TTF_RenderUNICODE_Blended?"yes":"NO",
                    real_TTF_RenderUNICODE_Solid?"yes":"NO",
                    real_TTF_SizeUTF8?"yes":"NO");
    }
}

SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg) {
    shim_resolve_ttf_render();
    char *conv = NULL;
    const char *t = shim_ttf_prep_text(text, &conv);
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Blended font=%p enc=%s orig='%.24s'%s\n",
                (void *)font, g_ttf_enc, text ? text : "(null)", conv ? " (transcode->UTF8)" : "");
        shim_hexdump("utf8", t, 32);
    }
    SDL_Surface *s = real_TTF_RenderUTF8_Blended
        ? real_TTF_RenderUTF8_Blended(font, t, fg) : NULL;
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Blended -> %p %s\n", (void *)s,
                s ? "(ok)" : "NULL(!)");
        if (s) fprintf(stderr, "    surf w=%d h=%d\n", s->w, s->h);
    }
    free(conv);
    return s;
}

SDL_Surface *TTF_RenderUTF8_Shaded(TTF_Font *font, const char *text, SDL_Color fg, SDL_Color bg) {
    shim_resolve_ttf_render();
    char *conv = NULL;
    const char *t = shim_ttf_prep_text(text, &conv);
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Shaded font=%p enc=%s orig='%.24s'%s\n",
                (void *)font, g_ttf_enc, text ? text : "(null)", conv ? " (transcode->UTF8)" : "");
        shim_hexdump("utf8", t, 32);
    }
    SDL_Surface *s = real_TTF_RenderUTF8_Shaded
        ? real_TTF_RenderUTF8_Shaded(font, t, fg, bg) : NULL;
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Shaded -> %p %s\n", (void *)s,
                s ? "(ok)" : "NULL(!)");
        if (s) fprintf(stderr, "    surf w=%d h=%d\n", s->w, s->h);
    }
    free(conv);
    return s;
}

SDL_Surface *TTF_RenderUTF8_Solid(TTF_Font *font, const char *text, SDL_Color fg) {
    shim_resolve_ttf_render();
    char *conv = NULL;
    const char *t = shim_ttf_prep_text(text, &conv);
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Solid font=%p enc=%s orig='%.24s'%s\n",
                (void *)font, g_ttf_enc, text ? text : "(null)", conv ? " (transcode->UTF8)" : "");
        shim_hexdump("utf8", t, 32);
    }
    SDL_Surface *s = real_TTF_RenderUTF8_Solid
        ? real_TTF_RenderUTF8_Solid(font, t, fg) : NULL;
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUTF8_Solid -> %p %s\n", (void *)s,
                s ? "(ok)" : "NULL(!)");
        if (s) fprintf(stderr, "    surf w=%d h=%d\n", s->w, s->h);
    }
    free(conv);
    return s;
}

SDL_Surface *TTF_RenderUNICODE_Blended(TTF_Font *font, const Uint16 *text, SDL_Color fg) {
    shim_resolve_ttf_render();
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUNICODE_Blended font=%p\n", (void *)font);
        shim_hexdump("unicode16", text, 64);
    }
    SDL_Surface *s = real_TTF_RenderUNICODE_Blended
        ? real_TTF_RenderUNICODE_Blended(font, text, fg) : NULL;
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUNICODE_Blended -> %p %s\n", (void *)s,
                s ? "(ok)" : "NULL(!)");
        if (s) fprintf(stderr, "    surf w=%d h=%d\n", s->w, s->h);
    }
    return s;
}

SDL_Surface *TTF_RenderUNICODE_Solid(TTF_Font *font, const Uint16 *text, SDL_Color fg) {
    shim_resolve_ttf_render();
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUNICODE_Solid font=%p\n", (void *)font);
        shim_hexdump("unicode16", text, 64);
    }
    SDL_Surface *s = real_TTF_RenderUNICODE_Solid
        ? real_TTF_RenderUNICODE_Solid(font, text, fg) : NULL;
    if (shim_ttf_verbose()) {
        fprintf(stderr, "[shim:TTF] RenderUNICODE_Solid -> %p %s\n", (void *)s,
                s ? "(ok)" : "NULL(!)");
        if (s) fprintf(stderr, "    surf w=%d h=%d\n", s->w, s->h);
    }
    return s;
}

int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h) {
    shim_resolve_ttf_render();
    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] SizeUTF8 font=%p text='%.20s'\n",
                (void *)font, text ? text : "(null)");
    int ret = real_TTF_SizeUTF8 ? real_TTF_SizeUTF8(font, text, w, h) : -1;
    if (shim_ttf_verbose())
        fprintf(stderr, "[shim:TTF] SizeUTF8 -> %d w=%d h=%d\n",
                ret, w ? *w : -1, h ? *h : -1);
    return ret;
}

/* SMPEG 是不透明句柄（来自 SDL2/SMPEG.h，镜像未自带该头）；仅作指针传递，前向声明即可。 */
typedef struct SMPEG SMPEG;

/* glibc 标准流对象（见 glibc <libio.h>，此处前向声明即可，链接期由 libc 提供） */
extern FILE _IO_2_1_stdin_;
extern FILE _IO_2_1_stdout_;
extern FILE _IO_2_1_stderr_;

/* bionic 的 __sF[] 数组（stdin/stdout/stderr）。导出为版本 LIBC。 */
FILE __sF[3];

/* ---- SIGSEGV 崩溃诊断（增强版）：打印故障地址 + 完整调用链 ----
 * 旧版仅用 backtrace() 打 3 帧且丢弃 si_addr，无法区分"NULL 解引用 vs 越界读"，
 * 也拿不到真实调用链。本版（用于定位 libSWD3E.so+0x13d4d4 半字数组扫描越界崩）：
 *   - 打印 info->si_addr：近 0 ⇒ NULL 解引用；落在某缓冲末端 ⇒ 越界读（数据未装入）。
 *   - 用 ucontext 直接读故障瞬间寄存器 PC/SP/FP(x29)/LR(x30)。
 *   - 帧指针回溯（FP-walk）：从故障函数帧向上逐帧取返回地址，给出真实调用链
 *     （绕过 signal 帧与坏栈导致的 backtrace() 截断），每帧 dladdr 解析模块+偏移
 *     （即使 stripped 也能定位 libSWD3E.so+0xXXXX，与静态反汇编偏移对照）。
 *   - SA_ONSTACK + 独立信号栈：即使主栈被踩也能安全展开，handler 自身不会二次崩。
 */
#ifndef REG_X29
#define REG_X29 29
#endif
#ifndef REG_X30
#define REG_X30 30
#endif

static char g_segv_stack[65536];   /* 独立信号栈，足够展开调用链 */

static void shim_segv_handler(int sig, siginfo_t *info, void *ctx) {
    /* 先恢复默认行为：handler 内任何二次故障（如读坏栈）立即终止，不递归。 */
    signal(SIGSEGV, SIG_DFL);

    ucontext_t *uc = (ucontext_t *)ctx;
    mcontext_t *mc = &uc->uc_mcontext;
    void *pc = (void *)(uintptr_t)mc->pc;
    void *sp = (void *)(uintptr_t)mc->sp;
    void *fp = (void *)(uintptr_t)mc->regs[REG_X29];
    void *lr = (void *)(uintptr_t)mc->regs[REG_X30];

    fprintf(stderr, "\n[shim:CRASH] ========== SIGSEGV ==========\n");
    fprintf(stderr, "[shim:CRASH] sig=%d si_code=%d si_addr=%p\n",
            sig, info ? info->si_code : -1, info ? info->si_addr : (void *)0);
    fprintf(stderr, "[shim:CRASH] PC=%p SP=%p FP(x29)=%p LR(x30)=%p\n", pc, sp, fp, lr);

    Dl_info di;
    if (dladdr(pc, &di) && di.dli_fbase) {
        fprintf(stderr, "[shim:CRASH] fault: %s +0x%llx%s%s\n",
                di.dli_fname,
                (unsigned long long)((char *)pc - (char *)di.dli_fbase),
                di.dli_sname ? "  " : "",
                di.dli_sname ? di.dli_sname : "");
    } else {
        fprintf(stderr, "[shim:CRASH] fault: (unresolved) %p\n", pc);
    }

    /* backtrace() 作为次要用源 */
    void *frames[64];
    int n = backtrace(frames, 64);
    fprintf(stderr, "[shim:CRASH] backtrace(%d):\n", n);
    for (int i = 0; i < n; i++) {
        if (dladdr(frames[i], &di) && di.dli_fbase) {
            fprintf(stderr, "  #%-2d %p  %s +0x%llx\n", i, frames[i],
                    di.dli_fname,
                    (unsigned long long)((char *)frames[i] - (char *)di.dli_fbase));
        } else {
            fprintf(stderr, "  #%-2d %p\n", i, frames[i]);
        }
    }

    /* 帧指针回溯：故障帧向上的真实调用链（抽成 shim_fpwalk，复用同一逻辑） */
    fprintf(stderr, "[shim:CRASH] fp-walk (from fault frame, 0=caller of fault):\n");
    shim_fpwalk(fp);

    shim_dump_open_ring("SIGSEGV (occurs immediately after 'RoleDataBase init Failed.')");

    fflush(stderr);
    raise(SIGSEGV);
}

__attribute__((constructor))
static void shim_segv_init(void) {
    /* SHIM_DUMP_OPEN=1：实时打印每一次文件打开（路径+返回值），用于定位 RoleDataBase
     * init 期间游戏究竟打开/尝试打开了哪些文件。绕过环形缓冲双副本问题。 */
    if (getenv("SHIM_DUMP_OPEN"))
        g_dump_open = 1;

    /* 安装独立信号栈，避免主栈被踩时 handler 自身无法运行 */
    stack_t ss;
    ss.ss_sp = g_segv_stack;
    ss.ss_size = sizeof(g_segv_stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = shim_segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

__attribute__((constructor))
static void init_sF(void) {
    memcpy(&__sF[0], &_IO_2_1_stdin_,  sizeof(FILE));
    memcpy(&__sF[1], &_IO_2_1_stdout_, sizeof(FILE));
    memcpy(&__sF[2], &_IO_2_1_stderr_, sizeof(FILE));
}

/* ---- Android_JNI_* 文件 I/O 辅助 ---- */
long Android_JNI_FileSize(FILE *f) {
    if (!f) return -1;
    long cur = ftell(f);
    if (cur < 0) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, cur, SEEK_SET);
    return sz;
}

int Android_JNI_FileClose(FILE *f) { return f ? fclose(f) : -1; }

/* 设备侧无 Art/JVM；返回 NULL。游戏资源加载走下面不带 env 的文件函数。 */
void *Android_JNI_GetEnv(void) { return NULL; }

int Android_JNI_FileSeek(FILE *f, long off, int whence) {
    return f ? fseek(f, off, whence) : -1;
}

size_t Android_JNI_FileRead(void *buf, size_t size, size_t nmemb, FILE *f) {
    return f ? fread(buf, size, nmemb, f) : 0;
}

FILE *Android_JNI_FileOpen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f)
        fprintf(stderr, "[shim] Android_JNI_FileOpen FAILED: path='%s' mode='%s'\n",
                path ? path : "(null)", mode ? mode : "(null)");
    if (g_dump_open) {
        fprintf(stderr, "[shim:Android_JNI_FileOpen] '%s' -> %p%s\n", path ? path : "(null)",
                (void *)f, f ? "" : " (FAIL)");
        shim_fpwalk(__builtin_frame_address(0));
    }
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "Android_JNI_FileOpen '%s' -> %p%s",
        path ? path : "(null)", (void *)f, f ? "" : " FAIL"); shim_open_push(_b); }
    return f;
}

/* ---- APKXAndroid_JNI_FileOpen（hook 目标）----
 * 游戏所有“资源读取”统一经此函数（APKX = Android APK 资源读取器）。
 * 在 Android 上它从 APK assets 经 AAssetManager 打开；glibc 上无 APK/AAssetManager，
 * 原函数返回 NULL（导致 IMG_Load_RW(NULL) → SDL_CreateTextureFromSurface 崩溃）。
 *
 * 本实现把它 hook 到本地文件系统：把相对资源名映射到 $GAMEDIR/assets 树，用
 * SDL_RWFromFile 打开真实文件。映射候选（按序尝试，第一个成功即返回）：
 *   $ANDROID_APP_PATH/<rel>          (= $GAMEDIR/assets/<rel>)
 *   $GAMEDIR/<rel>                   (root 可能是 $GAMEDIR 而非 assets)
 *   $ANDROID_APP_PATH/Resource/<rel> (若 rel 未含 Resource/ 前缀)
 *   $GAMEDIR/Resource/<rel>
 * rel = 去掉首部 '/' 后的 fileName。 */
SDL_RWops *APKXAndroid_JNI_FileOpen(const char *fileName, const char *mode) {
    if (!fileName) fileName = "";
    const char *m = (mode && *mode) ? mode : "rb";
    /* 诊断：SHIM_DUMP_OPEN 时打印 APKX 入口文件名 + 完整调用链，
     * 看清是谁（fCreateFile / RoleDataBase）发起了资源读取。不改变原逻辑。 */
    if (g_dump_open) {
        fprintf(stderr, "[apkx] ENTER fileName='%s' mode='%s'\n", fileName, m);
        shim_fpwalk(__builtin_frame_address(0));
    }
    /* 绝对路径（已含根，例如被 hook 后的 GetResourcePath 给出的路径）直接打开，
     * 避免与 $ANDROID_APP_PATH 重复拼接成 $APP/$APP/... */
    if (fileName[0] == '/') {
        SDL_RWops *rw = SDL_RWFromFile(fileName, m);
        fprintf(stderr, "[apkx] abs '%s' -> %s\n", fileName, rw ? "OK" : "NULL");
        return rw;
    }
    const char *app = getenv("ANDROID_APP_PATH");
    char appbuf[2048];
    if (!app || !*app) { appbuf[0] = '.'; appbuf[1] = 0; app = appbuf; }

    const char *rel = fileName;
    while (*rel == '/') rel++;

    char gdir[2048];
    snprintf(gdir, sizeof(gdir), "%s", app);
    char *slash = strrchr(gdir, '/');
    if (slash) *slash = 0;   /* gdir = dirname(app) = $GAMEDIR */

    char cand[4][4096];
    int nc = 0;
    snprintf(cand[nc++], sizeof(cand[0]), "%s/%s",  app,  rel);
    snprintf(cand[nc++], sizeof(cand[0]), "%s/%s",  gdir, rel);
    if (!strstr(rel, "Resource/")) {
        snprintf(cand[nc++], sizeof(cand[0]), "%s/Resource/%s",  app,  rel);
        snprintf(cand[nc++], sizeof(cand[0]), "%s/Resource/%s",  gdir, rel);
    }

    for (int i = 0; i < nc; i++) {
        SDL_RWops *rw = SDL_RWFromFile(cand[i], m);   /* 走本 shim 的拦截器→真实 fopen */
        if (rw) {
            fprintf(stderr, "[apkx] '%s' -> OK ('%s')\n", fileName, cand[i]);
            { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "APKX '%s' -> OK ('%s')",
                fileName ? fileName : "(null)", cand[i]); shim_open_push(_b); }
            return rw;
        }
    }
    fprintf(stderr, "[apkx] '%s' -> NULL (%d candidates, app='%s')\n",
            fileName, nc, app);
    { char _b[OPEN_EVT_LEN]; snprintf(_b, sizeof _b, "APKX '%s' -> NULL (%d candidates)",
        fileName ? fileName : "(null)", nc); shim_open_push(_b); }
    return NULL;
}

/* ---- 资源根路径重定向（hook 目标）----
 * 在 Android 上，这些函数经 JNI 取得 APK/sdcard 根路径，写入调用方提供的 char* 缓冲；
 * glibc 上 JNI 返回空串，导致游戏拼出空资源路径 → LoadImageFile 收到空 path →
 * IMG_Load_RW(NULL) 崩溃。这里把它们重定向到本机文件系统：
 *   $GAMEDIR/assets  —— 资源（APKX / Resource 树）
 *   $GAMEDIR         —— 存档/数据（SDCard/Target/Inter/GAMESAVE）
 * 这些函数都是 free function（mangled 形如 _Z11GetAPKXPathPc），唯一参数 char* out
 * 落在 ARM64 x0，与我们的实现一致，无需猜测 this 约定。 */

/* 取 $GAMEDIR（loader 自身目录）。优先用 ANDROID_APP_PATH 的 dirname，
 * 否则回退 /proc/self/exe。返回静态缓冲（加载期单线程使用，足够）。 */
static const char *sw_gamedir(void) {
    static char gdir[2048];
    const char *app = getenv("ANDROID_APP_PATH");
    if (app && *app) {
        snprintf(gdir, sizeof(gdir), "%s", app);
        char *slash = strrchr(gdir, '/');
        if (slash) *slash = 0;            /* dirname(app) = $GAMEDIR */
        return gdir;
    }
    char link[1024];
    ssize_t l = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (l > 0) {
        link[l] = 0;
        char *s = strrchr(link, '/');
        if (s) *s = 0;
        snprintf(gdir, sizeof(gdir), "%s", link);
    } else {
        snprintf(gdir, sizeof(gdir), ".");
    }
    return gdir;
}

static void sw_path_write(const char *root, char *out) {
    if (out) {
        strncpy(out, root, 4095);
        out[4095] = 0;
    }
    fprintf(stderr, "[swpath] -> '%s'\n", root);
}

void GetAPKXPath(char *out) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s/assets", sw_gamedir());
    sw_path_write(buf, out);
}
void GetAPKXreadpath(char *out) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s/assets", sw_gamedir());
    sw_path_write(buf, out);
}
void GetSDCardPath(char *out)   { sw_path_write(sw_gamedir(), out); }
void GetTargetPath(char *out)   { sw_path_write(sw_gamedir(), out); }
void GetInterPath(char *out)    { sw_path_write(sw_gamedir(), out); }
void GetGAMESAVEPath(char *out) { sw_path_write(sw_gamedir(), out); }

/* fileIO::GetResourcePath(const char* name) —— 返回资源完整路径。
 * 模板为 "%s/Resource"（见 ELF 字符串），故返回 $GAMEDIR/assets/Resource/<name>。
 * 按实例方法约定接收 this(x0)/name(x1)；返回静态缓冲（调用方随即使用）。
 * 若 name 已为绝对路径则原样透传；否则拼 $GAMEDIR/assets/Resource/<rel>。
 * 注：即使 this 实际是静态方法（name 在 x0），本实现忽略 this、只使用 name，
 * 调用约定差异不影响正确性（this 不被解引用）。 */
const char *fileIO_GetResourcePath(void *self, const char *name) {
    (void)self;
    static char buf[4096];
    if (!name || !*name) { buf[0] = 0; return buf; }
    if (name[0] == '/') {                     /* 绝对路径，原样透传 */
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        fprintf(stderr, "[swpath] GetResourcePath '%s' -> abs\n", name);
        return buf;
    }
    const char *rel = name;
    while (*rel == '/') rel++;
    snprintf(buf, sizeof(buf), "%s/assets/Resource/%s", sw_gamedir(), rel);
    if (!sw_file_exists(buf)) {
        const char *found = sw_find_data_file(rel);
        if (found && sw_file_exists(found)) {
            strncpy(buf, found, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
        }
    }
    fprintf(stderr, "[swpath] GetResourcePath '%s' -> '%s'\n", name, buf);
    return buf;
}

/* ---- ACT / APKX 数据文件（RoleDataBase / PathDataBase / StoryDataBase）----
 * 引擎 OpenDataFiles() 调 fileIO::GetACTPath("maps.dat"|"path.dat"|"talk1.dat")。
 * 原实现：拼 fileIO 对象里的 ACT 根（init 时 = GetTargetPath = $GAMEDIR）+ 文件名，
 * 再 GetAndroidFileIsExists（JNI，glibc 上 methodID=NULL → 恒返回 0），失败则
 * Android_APKX_SetFile（同样 JNI 失败）并把路径写成空（path[0]=0,path[1]=0）。
 * fCreateFile 收到空路径 → fopen("") → "RoleDataBase init Failed." → SIGSEGV。
 *
 * 真机 assets/ 下是扁平文件（path.dat / maps.dat / all_char.act …），不是 APK。
 * GetACTPath 直接映射到 $GAMEDIR/assets/<name>。
 * GetAPKXFileLenv / GetAPKXFileOffsetv 是无参 JNI 包装（查“当前 SetFile 的文件”），
 * 签名是 void，不是 const char*。 */

static int sw_file_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void apkx_path(const char *name, char *out, size_t outsz) {
    const char *base = sw_gamedir();
    const char *rel = name ? name : "";
    while (*rel == '/') rel++;
    snprintf(out, outsz, "%s/assets/%s", base, rel);
}

/* 在 $GAMEDIR/assets、<name>、$GAMEDIR 下找数据文件。返回静态缓冲。 */
static const char *sw_find_data_file(const char *name) {
    static char buf[4096];
    const char *base = sw_gamedir();
    if (!name || !*name) { buf[0] = 0; return buf; }

    /* 绝对路径：先原样，缺失则把 /Music/ /Resource/ 补到 assets/ 下。 */
    if (name[0] == '/') {
        if (sw_file_exists(name)) {
            strncpy(buf, name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            return buf;
        }
        const char *ins = strstr(name, "/Music/");
        if (!ins) ins = strstr(name, "/Resource/");
        if (!ins) ins = strstr(name, "/Video/");
        if (!ins) ins = strstr(name, "/zh-Hant/");
        if (!ins) ins = strstr(name, "/zh-Hans/");
        if (ins) {
            snprintf(buf, sizeof(buf), "%s/assets%s", base, ins);
            if (sw_file_exists(buf)) return buf;
        }
        name = strrchr(name, '/') + 1;
    }

    const char *rel = name;
    while (*rel == '/') rel++;
    if (!*rel) { buf[0] = 0; return buf; }
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    snprintf(buf, sizeof(buf), "%s/assets/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/assets/Resource/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/assets/Music/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/assets/Video/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/assets/zh-Hant/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    snprintf(buf, sizeof(buf), "%s/assets/zh-Hans/%s", base, rel);
    if (sw_file_exists(buf)) return buf;
    /* 即使不存在也回到 assets/，让后续 fopen 打出真实路径而不是空串 */
    snprintf(buf, sizeof(buf), "%s/assets/%s", base, rel);
    return buf;
}

/* fileIO::GetVideoPath：原实现拼 GetTargetPath+$GAMEDIR+"/Video/"+小写文件名，
 * 再 GetAndroidFileIsExists。真机片源在 $GAMEDIR/assets/Video/，原路径找不到就
 * 返回 NULL，cBinker::bik_Open 直接失败，新游戏开场被跳过（“十六年以后”）。 */
const char *fileIO_GetVideoPath(void *self, const char *name) {
    static char buf[4096];
    char fname[256];
    const char *rel = name ? name : "";
    size_t i;

    while (*rel == '/') rel++;
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;
    for (i = 0; rel[i] && i + 1 < sizeof(fname); i++) {
        unsigned char c = (unsigned char)rel[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 32);
        fname[i] = (char)c;
    }
    fname[i] = 0;

    buf[0] = 0;
    if (name && name[0] == '/' && sw_file_exists(name)) {
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    } else if (fname[0]) {
        const char *found = sw_find_data_file(fname);
        if (found && sw_file_exists(found)) {
            strncpy(buf, found, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
        } else {
            snprintf(buf, sizeof(buf), "%s/assets/Video/%s", sw_gamedir(), fname);
        }
    }

    int ok = sw_file_exists(buf);
    fprintf(stderr, "[swpath] GetVideoPath '%s' -> '%s'%s\n",
            name ? name : "", buf, ok ? "" : " (MISSING)");
    if (self) {
        strncpy((char *)self, ok ? buf : "", 1023);
        ((char *)self)[1023] = 0;
        return ok ? (const char *)self : NULL;
    }
    return ok ? buf : NULL;
}

const char *fileIO_GetACTPath(void *self, const char *name) {
    const char *found = sw_find_data_file(name);
    fprintf(stderr, "[swpath] GetACTPath '%s' -> '%s'%s\n",
            name ? name : "", found, sw_file_exists(found) ? "" : " (MISSING)");
    if (self) {
        strncpy((char *)self, found, 2047);
        ((char *)self)[2047] = 0;
        return (const char *)self;
    }
    return found;
}

int GetAndroidFileIsExists(const char *path) {
    int ok = 0;
    if (path && *path) {
        if (sw_file_exists(path))
            ok = 1;
        else {
            const char *found = sw_find_data_file(path);
            ok = (found && sw_file_exists(found));
        }
    }
    fprintf(stderr, "[shim:EXISTS] '%s' -> %d\n", path ? path : "", ok);
    return ok;
}

int Android_APKX_SetFile(const char *name, const char *mode) {
    (void)mode;
    const char *found = sw_find_data_file(name);
    strncpy(g_apkx_current, found, sizeof(g_apkx_current) - 1);
    g_apkx_current[sizeof(g_apkx_current) - 1] = 0;
    int ok = sw_file_exists(g_apkx_current);
    if (!ok && found && *found) {
        struct stat st;
        if (stat(found, &st) == 0 && S_ISDIR(st.st_mode)) {
            char inside[4096];
            snprintf(inside, sizeof(inside), "%s/StringDB.txt", found);
            if (sw_file_exists(inside)) {
                strncpy(g_apkx_current, inside, sizeof(g_apkx_current) - 1);
                g_apkx_current[sizeof(g_apkx_current) - 1] = 0;
                ok = 1;
            }
        }
    }
    fprintf(stderr, "[shim:APKX] SetFile name='%s' -> '%s' ok=%d\n",
            name ? name : "", g_apkx_current, ok);
    return ok ? 1 : 0;
}

/* 引擎符号 _Z14GetAPKXFileLenv / _Z17GetAPKXFileOffsetv：无参，查当前 SetFile。 */
long GetAPKXFileLenv(void) {
    struct stat st;
    long ret = (g_apkx_current[0] && stat(g_apkx_current, &st) == 0)
                   ? (long)st.st_size : 0L;
    fprintf(stderr, "[shim:APKX] FileLenv('%s') -> %ld\n", g_apkx_current, ret);
    return ret;
}

long GetAPKXFileOffsetv(void) {
    fprintf(stderr, "[shim:APKX] FileOffsetv('%s') -> 0\n", g_apkx_current);
    return 0L;
}

/* ---- SMPEG_new_rwops：跳过过小片源，其余转发随包 libsmpeg2 ----
 * 真签名是 (SDL_RWops*, SMPEG_Info*, int freesrc, int sdl_audio)。
 * 旧桩把 SMPEG_status 做成立刻 STOPPED，开场会被当成播完。 */
static SMPEG *(*real_SMPEG_new_rwops)(SDL_RWops *, void *, int, int);
static const char *(*real_SMPEG_error)(SMPEG *);

static void shim_resolve_smpeg(void) {
    void *h;
    char path[2048];

    if (real_SMPEG_new_rwops)
        return;
    snprintf(path, sizeof(path), "%s/libsmpeg2.so", sw_gamedir());
    h = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
    if (!h)
        h = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    if (!h)
        h = dlopen("libsmpeg2.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[smpeg] dlopen libsmpeg2.so failed: %s\n", dlerror());
        return;
    }
    real_SMPEG_new_rwops =
        (SMPEG *(*)(SDL_RWops *, void *, int, int))dlsym(h, "SMPEG_new_rwops");
    real_SMPEG_error = (const char *(*)(SMPEG *))dlsym(h, "SMPEG_error");
    if (!real_SMPEG_new_rwops)
        fprintf(stderr, "[smpeg] dlsym SMPEG_new_rwops failed: %s\n", dlerror());
}

SMPEG *SMPEG_new_rwops(SDL_RWops *src, void *info, int freesrc, int sdl_audio) {
    Sint64 sz = 0;

    if (src && src->size)
        sz = src->size(src);
    /* PSV 同策略：nil.bik 一类空片会卡住解码器 */
    if (src && sz >= 0 && sz < 100 * 1024) {
        fprintf(stderr, "[smpeg] skip tiny rwops size=%lld\n", (long long)sz);
        if (freesrc)
            SDL_RWclose(src);
        return NULL;
    }

    shim_resolve_smpeg();
    if (!real_SMPEG_new_rwops) {
        if (freesrc && src)
            SDL_RWclose(src);
        return NULL;
    }

    SMPEG *mpeg = real_SMPEG_new_rwops(src, info, freesrc, sdl_audio);
    fprintf(stderr, "[smpeg] new_rwops size=%lld mpeg=%p%s%s\n",
            (long long)sz, (void *)mpeg,
            (mpeg && real_SMPEG_error && real_SMPEG_error(mpeg)) ? " err=" : "",
            (mpeg && real_SMPEG_error && real_SMPEG_error(mpeg))
                ? real_SMPEG_error(mpeg) : "");
    return mpeg;
}
