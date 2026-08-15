# Sword3 so-loader — 技术交接文档 (HANDOFF)

> 端口：`com.softstar.G.swd3e`（仙剑奇侠传三，Softstar，SDL2 2D）
> 基线：summertimesaga so-loader 改写（同范式：加载 Android `.so` + 原生 shim）
> 重构：2026-07-31 架构清理（仅重组 loader 内部 + 修实机 bug + 完善构建/部署/文档，运行时行为等价）
> 日期：2026-07-31

## 0. 架构清理 / Bug 修复摘要（2026-07-31）

- **深度 = 架构清理**：保留运行时行为，重组 loader 内部加载编排（`main.c` 抽取
  `setup_device_sdl_video` / `load_secondary_libs` / `load_device_gles`），不重写 shim 逻辑。
- **Bug A 修复**：`main.c` 的 `SECONDARY_SOS[4]` 由设备版 `"/usr/lib/libSDL2_image-2.0.so.0"`
  改回随包 Android 版 `"libSDL2_image.so"`（设备版 `IMG_Load` 解码失败 → 黑屏）。
- **Bug B 修复**：新增部署期 `tools/patch_libs.py`（白名单 + 幂等 + `--verify`），把随包 Android `.so`
  的 `.gnu.version_r` 的 `LIBC` verneed 标 `WEAK`，解决 `dlopen` 报 `version LIBC not found`。
- **新增**：`tools/patch_libs.sh`（薄包装）、`deploy.sh`（build→staging→patch→推送指引）。
- **build_docker.sh**：链接命令逐字不变；新增 `liblog.so` 构建步骤（用 `stubs/liblog_stub.c`）。

## 1. 符号核对结论（docker readelf）

### 游戏自带 `libSDL2.so`（soname `libSDL2.so`，与设备 `libSDL2-2.0.so.0` 不冲突）
- **NEEDED**：`libhidapi, libdl, libGLESv1_CM, libGLESv2, libOpenSLES, liblog, libandroid, libc, libm`
- **UNDEFINED (248)**：`gl*`（GLESv1_CM+GLESv2）、`ANativeWindow*`、`OpenSLES`(`slCreateEngine`/`SL_IID_*`)、`ASensor*`/`ALooper*`、`hidapi`、`__android_log_*`、`__system_property_get`
- **无 `egl*` 符号** → R8（egl_shim 覆盖不全）风险被证伪：EGL 只经 `my_dlopen`/`my_dlsym` 运行时拦截，不需要独立 `libEGL.so` shim 文件。

### 主库 `libSWD3E.so`
- **NEEDED**：含 `libEGL.so` + SDL2 全家桶 + GLES + `libandroid` + `libc++_shared`
- **UNDEFINED (205)**：**纯** `SDL_*`/`Mix_*`/`TTF_*`/`IMG_*`/`SMPEG_*` + C++ 异常/RTTI + 标准 libc/m/dl + `Android_JNI_*`
- **无 `egl*` / 无 `ANative*` / 无 `OpenSLES` / 无 `SL_*` / 无 `ASensor`**（这些都在 `libSDL2.so` 内部）
- 结论：游戏自身不直调 EGL/GLES；是 Android 版 `libSDL2.so` 内部用 EGL→`egl_shim`→原生 SDL2，GLES→设备 Mali。

### 资源路径（`strings`）
- 见 `GetResourcePath`、`%s/Resource`、`getAssets`；**无硬编码** `/data/`/`/sdcard/`。
- loader 把 `ANDROID_APP_PATH/ANDROID_ARGUMENT/ANDROID_PRIVATE` 设为 `$GAMEDIR/assets`，
  命中 `SDL_RWFromFile("Resource/...", ...)`。

## 2. 编译修复记录

