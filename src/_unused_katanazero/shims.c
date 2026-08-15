/* shims.c — shims do so-loader Sonic Mania (RSDKv5) p/ Mali-450.
 * Resolve os simbolos que o libsonicmania.so importa do bionic/NDK/cxx que a
 * glibc nao tem com o mesmo nome. Os pthread (create/rwlock/setspecific) sao
 * tipos COMPATIVEIS LP64 (so mutex diverge, e o RSDK nao importa mutex) ->
 * passthrough direto. cxx new/delete -> malloc/free. OpenSLES -> stub (audio
 * sai por SDL depois). __android_log_print -> stderr. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shims.h"
#include <dlfcn.h>
#include "opensles_shim.h"

/* ---- pthread bionic->glibc (tipos compativeis: passthrough) ---- */
int pthread_create_fake(pthread_t *t, const void *attr, void *(*f)(void *), void *arg) {
  (void)attr;
  return pthread_create(t, NULL, f, arg);
}
int pthread_rwlock_rdlock_fake(pthread_rwlock_t *l) { return pthread_rwlock_rdlock(l); }
int pthread_rwlock_wrlock_fake(pthread_rwlock_t *l) { return pthread_rwlock_wrlock(l); }
int pthread_rwlock_unlock_fake(pthread_rwlock_t *l) { return pthread_rwlock_unlock(l); }
int pthread_setspecific_fake(pthread_key_t k, const void *v) { return pthread_setspecific(k, v); }

/* ---- C++ runtime / operators ---- */
void *_Znwm(unsigned long n) { return malloc(n ? n : 1); }     /* operator new */
void *_Znam(unsigned long n) { return malloc(n ? n : 1); }     /* operator new[] */
void _ZdlPv(void *p) { free(p); }                              /* operator delete */
void _ZdaPv(void *p) { free(p); }                              /* operator delete[] */

int __cxa_atexit(void (*f)(void *), void *arg, void *dso) { (void)f; (void)arg; (void)dso; return 0; }
void __cxa_finalize(void *dso) { (void)dso; }
int __cxa_guard_acquire(uint64_t *g) { return !*(volatile uint8_t *)g; }
void __cxa_guard_release(uint64_t *g) { *(volatile uint8_t *)g = 1; }
void __cxa_guard_abort(uint64_t *g) { (void)g; }
void *__cxa_allocate_exception(unsigned long n) { return malloc(n ? n : 1); }
void __cxa_free_exception(void *p) { free(p); }
void __cxa_throw(void *e, void *t, void *d) { (void)e; (void)d;
  const char *tn = "?"; if (t) { const char *n = *(const char **)((char *)t + 8); if (n) tn = n; }
  fprintf(stderr, "[shim] __cxa_throw type=%s -> abort\n", tn); abort(); }
void *__cxa_begin_catch(void *e) { return e; }
void __cxa_end_catch(void) {}
void __cxa_pure_virtual(void) { fprintf(stderr, "[shim] pure_virtual -> abort\n"); abort(); }
int __gxx_personality_v0(int a, int b, unsigned long c, void *d, void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }

void __stack_chk_fail(void) { fprintf(stderr, "[shim] stack_chk_fail\n"); abort(); }

/* ---- liblog ---- */
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return 0;
}

/* ---- OpenSLES interface IDs (stub NULL; audio sai por SDL depois) ---- */
void *SL_IID_ENGINE_shim = 0;
void *SL_IID_PLAY_shim = 0;
void *SL_IID_RECORD_shim = 0;
void *SL_IID_BUFFERQUEUE_shim = 0;
void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim = 0;
void *SL_IID_ANDROIDCONFIGURATION_shim = 0;

/* ---- AAssetManager -> abre Data.rsdk e outros via fopen do cwd ---- */
#include <string.h>
void *AAssetManager_fromJava(void *env, void *mgr) { (void)env; (void)mgr; return (void *)0x1; }
void *AAssetManager_open(void *mgr, const char *name, int mode) {
  (void)mgr; (void)mode;
  if (!name) return NULL;
  FILE *f = fopen(name, "rb");
  /* GameMaker abre "assets/audiogroupN.dat" etc.; nossos arquivos estao no root
   * do port dir -> tenta tambem sem o prefixo "assets/" e o basename. */
  if (!f && strncmp(name, "assets/", 7) == 0) f = fopen(name + 7, "rb");
  if (!f && strncmp(name, "asset/", 6) == 0) f = fopen(name + 6, "rb");
  if (!f) { const char *b = strrchr(name, '/'); if (b) f = fopen(b + 1, "rb"); }
  fprintf(stderr, "[asset] open %s -> %p\n", name, (void *)f);
  return f;
}
long AAsset_getLength(void *a) { FILE *f = a; long c = ftell(f); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, c, SEEK_SET); return n; }
long AAsset_getLength64(void *a) { return AAsset_getLength(a); }
int AAsset_read(void *a, void *buf, size_t n) { size_t r=fread(buf,1,n,(FILE*)a); static int c=0; if(c++<3) fprintf(stderr,"[asset] read %zu->%zu\n",n,r); return (int)r; }
long AAsset_seek(void *a, long off, int whence) { fseek((FILE *)a, off, whence); return ftell((FILE *)a); }
const void *AAsset_getBuffer(void *a) {
  FILE *f = a; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  void *buf = malloc(n); if (buf) { if(fread(buf, 1, n, f)!=(size_t)n){} }
  fprintf(stderr, "[asset] getBuffer %ld bytes -> %p\n", n, buf);
  return buf;
}
void AAsset_close(void *a) { if (a) fclose((FILE *)a); }
/* ---- bionic _chk / misc ---- */
int __vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list ap) { (void)flag; (void)slen; return vsprintf(s, fmt, ap); }
int __vsnprintf_chk(char *s, size_t n, int flag, size_t slen, const char *fmt, va_list ap) { (void)flag; (void)slen; return vsnprintf(s, n, fmt, ap); }
char *__strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
char *__strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
char *__strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
void *__memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
int my_register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *d) { (void)a; (void)b; (void)c; (void)d; return 0; }

