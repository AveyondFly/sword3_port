/*
 * libc_compat_shim.c - LIBC version-node compatibility shim (Bug 1 root-cause fix).
 *
 * Background:
 *   The bundled Android .so files (libc++_shared / libsmpeg2 / libhidapi) record their
 *   libc/libm/libpthread symbol dependencies under the bionic "LIBC" version node in
 *   .gnu.version_r. The deploy step (tools/patch_libs.py) weakens that verneed to WEAK,
 *   but some embedded glibc builds do NOT honor WEAK version fallback and still reject
 *   the dlopen with "undefined symbol: free, version LIBC".
 *
 * Approach:
 *   This file is compiled together with libbionic_shim.c into libbionic_shim.so, which is
 *   preloaded first with RTLD_GLOBAL in main.c's load_secondary_libs. Here we define the
 *   "LIBC" version node and export every X@LIBC symbol that the three .so files actually
 *   depend on, forwarding each one straight to the real glibc / libpthread / libm
 *   implementation (no reimplementation, to avoid behavioural differences). Secondary
 *   dlopens then resolve X@LIBC against this .so and succeed.
 *
 * Implementation notes:
 *   - LIBC_FWD* macros implement each symbol as a thin wrapper whose body calls the
 *     same-named glibc function. This .so does NOT define an unversioned free/malloc/...,
 *     so the inner call resolves to glibc's real implementation (no recursion).
 *   - .symver aliases the wrapper to X@LIBC, paired with the LIBC node in
 *     src/libbionic_shim.vers. Each top-level .symver __asm__ carries a trailing ';'.
 *   - The param list (P) and call-arg list (A) are passed as PARENTHESIZED tuples so the
 *     macro receives them as single arguments; do NOT write bare comma lists here.
 *   - Variadic functions (printf/fprintf/...) are forwarded via va_list + the v* form.
 *   - Android-only symbols get a safe stub; __errno forwards to glibc __errno_location;
 *     strerror_r uses XSI semantics (returns int) to avoid clashing with glibc's GNU
 *     variant (which returns char*).
 *
 * This file only adds X@LIBC versioned exports. It does NOT touch the shim's existing
 * unversioned Android_JNI_* / fopen interposes, nor the load order in main.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>

/* These libc-internal symbols are not always declared by the headers; declare them
 * explicitly with glibc's real signatures to avoid implicit-declaration errors and
 * return-type mismatches. */
extern size_t __ctype_get_mb_cur_max(void);
extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);
extern void __cxa_finalize(void *dso_handle);
extern void __stack_chk_fail(void);

/* Forwarding macros.
 *   P = parameter declaration list (parenthesized), A = call-argument list (parenthesized,
 *       the parameter names). The wrapper body calls (Name)(A), which resolves to the real
 *       glibc implementation. Each wrapper is aliased to X@LIBC via .symver.
 *   A single 4-arg form (LIBC_FWD / LIBC_FWD0) is used for every symbol including
 *   void-returning ones (the body uses 'return (Name) A;', which is valid even when the
 *   inner call returns void). The two _Noreturn functions (exit/abort) are written out
 *   explicitly below. Each .symver __asm__ ends with ';' (required for top-level asm). */
