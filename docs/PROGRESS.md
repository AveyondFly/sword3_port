# 轩辕剑3天之痕 (Sword3) NextOS so-loader — 进展总结

> 端口：`com.softstar.G.swd3e`（大宇 Softstar，纯 SDL2 2D 移植版）
> 基线：summertimesaga so-loader 范式改写（加载 Android `.so` + 原生 shim 桥接）
> 仓库：`nextos-my-ports`（gitee `windstarry/ports_android`，master → origin/master）
> 最后更新：**2026-07-28 18:40 (GMT+8)**

---

## 0. TL;DR

- **Loader 已编译通过**：`sword3` = **181584B** aarch64 PIE + `libbionic_shim.so` = **18568B**（git-ignored 二进制，仅入库源码）。
- **资源路径系统已完全打通**：8 个 `hook_arm64` 全部命中，171 条 `[swpath]` 拼出正确路径 `$GAMEDIR/assets/Resource/<name>`，空 path 崩溃根因已彻底消除。
- **PNG 数据已证完好**：332 条 `[shim:PNG_RW]` 诊断全部 `PNG-OK`（`magic=89504e47` + 正确 `size`）→ 100% 排除数据损坏/偏移/加密（NOT-PNG 假设证伪）。解码失败纯属 RWops `seek` 语义问题。
- **修复已实施（待回归）**：游戏自构造 FILE-backed RWops 的 `seek` 回调脆弱（首次 `SDL_RWseek(...,RW_SEEK_END)` 直接 `CRASH sig=11`）。已重写 `IMG_LoadPNG_RW`：从当前位置 `slurp` 到 EOF 进增长缓冲（不 seek-to-end），用天生可 seek 的 `SDL_RWFromMem` 调 `IMG_Load_RW`。已重建（sword3 181584B / shim 18568B），**待实机回传 `debug.log` 确认 `decode OK` 且主菜单渲染**。

---

## 1. 可行性分析（架构评审，已完成）

对抽取目录（`build/sword3/`）做 ELF/strings 侦察：

| 目标 | 结论 |
|------|------|
| `libSWD3E.so`（主库） | ELF64 LSB shared object，**ARM aarch64 / Android 21 / NDK r20 / stripped**；入口 **`SDL_main`**；6 个 `Java_com_softstar_G_swd3e_SDLActivity_*` 回调（无 Java 时无害死代码） |
| NEEDED | `libSDL2 / libSDL2_image / libSDL2_mixer / libSDL2_ttf / libsmpeg2 / libc++_shared / libGLESv1_CM / libGLESv2 / liblog / libandroid / libEGL` + 标准 libc/m/dl |
| UNDEFINED (204) | **纯** `SDL_*`/`Mix_*`/`TTF_*`/`IMG_*`/`SMPEG_*` + C++ 异常/RTTI + 标准 libc + `Android_JNI_*`；**无裸 `egl*`/`ANative*`/`OpenSLES`/`SL_*`**（都在游戏自带 `libSDL2.so` 内部） |
| 资源路径 | `strings` 见 `GetResourcePath` / `%s/Resource` / `getAssets`；**无硬编码** `/data/`/`/sdcard/` |
| 游戏自带 `libSDL2.so` | soname=`libSDL2.so`，与设备 `libSDL2-2.0.so.0` **不冲突**；无 `egl*` 符号（EGL 仅经 `my_dlsym` 运行时拦截，证伪 R8 风险） |

**架构师可行性结论 = HIGH**：纯 SDL2 2D，所有 NEEDED 落在框架 shim 覆盖范围内；入口 `SDL_main` 无需 Java；无源码 → 只能 so-loader 路线。风险登记 10 项（R1 C++ 运行时错配[高] / R2 SDL2 后端 / R3 音频 / R4 SMPEG 视频 / R5 资源路径 / R6 Java 回调 / R7 仅 arm64 / R8 egl_shim 覆盖 / R9 Android libSDL2 差 / R10 资源完整性）。

---

## 2. Loader 编译与骨架（已完成）

