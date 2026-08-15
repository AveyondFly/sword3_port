/*
 * liblog_stub.c — Android liblog.so 的最小替身（glibc/Linux 环境用）。
 *
 * 仙剑三的 Android .so（libhidapi / libSDL2_mixer / libSDL2_ttf ...）在 NEEDED
 * 里声明 liblog.so，且运行期会调用 __android_log_*。Linux 设备没有 liblog，
 * 这里提供一个只把日志转发到 stderr 的桩，避免 dlopen/符号解析失败。
 *
 * 仅导出 NDK liblog 的公共符号；实现全部转发到 fprintf(stderr)，不依赖 Android。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define LOG_TAG "[liblog-stub]"

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s[%s] ", LOG_TAG, tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    fprintf(stderr, "%s[%s] ", LOG_TAG, tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
    fprintf(stderr, "%s[%s] %s\n", LOG_TAG, tag ? tag : "", text ? text : "");
    return 0;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s[%s] ASSERT(%s): ", LOG_TAG, tag ? tag : "", cond ? cond : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

int __android_log_bwrite(int32_t tag, const void *payload, size_t len) {
    (void)tag; (void)payload; (void)len;
    return 0;
}

int __android_log_btwrite(int32_t tag, uint32_t type, const void *payload, size_t len) {
    (void)tag; (void)type; (void)payload; (void)len;
    return 0;
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...) {
    (void)bufID;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%s[%s] ", LOG_TAG, tag ? tag : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 0;
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text) {
    (void)bufID;
    fprintf(stderr, "%s[%s] %s\n", LOG_TAG, tag ? tag : "", text ? text : "");
    return 0;
}

void __android_log_logd_logger(int prio, const char *tag, const char *msg) {
    fprintf(stderr, "%s[%s] %s\n", LOG_TAG, tag ? tag : "", msg ? msg : "");
}