#define LIBC_FWD(Ret, Name, P, A)                                        \
    Ret shim_##Name P { return (Name) A; }                               \
    __asm__(".symver shim_" #Name ", " #Name "@LIBC");

#define LIBC_FWD0(Ret, Name)                                             \
    Ret shim_##Name(void) { return (Name)(); }                          \
    __asm__(".symver shim_" #Name ", " #Name "@LIBC");

/* ===================== memory / allocation ===================== */
LIBC_FWD(void *, calloc, (size_t nmemb, size_t size), (nmemb, size))
LIBC_FWD(void, free, (void *ptr), (ptr))
LIBC_FWD(void *, malloc, (size_t size), (size))
LIBC_FWD(int, posix_memalign, (void **memptr, size_t alignment, size_t size),
         (memptr, alignment, size))
LIBC_FWD(void *, realloc, (void *ptr, size_t size), (ptr, size))

/* ===================== string (non-wide) ===================== */
LIBC_FWD(void *, memchr, (const void *s, int c, size_t n), (s, c, n))
LIBC_FWD(int, memcmp, (const void *s1, const void *s2, size_t n), (s1, s2, n))
LIBC_FWD(void *, memcpy, (void *dest, const void *src, size_t n), (dest, src, n))
LIBC_FWD(void *, memmove, (void *dest, const void *src, size_t n), (dest, src, n))
LIBC_FWD(void *, memset, (void *s, int c, size_t n), (s, c, n))
LIBC_FWD(int, strcmp, (const char *s1, const char *s2), (s1, s2))
LIBC_FWD(int, strcoll, (const char *s1, const char *s2), (s1, s2))
LIBC_FWD(char *, strdup, (const char *s), (s))
LIBC_FWD(size_t, strxfrm, (char *dest, const char *src, size_t n), (dest, src, n))
LIBC_FWD(size_t, strlen, (const char *s), (s))
LIBC_FWD(char *, strerror, (int errnum), (errnum))

/* ===================== wide char ===================== */
LIBC_FWD(wint_t, btowc, (int c), (c))
LIBC_FWD(size_t, mbrlen, (const char *s, size_t n, mbstate_t *ps), (s, n, ps))
LIBC_FWD(size_t, mbrtowc, (wchar_t *pwc, const char *s, size_t n, mbstate_t *ps),
         (pwc, s, n, ps))
LIBC_FWD(size_t, mbsnrtowcs, (wchar_t *dest, const char **src, size_t nms,
         size_t len, mbstate_t *ps), (dest, src, nms, len, ps))
LIBC_FWD(size_t, mbsrtowcs, (wchar_t *dest, const char **src, size_t len,
         mbstate_t *ps), (dest, src, len, ps))
LIBC_FWD(int, mbtowc, (wchar_t *pwc, const char *s, size_t n), (pwc, s, n))
LIBC_FWD(size_t, wcrtomb, (char *s, wchar_t wc, mbstate_t *ps), (s, wc, ps))
LIBC_FWD(int, wcscoll, (const wchar_t *w1, const wchar_t *w2), (w1, w2))
LIBC_FWD(size_t, wcslen, (const wchar_t *s), (s))
LIBC_FWD(wchar_t *, wcsncpy, (wchar_t *dest, const wchar_t *src, size_t n),
         (dest, src, n))
LIBC_FWD(size_t, wcsnrtombs, (char *dest, const wchar_t **src, size_t nwc,
         size_t len, mbstate_t *ps), (dest, src, nwc, len, ps))
LIBC_FWD(size_t, wcsxfrm, (wchar_t *dest, const wchar_t *src, size_t n),
         (dest, src, n))
LIBC_FWD(int, wctob, (wint_t c), (c))
LIBC_FWD(wchar_t *, wmemchr, (const wchar_t *s, wchar_t c, size_t n), (s, c, n))
LIBC_FWD(int, wmemcmp, (const wchar_t *s1, const wchar_t *s2, size_t n), (s1, s2, n))
LIBC_FWD(wchar_t *, wmemcpy, (wchar_t *dest, const wchar_t *src, size_t n),
         (dest, src, n))
LIBC_FWD(wchar_t *, wmemmove, (wchar_t *dest, const wchar_t *src, size_t n),
         (dest, src, n))
LIBC_FWD(wchar_t *, wmemset, (wchar_t *s, wchar_t c, size_t n), (s, c, n))

/* ===================== ctype / locale ===================== */
LIBC_FWD0(size_t, __ctype_get_mb_cur_max)
LIBC_FWD(int, islower, (int c), (c))
LIBC_FWD(int, isupper, (int c), (c))
LIBC_FWD(int, isxdigit, (int c), (c))
LIBC_FWD(int, iswalpha, (wint_t c), (c))
LIBC_FWD(int, iswblank, (wint_t c), (c))
LIBC_FWD(int, iswcntrl, (wint_t c), (c))
LIBC_FWD(int, iswdigit, (wint_t c), (c))
LIBC_FWD(int, iswlower, (wint_t c), (c))
LIBC_FWD(int, iswprint, (wint_t c), (c))
LIBC_FWD(int, iswpunct, (wint_t c), (c))
LIBC_FWD(int, iswspace, (wint_t c), (c))
LIBC_FWD(int, iswupper, (wint_t c), (c))
LIBC_FWD(int, iswxdigit, (wint_t c), (c))
LIBC_FWD(int, tolower, (int c), (c))
LIBC_FWD(int, toupper, (int c), (c))
LIBC_FWD(wint_t, towlower, (wint_t c), (c))
LIBC_FWD(wint_t, towupper, (wint_t c), (c))
LIBC_FWD(void, freelocale, (locale_t loc), (loc))
LIBC_FWD0(struct lconv *, localeconv)
LIBC_FWD(locale_t, newlocale, (int category_mask, const char *locale,
         locale_t base), (category_mask, locale, base))
LIBC_FWD(char *, setlocale, (int category, const char *locale), (category, locale))
LIBC_FWD(locale_t, uselocale, (locale_t loc), (loc))
LIBC_FWD(size_t, strftime, (char *s, size_t max, const char *format,
         const struct tm *tm), (s, max, format, tm))

/* ===================== stdio (non-variadic part) ===================== */
LIBC_FWD(FILE *, fdopen, (int fd, const char *mode), (fd, mode))
LIBC_FWD(int, fflush, (FILE *stream), (stream))
LIBC_FWD(int, fputc, (int c, FILE *stream), (c, stream))
LIBC_FWD(size_t, fwrite, (const void *ptr, size_t size, size_t nmemb,
         FILE *stream), (ptr, size, nmemb, stream))
LIBC_FWD(int, getc, (FILE *stream), (stream))
LIBC_FWD(int, puts, (const char *s), (s))
LIBC_FWD(void, perror, (const char *s), (s))
LIBC_FWD(void, openlog, (const char *ident, int option, int facility),
         (ident, option, facility))
LIBC_FWD0(void, closelog)
LIBC_FWD(int, ungetc, (int c, FILE *stream), (c, stream))

/* ===================== time / os ===================== */
LIBC_FWD(int, clock_gettime, (clockid_t clk_id, struct timespec *tp), (clk_id, tp))
LIBC_FWD(int, nanosleep, (const struct timespec *req, struct timespec *rem),
         (req, rem))
LIBC_FWD(ssize_t, read, (int fd, void *buf, size_t count), (fd, buf, count))
LIBC_FWD(int, close, (int fd), (fd))
LIBC_FWD0(int, sched_yield)
LIBC_FWD(long, sysconf, (int name), (name))

/* ===================== stdlib / numeric conversion ===================== */
/* exit / abort are _Noreturn; written out explicitly (no 'return') to avoid warnings. */
void shim_exit(int status) { exit(status); }
__asm__(".symver shim_exit, exit@LIBC");

void shim_abort(void) { abort(); }
__asm__(".symver shim_abort, abort@LIBC");

LIBC_FWD(double, strtod, (const char *nptr, char **endptr), (nptr, endptr))
LIBC_FWD(float, strtof, (const char *nptr, char **endptr), (nptr, endptr))
LIBC_FWD(long, strtol, (const char *nptr, char **endptr, int base),
         (nptr, endptr, base))
LIBC_FWD(long double, strtold, (const char *nptr, char **endptr), (nptr, endptr))
LIBC_FWD(long double, strtold_l, (const char *nptr, char **endptr, locale_t loc),
         (nptr, endptr, loc))
LIBC_FWD(long long, strtoll, (const char *nptr, char **endptr, int base),
         (nptr, endptr, base))
LIBC_FWD(long long, strtoll_l, (const char *nptr, char **endptr, int base,
         locale_t loc), (nptr, endptr, base, loc))
LIBC_FWD(unsigned long, strtoul, (const char *nptr, char **endptr, int base),
         (nptr, endptr, base))
LIBC_FWD(unsigned long long, strtoull, (const char *nptr, char **endptr,
         int base), (nptr, endptr, base))
LIBC_FWD(unsigned long long, strtoull_l, (const char *nptr, char **endptr,
         int base, locale_t loc), (nptr, endptr, base, loc))
LIBC_FWD(double, wcstod, (const wchar_t *nptr, wchar_t **endptr), (nptr, endptr))
LIBC_FWD(float, wcstof, (const wchar_t *nptr, wchar_t **endptr), (nptr, endptr))
LIBC_FWD(long, wcstol, (const wchar_t *nptr, wchar_t **endptr, int base),
         (nptr, endptr, base))
LIBC_FWD(long double, wcstold, (const wchar_t *nptr, wchar_t **endptr),
         (nptr, endptr))
LIBC_FWD(long long, wcstoll, (const wchar_t *nptr, wchar_t **endptr, int base),
         (nptr, endptr, base))
LIBC_FWD(unsigned long, wcstoul, (const wchar_t *nptr, wchar_t **endptr,
         int base), (nptr, endptr, base))
LIBC_FWD(unsigned long long, wcstoull, (const wchar_t *nptr, wchar_t **endptr,
         int base), (nptr, endptr, base))

/* ===================== math (libm) ===================== */
LIBC_FWD(double, exp2, (double x), (x))
LIBC_FWD(double, pow, (double x, double y), (x, y))

/* ===================== pthread (libpthread) ===================== */
LIBC_FWD(int, pthread_cond_broadcast, (pthread_cond_t *cond), (cond))
LIBC_FWD(int, pthread_cond_destroy, (pthread_cond_t *cond), (cond))
LIBC_FWD(int, pthread_cond_signal, (pthread_cond_t *cond), (cond))
LIBC_FWD(int, pthread_cond_timedwait, (pthread_cond_t *cond,
         pthread_mutex_t *mutex, const struct timespec *abstime),
         (cond, mutex, abstime))
LIBC_FWD(int, pthread_cond_wait, (pthread_cond_t *cond, pthread_mutex_t *mutex),
         (cond, mutex))
LIBC_FWD(int, pthread_create, (pthread_t *thread, const pthread_attr_t *attr,
         void *(*start_routine)(void *), void *arg),
         (thread, attr, start_routine, arg))
LIBC_FWD(int, pthread_detach, (pthread_t thread), (thread))
LIBC_FWD(int, pthread_equal, (pthread_t t1, pthread_t t2), (t1, t2))
LIBC_FWD(void *, pthread_getspecific, (pthread_key_t key), (key))
LIBC_FWD(int, pthread_join, (pthread_t thread, void **retval), (thread, retval))
LIBC_FWD(int, pthread_key_create, (pthread_key_t *key,
         void (*destructor)(void *)), (key, destructor))
LIBC_FWD(int, pthread_key_delete, (pthread_key_t key), (key))
LIBC_FWD(int, pthread_mutex_destroy, (pthread_mutex_t *mutex), (mutex))
LIBC_FWD(int, pthread_mutex_init, (pthread_mutex_t *mutex,
         const pthread_mutexattr_t *attr), (mutex, attr))
LIBC_FWD(int, pthread_mutex_lock, (pthread_mutex_t *mutex), (mutex))
LIBC_FWD(int, pthread_mutex_trylock, (pthread_mutex_t *mutex), (mutex))
LIBC_FWD(int, pthread_mutex_unlock, (pthread_mutex_t *mutex), (mutex))
LIBC_FWD(int, pthread_mutexattr_destroy, (pthread_mutexattr_t *attr), (attr))
LIBC_FWD(int, pthread_mutexattr_init, (pthread_mutexattr_t *attr), (attr))
LIBC_FWD(int, pthread_mutexattr_settype, (pthread_mutexattr_t *attr, int type),
         (attr, type))
LIBC_FWD(int, pthread_once, (pthread_once_t *once_control,
         void (*init_routine)(void)), (once_control, init_routine))
LIBC_FWD0(pthread_t, pthread_self)
LIBC_FWD(int, pthread_setspecific, (pthread_key_t key, const void *value),
         (key, value))

/* ===================== variadic / va_list forwarding ===================== */
int shim_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_printf, printf@LIBC");

int shim_fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_fprintf, fprintf@LIBC");

int shim_sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(str, fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_sprintf, sprintf@LIBC");

int shim_snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_snprintf, snprintf@LIBC");

int shim_sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_sscanf, sscanf@LIBC");

int shim_swprintf(wchar_t *wcs, size_t maxlen, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(wcs, maxlen, fmt, ap);
    va_end(ap);
    return r;
}
__asm__(".symver shim_swprintf, swprintf@LIBC");

void shim_syslog(int priority, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
}
__asm__(".symver shim_syslog, syslog@LIBC");

int shim_vfprintf(FILE *stream, const char *fmt, va_list ap) {
    return vfprintf(stream, fmt, ap);
}
__asm__(".symver shim_vfprintf, vfprintf@LIBC");

int shim_vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    return vsnprintf(str, size, fmt, ap);
}
__asm__(".symver shim_vsnprintf, vsnprintf@LIBC");

int shim_vsprintf(char *str, const char *fmt, va_list ap) {
    return vsprintf(str, fmt, ap);
}
__asm__(".symver shim_vsprintf, vsprintf@LIBC");

int shim_vsscanf(const char *str, const char *fmt, va_list ap) {
    return vsscanf(str, fmt, ap);
}
__asm__(".symver shim_vsscanf, vsscanf@LIBC");

int shim_vasprintf(char **strp, const char *fmt, va_list ap) {
    return vasprintf(strp, fmt, ap);
}
__asm__(".symver shim_vasprintf, vasprintf@LIBC");

/* ===================== C++ / runtime / libc-internal (special) ===================== */

/* open: forward to glibc real open64 (NOT the interpose open, to avoid recursion).
 * Versioned as open@LIBC so libc++_shared's versioned open reference resolves here.
 * Variadic flags/... args are collapsed to (path, flags, mode) like the interpose. */
int shim_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    return open64(path, flags, mode);
}
__asm__(".symver shim_open, open@LIBC");

/* __errno: bionic returns int*; glibc equivalent is __errno_location (also int*). */
int *__errno(void) { return __errno_location(); }
__asm__(".symver __errno, __errno@LIBC");

/* __cxa_atexit / __cxa_finalize / __stack_chk_fail: forward to glibc real impl. */
int shim___cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    return __cxa_atexit(func, arg, dso_handle);
}
__asm__(".symver shim___cxa_atexit, __cxa_atexit@LIBC");