### 2.1 误用 katanazero 脚手架（已修正）
初始 `src/` 误入了 **katanazero** 端口的 3 个文件，与 summertimesaga 派生的 `imports.c`
（自包含，所有 shim 内联）冲突，导致链接期 multiple definition + 一个 undefined：
- `shims.c` / `shims.h`：重定义 `pthread_create_fake`、`AAssetManager_fromJava`、`__*_chk` 系列
- `imports.gen.c`：重定义 `dynlib_functions[]`（katanazero 的极小表）
- `pthread_bridge.c` / `pthread_bridge.h`：定义 `revc_pthread_table` / `b_mutex_*`，但 sword3 的
  `imports.c` 并不引用（summertimesaga 范式无 pthread bridge）

**修正**：把上述 5 文件移入 `src/_unused_katanazero/`（git-ignored，保留作反例参考），
`src/` 恢复为 summertimesaga 同款文件集。链接错误全部消失。

### 2.2 `glDrawTexfOES` 未定义引用
`imports.c` 的 `dynlib_functions[]` 含 `{"glDrawTexfOES", ...}` 表项，但该符号在 docker 镜像的
`libGLESv2` 中**不导出**（summertimesaga 原版依赖 LibreELEC 工具链的 `libGLESv2` 恰好导出它）。

**修正**：在 `imports.c` 内联一个可移植定义，运行时经 `dlsym(RTLD_DEFAULT, "glDrawTexfOES")`
解析并转发，不可用时静默 no-op（2D 游戏一般从不调用）。不依赖工具链 libGLESv2。

### 2.3 其余已解决（本轮之前）
- `egl_shim.c`：`summertime_screen_*`→`egl_shim_screen_*` 重命名对齐 `main.c` 的 extern；
  光标位置置 0（`summertime_cursor_x/y = 0`）；`g_summertime_screen_w/h` 同步。
- `main.c`：修正头部 `SDL_*/` 注释提早闭合 `*/` 引发的级联编译错误；`dynlib_numfunctions`→
  `dynlib_functions_count`；`p_JNI_OnLoad`/`p_nativeSetupJNI` 改为函数指针类型。
- 恢复 `etc1.c/.h`（`imports.c` 调用 `ss_etc1_*`）与 `pthread_bridge.c/.h`（后确认非必需，已弃用）。

### 2.4 构建产物
- `sword3`：aarch64 PIE（链接命令见 build_docker.sh，逐字不变）。
- `libbionic_shim.so`：glibc 编译，导出 `__sF@LIBC` + `Android_JNI_*`。
- `liblog.so`：**新增构建步骤**，用 `stubs/liblog_stub.c`（`-shared -fPIC -D_GNU_SOURCE`，strip）。
- 残留告警（无害）：`util.c:48 snprintf` 截断告警。

## 3. 运行期架构

```
launcher(sh) ──> sword3(bin)
   1) 设备 SDL_Init(VIDEO) + egl_shim_create_window()   ← 设备自选后端（fbdev/kmsdrm/wayland）
   2) setenv SDL_VIDEODRIVER=android / SDL_AUDIODRIVER=android   ← 仅给游戏内置静态 libSDL2
   3) RTLD_GLOBAL 预载 secondary .so（libbionic_shim/liblog 必须第一）：
        libSDL2.so / libSDL2_image.so(随包 Android 版！非设备版) / libSDL2_mixer.so /
        libSDL2_ttf.so / libsmpeg2.so / libhidapi.so / libc++_shared.so
      + 设备 libGLESv2/v1_CM/EGL
      ★ 所有随包 Android .so 须部署期 LIBC->WEAK（见 §6），否则 dlopen 失败
   4) so_load(libSWD3E.so) → so_relocate → so_resolve(dynlib_functions)
   5) 假 JavaVM/JNIEnv → JNI_OnLoad + nativeSetupJNI → SDL_main
   6) ANDROID_APP_PATH/ARGUMENT/PRIVATE = $GAMEDIR/assets
```

- 单实例清理、崩溃处理、performance governor 均在二进制内（`sw_kill_prior_instances` 等）。
- 铁律遵守：设备侧 SDL2 后端全程自选；`SDL_VIDEODRIVER=android` 只对游戏内部 libSDL2 生效。
- `libSDL2_image.so` 为随包 Android 版（**严禁**设备 `/usr/lib/libSDL2_image-2.0.so.0`，否则黑屏）。