/* ---- 15 unresolved restantes (libc bionic + input) ---- */
extern int *__errno_location(void);
int *__errno(void) { return __errno_location(); }
size_t __strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
ssize_t __read_chk(int fd, void *b, size_t n, size_t bl) { (void)bl; extern long read(int,void*,size_t); return (int)read(fd,b,n); }
int __system_property_get(const char *k, char *v) { (void)k; if (v) v[0] = 0; return 0; }
void android_set_abort_message(const char *m) { (void)m; }
char g_sF_stub[3][256];
void *__sF = g_sF_stub;
int AInputEvent_getDeviceId(void *e) { (void)e; return 0; }
int AMotionEvent_getButtonState(void *e) { (void)e; return 0; }

/* ---- Oboe dlopen("libOpenSLES.so") -> opensles_shim (SDL2 audio) ---- */
#define SL_MAGIC ((void *)0x5151ABCDul)
void *my_dlopen(const char *name, int flag) {
  if (name && strstr(name, "OpenSLES")) { fprintf(stderr, "[sl] dlopen %s -> shim\n", name); return SL_MAGIC; }
  return dlopen(name, flag);
}
void *SL_IID_VOLUME_shim = 0;
void *my_dlsym(void *h, const char *name) {
  if (h == SL_MAGIC) {
    fprintf(stderr, "[sl] dlsym %s\n", name ? name : "?");
    if (!name) return NULL;
    if (strcmp(name, "slCreateEngine") == 0) return (void *)slCreateEngine_shim;
    /* SL_IID_* sao VARIAVEIS (ponteiros p/ o IID). dlsym devolve o ENDERECO da
     * variavel; o yyal le *addr p/ obter o IID. */
    if (strcmp(name, "SL_IID_ENGINE") == 0) return &SL_IID_ENGINE_shim;
    if (strcmp(name, "SL_IID_PLAY") == 0) return &SL_IID_PLAY_shim;
    if (strcmp(name, "SL_IID_VOLUME") == 0) return &SL_IID_VOLUME_shim;
    if (strcmp(name, "SL_IID_RECORD") == 0) return &SL_IID_RECORD_shim;
    if (strcmp(name, "SL_IID_BUFFERQUEUE") == 0) return &SL_IID_BUFFERQUEUE_shim;
    if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0) return &SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim;
    if (strcmp(name, "SL_IID_ANDROIDCONFIGURATION") == 0) return &SL_IID_ANDROIDCONFIGURATION_shim;
    return NULL;
  }
  void *r = dlsym(h, name);
  static int c = 0; if (!r && c++ < 60) fprintf(stderr, "[dl] dlsym(%p,%s) -> NULL\n", h, name ? name : "?");
  return r;
}

/* ---- bionic extras p/ Katana ZERO (GameMaker) ---- */
#include <unistd.h>
#include <fcntl.h>
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); return 0;
}
void __assert2_shim(const char *file, int line, const char *func, const char *expr) {
  fprintf(stderr, "[assert] %s:%d %s: %s\n", file ? file : "?", line, func ? func : "?", expr ? expr : "?");
  abort();
}
char *__strncpy_chk2_shim(char *d, const char *s, size_t n, size_t dl, size_t sl) { (void)dl; (void)sl; return strncpy(d, s, n); }
char *__strncat_chk_shim(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }
size_t __strlcpy_chk_shim(char *d, const char *s, size_t n, size_t dl) { (void)dl;
  size_t sl = strlen(s); if (n) { size_t c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = 0; } return sl; }
size_t __strlcat_chk_shim(char *d, const char *s, size_t n, size_t dl) { (void)dl;
  size_t dlen = strnlen(d, n), sl = strlen(s); if (dlen == n) return n + sl;
  size_t c = sl < n - dlen - 1 ? sl : n - dlen - 1; memcpy(d + dlen, s, c); d[dlen + c] = 0; return dlen + sl; }