void shim___cxa_finalize(void *dso_handle) { __cxa_finalize(dso_handle); }
__asm__(".symver shim___cxa_finalize, __cxa_finalize@LIBC");

void shim___stack_chk_fail(void) { __stack_chk_fail(); }
__asm__(".symver shim___stack_chk_fail, __stack_chk_fail@LIBC");

/* android_set_abort_message: bionic-only; glibc has no equivalent. Safe no-op stub. */
void android_set_abort_message(const char *msg) { (void)msg; }
__asm__(".symver android_set_abort_message, android_set_abort_message@LIBC");

/* dl_iterate_phdr: provided by glibc; forward as-is. */
int shim_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
                         void *data) {
    return dl_iterate_phdr(cb, data);
}
__asm__(".symver shim_dl_iterate_phdr, dl_iterate_phdr@LIBC");

/* strerror_r: bionic uses XSI semantics (returns int: 0 ok / -1 fail, fills buf).
 * glibc default is GNU semantics (returns char*). Forwarding glibc's GNU variant
 * directly would make the caller misinterpret a pointer as an int. Implement XSI
 * semantics here, reusing glibc's strerror to fill the buffer. */
int shim_strerror_r(int errnum, char *buf, size_t buflen) {
    const char *s = strerror(errnum);
    if (s == NULL) return -1;
    strncpy(buf, s, buflen);
    if (buflen > 0) buf[buflen - 1] = '\0';
    return 0;
}
__asm__(".symver shim_strerror_r, strerror_r@LIBC");