- **误用 katanazero 脚手架**：初始 `src/` 误入 `shims.c`/`imports.gen.c`/`pthread_bridge.c`，与 summertimesaga 派生、自包含的 `imports.c` 重定义冲突。已移入 `src/_unused_katanazero/`（git-ignored 反例），`src/` 恢复 summertimesaga 同款文件集。
- **`glDrawTexfOES` 未定义引用**：docker 镜像 `libGLESv2` 不导出该 OES 扩展。改为 `imports.c` 内联运行时 `dlsym(RTLD_DEFAULT,"glDrawTexfOES")` + no-op 兜底。
- **构建**：`bash build_docker.sh`（镜像 `ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest`），链接 `-lSDL2 -lGLESv2 -lGLESv1_CM -ldl -lm -lpthread -lstdc++ -lgcc_s -rdynamic`。产物 `sword3`(181584B) / `libbionic_shim.so`(18544B)。
- **交付**：`sword3.sh`（启动脚本，前台/单实例/中文 locale/`SUMMERTIME_CURSOR=0`/绝不强制 `SDL_VIDEODRIVER`）、`README.md`、`docs/HANDOFF.md`。
- 同步陷阱已记录：新 build 在 `ports/sword3/`，而 `deploy_run.py` 的 `LOCAL_DIR=test/sword3/sword3` 只上传该处；须 `cp -f ports/sword3/sword3 test/sword3/sword3/sword3` + shim 同步，避免上传陈旧 loader。

---

## 3. 资源路径崩溃根因 → 修复（已完成，已实机验证）

**症状**：早期实机 `debug.log` 显示 `[shim:IMG] src=(nil)`、`Unable to load image file:` 文件名空、`fCreateFile error:` 路径空 → `LoadImageFile` 收空 path 直接 `IMG_Load_RW(NULL)` 短路，根本不调任何文件 open（`[shim:RW]/[shim:BMP]/[shim:fopen]/[shim:open]` 全 0 命中）。

**根因**：glibc 上 `GetAPKXPath`/`GetSDCardPath`/`GetInterPath`/`GetGAMESAVEPath`/`fileIO::GetResourcePath` 等资源根 getter 经 JNI 返回空串 → 游戏拼出空资源路径。

**修复**（`src/libbionic_shim.c` + `src/main.c`）：用 `hook_arm64` 在 `so_finalize()` 前（text RWX）改写 8 个游戏内函数入口，跳到 shim 本地实现：
- `GetAPKXPath`/`GetAPKXreadpath` → `$GAMEDIR/assets`
- `GetSDCardPath`/`GetTargetPath`/`GetInterPath`/`GetGAMESAVEPath` → `$GAMEDIR`
- `fileIO_GetResourcePath(name)` → `$GAMEDIR/assets/Resource/<name>`（按实例方法约定 `this`(x0)/`name`(x1)，忽略 this）
- `APKXAndroid_JNI_FileOpen` → 相对名映射到 `$GAMEDIR/assets` 树

关键点：`so_find_addr_safe` 用 **mangled** 名（精确匹配 dynsym），`dlsym(RTLD_DEFAULT)` 用 shim **unmangled** 名。**mangled 名勘误**：`GetInterPath` 真实导出是 `_Z12GetInterPathPc`（非 `_Z13`）。另 elfscan 证实 `GetResourceAPKX`/`GetResourceFile` 在动态符号表 NOT FOUND（libSWD3E 内部静态函数，无需 hook）。

**实机验证结果**：8 个 `[hook]` 全部打出；`GetResourcePath` → 171 条 `[swpath]`，路径正确拼成 `/storage/roms/ports/sword3/assets/Resource/<name>`，资源文件全部存在且大小正常（dpad_dl.png 122KB、CT.ttf 6.6MB、big5ToUtf8.bin 83KB）。**空 path 短路已彻底消失**。

---

## 4. PNG 解码失败（当前阻塞点）

### 4.1 现象
路径打通后，游戏 `fopen` 真实 PNG 成功、把数据喂给解码器，但 `surf=(nil)` → `SDL_CreateTextureFromSurface` 空 surface 崩（`LR=libSWD3E.so+0x1428c4`）。`[shim:FP]/[shim:RW]/[shim:BMP]/[shim:MEM]` **全 0 命中** → 游戏不走 SDL 的标准 RWops 构造器，而是自己 fopen + 自构造 FILE-backed RWops 直接喂解码。