## 4. 实机测试清单

- [ ] Mali-G31 / wayland：首帧出现、菜单中文、可进入游戏
- [ ] RK3562 / Mali-G52 / kmsdrm：同上
- [ ] 音频（BGM/音效）经 OpenSLES→SDL2 正常
- [ ] 过场动画（SMPEG）播放
- [ ] 返回键/音量键（Java_* 回调缺失 → 必要时 gptokeyb 映射）
- [ ] `LANG=zh_CN.UTF-8` 下繁简资源均加载；确认 `locale -a` 含 `zh_CN`/`zh_TW`
- [ ] `debug.log` 无 `so_resolve failed` / 缺符号报错
- [ ] 确认 `libSWD3E.so` 实际资源路径与 `assets/` 前缀一致（strings 未见硬编码绝对路径）
- [ ] 部署期 `tools/patch_libs.sh --verify $GAMEDIR` 返回 0（全 LIBC->WEAK）

## 5. 文件清单

```
ports/sword3/  (NextOs-Ports/sword3)
├── build_docker.sh          # aarch64 交叉编译（sword3 + libbionic_shim.so + liblog.so）；链接命令不变
├── deploy.sh                # [2026-07-31 新增] build→staging→patch→推送指引
├── sword3.sh                # 启动脚本（前台、单实例、中文 locale、SUMMERTIME_CURSOR=0）
├── README.md                # 端口说明（含随包/设备数据布局、patch 步骤、SDL2_image 禁设备版）
├── docs/
│   ├── REFACTOR_DESIGN.md   # 架构清理设计方案（规划产物）
│   ├── HANDOFF.md           # 本文件
│   └── PROGRESS.md          # 进展总结（原样携带）
├── src/                     # loader 源码（summertimesaga 派生）
│   ├── main.c               # [2026-07-31 架构清理重写] 加载编排拆分；修 SECONDARY_SOS(Bug A)；LIBC→WEAK 契约注释
│   ├── egl_shim.c  imports.c  jni_shim.c  opensles_shim.c  android_shim.c
│   ├── etc1.c  so_util.c  util.c  error.c  libbionic_shim.c  + *.h  hashmap.h
│   ├── libbionic_shim.vers  # version-script：导出 __sF@LIBC + Android_JNI_*
│   └── _unused_katanazero/  # 误用脚手架（git-ignored，反例）
├── stubs/
│   └── liblog_stub.c         # Android liblog.so 最小替身源码（构建 liblog.so 用）
└── tools/
    ├── patch_libs.py         # [2026-07-31 新增] ELF LIBC→WEAK（目录递归/白名单/幂等/--verify）
    └── patch_libs.sh         # [2026-07-31 新增] 薄包装：定位 python3 后调 patch_libs.py
```

## 6. 部署期 LIBC→WEAK（Bug B 修复，必须执行）

随包 Android `.so` 在 `.gnu.version_r` 把 `libc`/`libm` 标成 bionic 的 `LIBC` 版本节点，glibc 设备
没有 → `dlopen` 直接失败（`undefined symbol: free, version LIBC`）。部署期把它们标 `WEAK`：

```bash
# 主机：对含游戏 .so 的暂存目录编排（含 patch + 推送指引）
bash deploy.sh <staging-dir>
# 或直接：
bash tools/patch_libs.sh "$GAMEDIR"          # 默认白名单 9 个 Android .so；幂等可重复
bash tools/patch_libs.sh --verify "$GAMEDIR"  # 仅校验，返回 0=全 WEAK / 非0=有未弱化
```

- `tools/patch_libs.sh` 默认只 patch 白名单内 9 个随包 Android `.so`，`--all` 兜底任意含 `LIBC` 的 ELF。
- `libbionic_shim.so` / `liblog.so` 由 glibc 编译，无 `LIBC` verneed，不须 patch。
- `main.c` 不执行 ELF 改写；遗漏 patch 时 `load_secondary_libs` 的 `dlopen` 报 `version LIBC` 错误。