/* ---- extra X@LIBC for Android SDL2_image / mixer / ttf ----
 * 这三份库比 libc++_shared 多要 longjmp/strncpy/fopen 等。不提供则
 * dlopen 报 "undefined symbol: longjmp, version LIBC"。fopen 等已有
 * unversioned interpose，这里只加 @LIBC 别名，不进 version script。 */
FILE *shim_fopen(const char *path, const char *mode) { return fopen(path, mode); }
__asm__(".symver shim_fopen, fopen@LIBC");
int shim_fclose(FILE *fp) { return fclose(fp); }
__asm__(".symver shim_fclose, fclose@LIBC");
size_t shim_fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    return fread(ptr, size, nmemb, fp);
}
__asm__(".symver shim_fread, fread@LIBC");
int shim_fseek(FILE *fp, long off, int whence) { return fseek(fp, off, whence); }
__asm__(".symver shim_fseek, fseek@LIBC");
int shim_fseeko(FILE *fp, off_t off, int whence) { return fseeko(fp, off, whence); }
__asm__(".symver shim_fseeko, fseeko@LIBC");
long shim_ftell(FILE *fp) { return ftell(fp); }
__asm__(".symver shim_ftell, ftell@LIBC");
off_t shim_ftello(FILE *fp) { return ftello(fp); }
__asm__(".symver shim_ftello, ftello@LIBC");
int shim_ferror(FILE *fp) { return ferror(fp); }
__asm__(".symver shim_ferror, ferror@LIBC");
int shim_feof(FILE *fp) { return feof(fp); }
__asm__(".symver shim_feof, feof@LIBC");
int shim_fileno(FILE *fp) { return fileno(fp); }
__asm__(".symver shim_fileno, fileno@LIBC");