char *__strrchr_chk_shim(const char *s, int c, size_t sl) { (void)sl; return strrchr(s, c); }
char *__strchr_chk_shim(const char *s, int c, size_t sl) { (void)sl; return strchr(s, c); }
int __open_2_shim(const char *path, int flags) { return open(path, flags); }
void __FD_SET_chk_shim(int fd, void *set, size_t sz) { (void)sz; FD_SET(fd, (fd_set *)set); }
void __FD_CLR_chk_shim(int fd, void *set, size_t sz) { (void)sz; FD_CLR(fd, (fd_set *)set); }
int __FD_ISSET_chk_shim(int fd, void *set, size_t sz) { (void)sz; return FD_ISSET(fd, (fd_set *)set); }
/* rwlock bionic-init: zera o slot (8B ptr) -> rw_real do bridge faz lazy-alloc */
int b_rwlock_init_shim(void *r, const void *attr) { (void)attr; *(void **)r = NULL; return 0; }
/* DIAG: wrappers de mem/str que logam args suspeitos (NULL/enorme) p/ caçar o crash */
int kz_memcmp(const void *a, const void *b, size_t n) {
  if (n > 0x1000000 || (!a && n) || (!b && n)) { static int c=0; if(c++<40) fprintf(stderr,"[memcmp!] a=%p b=%p n=%zu\n",a,b,n); }
  return memcmp(a, b, n);
}
size_t kz_strlen(const char *s) {
  if (!s) { static int c=0; if(c++<40) fprintf(stderr,"[strlen!] NULL\n"); return 0; }
  return strlen(s);
}
int kz_strcmp(const char *a, const char *b) {
  if (!a || !b) { static int c=0; if(c++<40) fprintf(stderr,"[strcmp!] a=%p b=%p\n",(void*)a,(void*)b); return a==b?0:(a?1:-1); }
  return strcmp(a, b);
}
/* getprogname: BSD/bionic; glibc nao tem -> sem shim a .so pula em lixo (crash) */
const char *getprogname(void) { return "katanazero"; }
void setprogname(const char *n) { (void)n; }

/* sigaction/signal: a libyoyo instala handler proprio p/ SIGSEGV/etc e re-raise
 * (raise(SIGSEGV) deliberado) -> mascara o crash real. Bloqueia a instalacao
 * nesses sinais p/ NOSSO crash_handler pegar o PC verdadeiro. Outros passam. */
#include <signal.h>
static int is_fatal_sig(int s) {
  return s == SIGSEGV || s == SIGBUS || s == SIGILL || s == SIGABRT || s == SIGFPE;
}
int my_sigaction(int sig, const void *act, void *old) {
  extern int sigaction(int, const struct sigaction *, struct sigaction *);
  if (is_fatal_sig(sig) && !getenv("KZ_ALLOW_SIGH")) {
    static int c = 0; if (c++ < 8) fprintf(stderr, "[sig] bloqueado sigaction(%d) da libyoyo\n", sig);
    if (old) memset(old, 0, sizeof(struct sigaction));
    return 0;
  }
  return sigaction(sig, (const struct sigaction *)act, (struct sigaction *)old);
}
/* syscall: bloqueia rt_sigaction/sigaltstack nos sinais fatais p/ a libyoyo NAO
 * instalar seu proprio crash-handler (que re-raise e mascara o PC real). aarch64:
 * rt_sigaction=134, sigaltstack=132, tgkill=131. Demais passam direto. */
#include <sys/syscall.h>
long my_syscall(long n, long a, long b, long c, long d, long e, long f) {
  if (!getenv("KZ_ALLOW_SIGH")) {
    if (n == 134 /*rt_sigaction*/) {
      int sig = (int)a;
      if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGABRT || sig == SIGFPE) {
        static int cc = 0; if (cc++ < 8) fprintf(stderr, "[sig] bloqueado syscall rt_sigaction(%d)\n", sig);
        return 0;
      }
    }
  }
  extern long syscall(long, ...);
  return syscall(n, a, b, c, d, e, f);
}

/* raise/abort: a libyoyo faz raise(SIGSEGV) deliberado (check de integridade?).
 * Logamos e CONTINUAMOS (return 0) p/ ver o fluxo real / pular a trava. */
int my_raise(int sig) {
  static int c = 0; if (c++ < 20) fprintf(stderr, "[raise] raise(%d) interceptado -> ignora (KZ_RAISE p/ permitir)\n", sig);
  if (getenv("KZ_RAISE")) { extern int raise(int); return raise(sig); }
  return 0;
}
void my_abort(void) {
  static int c = 0; if (c++ < 20) fprintf(stderr, "[abort] abort() interceptado -> ignora\n");
  if (getenv("KZ_ABORT")) { extern void abort(void); abort(); }
}
void *my_signal(int sig, void *h) {
  if (is_fatal_sig(sig) && !getenv("KZ_ALLOW_SIGH")) {
    static int c = 0; if (c++ < 8) fprintf(stderr, "[sig] bloqueado signal(%d) da libyoyo\n", sig);
    return NULL;
  }
  return (void *)signal(sig, (void (*)(int))h);
}