### 4.2 诊断时间线（逐步排除，含已证伪的假设）
1. **空 path 已排除**（§3）。`fopen` 成功拿到真实 FILE*。
2. **系统 libSDL2_image 解码正常（隔离测试 `test/imgtest/imgtest.c` 证明）**：同设备 `dlopen` 系统 `/usr/lib/libSDL2_image-2.0.so.0` + `IMG_Load(BattleComm.png)` → 80×80 bpp=32 成功。**排除 "libpng 缺失"**（那种会报 "Unsupported image format"）。
3. **自带 Android `libSDL2_image.so` 不可用**：NDK 编的，动态依赖 `LIBC` 版本节点（glibc 不提供）→ 在 glibc 上 libpng 解码异常报 "PNG not supported: unknown PNG chunk type"。**已切换** main.c `SECONDARY_SOS`：`"libSDL2_image.so"` → `"/usr/lib/libSDL2_image-2.0.so.0"`，设备端把自带版重命名为 `.android`。
4. **游戏从不查 `IMG_Load` 符号**（LD_DEBUG=symbols + `IMG_Load` 拦截器 0 命中双重证明）→ 续4 曾尝试的 `IMG_Load_RW` 内存重解拦截器**从未命中，已移除**（游戏解码路径不经 `IMG_Load`/`IMG_Load_RW`，而是经内部符号 `IMG_LoadPNG_RW`）。
5. **游戏确实 fopen + 读到完整正确字节**（`fopen`/`open`/`fread` 诊断）：游戏以有效、各异的 FILE* 打开每张 PNG；`fread` 显示 `want==got`、0 条 `EMPTY` → **磁盘数据 100% 正确进入内存**。
6. **最后一道诊断（`IMG_LoadPNG_RW` 拦截器，已回传验证）**：游戏经内部 `IMG_LoadPNG_RW` 把数据喂给系统解码器。拦截器打印该 RWops 的 `size` + 前 8 字节 `magic`，实机回传的 `debug.log` 显示 **332 条全部 `PNG-OK`**（`magic=89504e47` + 正确 `size`）→ **数据 100% 完好**，彻底排除损坏/偏移/加密（NOT-PNG 假设证伪）。解码失败纯属 RWops `seek` 语义问题。
   - **关键副产物**：在该诊断版 `IMG_LoadPNG_RW` 内部用 `SDL_RWseek(src,0,RW_SEEK_END)` 取长度时，**游戏 RWops 的 seek 回调在 SEEK_END 上直接 `CRASH sig=11`**（首图 BattleComm.png 即崩，0 条 `decode` 打印）——证明游戏自构造 RWops 的 seek 实现脆弱，不能依赖 seek-to-end。

### 4.3 当前代码状态（`src/libbionic_shim.c`）
- 全局遮挡：`fopen`/`open`/`fread`/`SDL_RWFromFile`/`IMG_Load`/`IMG_LoadPNG_RW`（转发 `dlsym(RTLD_NEXT)`，跳过 shim 自身命中 libc/系统库）。
- `fopen`/`open` 仅对 `.png` 打诊断；`fread` 对登记 png FILE* 且 `want≥1024` 打印 `want/got`。
- `IMG_LoadPNG_RW`（**修复版**）：不依赖 seek，从当前位置 `slurp` 到 EOF 进增长缓冲，包 `SDL_RWFromMem` 调 `real_IMG_Load_RW=dlsym(RTLD_NEXT,"IMG_Load_RW")`；打印 `[shim:PNG_RW] enter` + `decode OK/FAIL` 供回归确认。（早期诊断版曾打印 size+magic，已确认数据完好后替换为修复版。）
- 资源根 hook、`Android_JNI_*`、`__sF@LIBC`、`SMPEG_*` 桩（过场空操作，跳过不播）保留。