int shim_setjmp(jmp_buf env) { return _setjmp(env); }
__asm__(".symver shim_setjmp, setjmp@LIBC");
void shim_longjmp(jmp_buf env, int val) { longjmp(env, val); }
__asm__(".symver shim_longjmp, longjmp@LIBC");

LIBC_FWD(double, atof, (const char *nptr), (nptr))
LIBC_FWD(double, frexp, (double x, int *exp), (x, exp))
LIBC_FWD(double, modf, (double x, double *iptr), (x, iptr))
LIBC_FWD(double, log, (double x), (x))
LIBC_FWD(long, lround, (double x), (x))
LIBC_FWD(char *, strncpy, (char *dest, const char *src, size_t n), (dest, src, n))
LIBC_FWD(char *, strcpy, (char *dest, const char *src), (dest, src))
LIBC_FWD(char *, strcat, (char *dest, const char *src), (dest, src))
LIBC_FWD(int, strncmp, (const char *s1, const char *s2, size_t n), (s1, s2, n))
LIBC_FWD(char *, strstr, (const char *hay, const char *needle), (hay, needle))
LIBC_FWD(char *, strrchr, (const char *s, int c), (s, c))
LIBC_FWD(char *, strtok_r, (char *str, const char *delim, char **save),
         (str, delim, save))
LIBC_FWD(char *, getenv, (const char *name), (name))
LIBC_FWD(int, remove, (const char *path), (path))
LIBC_FWD(int, unlink, (const char *path), (path))
LIBC_FWD0(pid_t, getpid)
LIBC_FWD(struct tm *, gmtime, (const time_t *t), (t))
LIBC_FWD(void, qsort, (void *base, size_t nmemb, size_t size,
         int (*cmp)(const void *, const void *)), (base, nmemb, size, cmp))
LIBC_FWD(int, fstat, (int fd, struct stat *st), (fd, st))
LIBC_FWD(int, sigemptyset, (sigset_t *set), (set))
