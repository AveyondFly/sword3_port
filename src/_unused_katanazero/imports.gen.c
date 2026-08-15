// imports.gen.c — Katana ZERO (libyoyo.so, GameMaker/YYC).
// Tabela MÍNIMA: só os símbolos bionic-específicos (shims) + o bridge de
// pthread (mutex/cond/rwlock bionic->glibc). TODO o resto (libc/libm/libz,
// pthread_create/key/self, e o GL que a .so resolve via dlsym em runtime)
// cai no fallback dlsym(RTLD_DEFAULT) do so_resolve. Ver shims.c/pthread_bridge.c.
#include "imports.h"
#include "so_util.h"
#include "shims.h"
#include "android_shim.h"
#include "pthread_bridge.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

DynLibFunction dynlib_functions[] = {
  // ---- mem/str explicitos (evita ambiguidade do RTLD_DEFAULT/ifunc) ----
  {"memcmp", (uintptr_t)&kz_memcmp},
  {"memcpy", (uintptr_t)&memcpy},
  {"memmove", (uintptr_t)&memmove},
  {"memset", (uintptr_t)&memset},
  {"memchr", (uintptr_t)&memchr},
  {"strcmp", (uintptr_t)&kz_strcmp},
  {"strncmp", (uintptr_t)&strncmp},
  {"strlen", (uintptr_t)&kz_strlen},
  {"strcpy", (uintptr_t)&strcpy},
  {"strncpy", (uintptr_t)&strncpy},
  {"strcat", (uintptr_t)&strcat},
  {"strchr", (uintptr_t)&strchr},
  {"strrchr", (uintptr_t)&strrchr},
  {"strstr", (uintptr_t)&strstr},
  {"strdup", (uintptr_t)&strdup},
  // ---- bionic stdio/errno/log ----
  {"__sF", (uintptr_t)&__sF},
  {"__errno", (uintptr_t)&__errno},
  {"__android_log_print", (uintptr_t)&__android_log_print},
  {"__android_log_vprint", (uintptr_t)&__android_log_vprint},
  {"android_set_abort_message", (uintptr_t)&android_set_abort_message},
  {"__system_property_get", (uintptr_t)&__system_property_get},
  {"__assert2", (uintptr_t)&__assert2_shim},
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},
  {"__register_atfork", (uintptr_t)&my_register_atfork},
  {"getprogname", (uintptr_t)&getprogname},
  {"setprogname", (uintptr_t)&setprogname},
  {"sigaction", (uintptr_t)&my_sigaction},
  {"signal", (uintptr_t)&my_signal},
  {"raise", (uintptr_t)&my_raise},
  {"abort", (uintptr_t)&my_abort},
  {"syscall", (uintptr_t)&my_syscall},

  // ---- bionic _FORTIFY _chk family ----
  {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
  {"__memset_chk", (uintptr_t)&__memset_chk},
  {"__memmove_chk", (uintptr_t)&__memmove_chk},
  {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
  {"__strcat_chk", (uintptr_t)&__strcat_chk},
  {"__strncpy_chk", (uintptr_t)&__strncpy_chk},
  {"__strncpy_chk2", (uintptr_t)&__strncpy_chk2_shim},
  {"__strncat_chk", (uintptr_t)&__strncat_chk_shim},
  {"__strlen_chk", (uintptr_t)&__strlen_chk},
  {"__strlcpy_chk", (uintptr_t)&__strlcpy_chk_shim},
  {"__strlcat_chk", (uintptr_t)&__strlcat_chk_shim},
  {"__strrchr_chk", (uintptr_t)&__strrchr_chk_shim},
  {"__strchr_chk", (uintptr_t)&__strchr_chk_shim},
  {"__read_chk", (uintptr_t)&__read_chk},
  {"__open_2", (uintptr_t)&__open_2_shim},
  {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
  {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
  {"__FD_SET_chk", (uintptr_t)&__FD_SET_chk_shim},
  {"__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk_shim},
  {"__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_shim},

  // ---- AAssetManager -> fopen (game.droid + audiogroups) ----
  {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava},
  {"AAssetManager_open", (uintptr_t)&AAssetManager_open},
  {"AAsset_getLength", (uintptr_t)&AAsset_getLength},
  {"AAsset_getLength64", (uintptr_t)&AAsset_getLength64},
  {"AAsset_read", (uintptr_t)&AAsset_read},
  {"AAsset_seek", (uintptr_t)&AAsset_seek},
  {"AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer},
  {"AAsset_close", (uintptr_t)&AAsset_close},

  // ---- dlopen/dlsym -> intercepta libOpenSLES (audio via SDL) ----
  {"dlopen", (uintptr_t)&my_dlopen},
  {"dlsym", (uintptr_t)&my_dlsym},

  // ---- pthread: BRIDGE ABI bionic->glibc (mutex/cond/rwlock = tamanho divergente) ----
  {"pthread_create", (uintptr_t)&pthread_create_fake},
  {"pthread_mutex_init", (uintptr_t)&b_mutex_init},
  {"pthread_mutex_destroy", (uintptr_t)&b_mutex_destroy},
  {"pthread_mutex_lock", (uintptr_t)&b_mutex_lock},
  {"pthread_mutex_unlock", (uintptr_t)&b_mutex_unlock},
  {"pthread_mutex_trylock", (uintptr_t)&b_mutex_trylock},
  {"pthread_cond_init", (uintptr_t)&b_cond_init},
  {"pthread_cond_destroy", (uintptr_t)&b_cond_destroy},
  {"pthread_cond_wait", (uintptr_t)&b_cond_wait},
  {"pthread_cond_timedwait", (uintptr_t)&b_cond_timedwait},
  {"pthread_cond_signal", (uintptr_t)&b_cond_signal},
  {"pthread_cond_broadcast", (uintptr_t)&b_cond_broadcast},
  {"pthread_mutexattr_init", (uintptr_t)&b_mutexattr_init},
  {"pthread_mutexattr_destroy", (uintptr_t)&b_mutexattr_destroy},
  {"pthread_mutexattr_settype", (uintptr_t)&b_mutexattr_settype},
  {"pthread_rwlock_init", (uintptr_t)&b_rwlock_init_shim},
  {"pthread_rwlock_rdlock", (uintptr_t)&b_rwlock_rdlock},
  {"pthread_rwlock_wrlock", (uintptr_t)&b_rwlock_wrlock},
  {"pthread_rwlock_unlock", (uintptr_t)&b_rwlock_unlock},
  {"pthread_once", (uintptr_t)&b_once},
};
size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