### 4.4 待确认（回归验证）
用新 loader（18:38 重建）重跑实机，读取 `debug.log`：
- **`[shim:PNG_RW] decode OK` 出现且 `CRASH sig=11`/`Unable to load image` 消失** → 阻塞点解除，进主菜单渲染验证（Mali-G31 wayland / RK3562-Mali-G52 kmsdrm）；
- **仍 `decode FAIL` 或崩溃** → 退路：定位 `SDL_SS2D_LoadImage`（libSWD3E 内部 stripped 函数，借 `.rodata` 字符串引用 + 反汇编定位）hook 到系统 `IMG_Load(filename)` 强制解码。

---

## 5. 已知限制 / 次要待办（非阻塞）

- **`/Setting/env2.dat` / `/Setting/env.dat` 绝对路径 fopen→NULL**：应为 `$GAMEDIR/Setting/...`，存档/配置路径待处理（崩溃后再收）。
- **末尾空 path `[shim:fopen] '' -> (nil)` 多次**：`GetResourcePath` 之外的来源，待观察。
- **SMPEG 过场动画**：`libsmpeg2` 在 glibc 上无法加载（LIBC 版本节点），当前 `SMPEG_*` 安全空操作桩让游戏跳过过场；过场暂不播放（后续可换真实 `libsmpeg2` 或重编）。
- **Java_* 回调丢失**：返回键/音量键依赖 `Java_com_softstar_G_swd3e_SDLActivity_*` 回调，无 Java 时失效 → 必要时 gptokeyb 映射。
- **仅 arm64**：设备须 aarch64。

---

## 6. 下一步行动（按优先级）

1. **实机回归验证 `IMG_LoadPNG_RW` 修复**：用户本机 `cp -f ports/sword3/sword3 test/sword3/sword3/sword3` + shim 同步后 `python test/deploy_run.py` 抓 `debug.log`，确认 `[shim:PNG_RW] decode OK` 且 `CRASH sig=11` 消失。
2. 若仍 FAIL/崩溃 → 退路 hook `SDL_SS2D_LoadImage` 强制系统 `IMG_Load(filename)`（定位见 §4.4）。
3. 解码通后实机验证主菜单渲染（Mali-G31 wayland / RK3562-Mali-G52 kmsdrm）。
4. 处理 §5 次要项（存档路径、gptokeyb、SMPEG）。
5. 同步最终可用改动到 `sword3.sh` + docs + memory。

---

## 7. 文件清单（本次状态）

```
ports/sword3/
├── build_docker.sh          # aarch64 交叉编译（sword3 + libbionic_shim.so）
├── sword3.sh                # 启动脚本（前台/单实例/中文 locale/SUMMERTIME_CURSOR=0）
├── README.md                # 端口说明
├── docs/
│   ├── HANDOFF.md           # 技术交接（符号核对/编译修复/运行期架构）
│   └── PROGRESS.md          # 本文件（进展总结）
├── src/                     # loader 源码（summertimesaga 派生 + 资源路径 hook）
│   ├── main.c               # 8-hook 资源重定向 + SECONDARY_SOS 改用系统 libSDL2_image
│   ├── libbionic_shim.c     # 资源根 hook / fopen·open·fread 转发 / IMG_LoadPNG_RW 诊断
│   ├── libbionic_shim.vers  # 仅 __sF@LIBC 导出
│   ├── android_shim.c egl_shim.c imports.c jni_shim.c opensles_shim.c
│   ├── etc1.c so_util.c util.c error.c + *.h
│   └── _unused_katanazero/  # 误用脚手架（git-ignored，反例）
├── stubs/liblog_stub.c      # liblog 桩
├── build/                   # *.o（git-ignored）
├── sword3                   # loader 二进制 181584B（git-ignored）
└── libbionic_shim.so        # 18568B（git-ignored）

test/sword3/                 # 实机部署区（git-ignored）
├── sword3.sh                # 设备端 launcher（建 /tmp/sword3libs 符号链接、设 LD_LIBRARY_PATH）
├── sword3/                  # 上传的 loader + shim + 游戏 .so + assets（git-ignored）
└── sword3.zip               # 游戏数据整包（git-ignored，BYO-data）
```

> `.so` / 无扩展名 loader 二进制 / `assets/` / `build/` / `test/` 均受根 `.gitignore` 忽略，**绝不入库**（BYO-data + 编译产物）。仅提交源码与文档。
