# 仙剑奇侠传三 (Sword3 / `com.softstar.G.swd3e`) — NextOS ARM64 so-loader

纯 SDL2 2D 游戏（大宇 Softstar）的 Android `.so` 在 NextOS Linux ARM64 上的 so-loader 移植。
基于 summertimesaga 的 so-loader 范式改写：加载 Android 版 `libSWD3E.so`，用编译进 loader 的
shim + 设备 Mali 驱动把 SDL2/EGL/GLES/OpenSLES/Android 调用桥接到原生 SDL2。

## 目标设备 / 后端

| 设备 | GPU | SDL2 后端（设备自选，绝不强制） |
|------|-----|--------------------------------|
| Mali-G31 | wayland | `wayland` |
| RK3562 / Mali-G52 | kmsdrm | `kmsdrm` |
| Mali-450 | fbdev | `fbdev` |

仅 arm64 设备。后端由设备侧 SDL2 按系统自动选择（项目铁律：**绝不指定 `SDL_VIDEODRIVER`**）。

## 构建

```bash
cd ports/sword3
bash build_docker.sh        # 在 aarch64 交叉镜像内编译，产出 ./sword3 (PIE) + libbionic_shim.so + liblog.so
```

- 镜像：`ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest`
- 链接（**loader 自身只链设备 SDL2/GLES 用于创窗，不链任何随包游戏库**）：
  `-lSDL2 -lGLESv2 -lGLESv1_CM -ldl -lm -lpthread -lstdc++ -lgcc_s -rdynamic -Wl,-rpath,$ORIGIN`
- 产物 `sword3` / `libbionic_shim.so` / `liblog.so` 为 git-ignored 二进制；仅提交源码（`src/` + `tools/` + 脚本）。

## 数据布局（BYO-data，绝不入库）

`$GAMEDIR`（`/storage/roms/ports/sword3`）下分三类：

| 类别 | 文件 | 来源 / 说明 |
|------|------|------------|
| 随包（**游戏自带，禁顶替**） | `libSWD3E.so` `libSDL2.so` `libSDL2_image.so` `libSDL2_mixer.so` `libSDL2_ttf.so` `libsmpeg2.so` `libmpg123.so` `libhidapi.so` `libc++_shared.so` `assets/` | 游戏自带 Android 资源；裸 soname，与设备带版本 soname 不冲突 |
| 随包（**构建产出**） | `sword3` `libbionic_shim.so` `liblog.so` | `build_docker.sh` 产出；glibc 编译，无 `LIBC` verneed |
| 设备提供（**不随包**） | `libGLESv2` `libGLESv1_CM` `libEGL`（Mali 驱动）、`libc`/`libm`/`libdl`（glibc，须 `LIBC→WEAK`）、`libz.so.1` | 设备系统原生库，运行期按裸 soname 预载 |

> `.so` 与 `assets/` 受根 `.gitignore` 的 `*.so` / `assets/` 规则忽略，不入库。
> 随包 Android `.so` 必须随包，因为它们是**裸 soname**（如 `libSDL2.so`），而设备/镜像是**带版本 soname**
> （`libSDL2-2.0.so.0`），两者不匹配，不能由设备版顶替。

## SDL2_image 特别说明（严禁设备版）

- 必须使用**随包** Android 版 `libSDL2_image.so`（裸 soname），由 `main.c` 的 `SECONDARY_SOS[4]`
  以 `RTLD_GLOBAL` 加载，供 `libSWD3E.so` 的 `IMG_*` 导入解析。
- **严禁**加载设备 `/usr/lib/libSDL2_image-2.0.so.0`：其 `IMG_Load` 在 glibc 上解码失败
  （"PNG not supported" / 返回 nil）→ **实机黑屏**。这是已定位并修复的 Bug A。
- 设备提供 `libGLESv2/v1_CM/EGL` 也不经 loader 链接，仅运行期预载到全局符号域。

## 部署 / patch 步骤（关键）

随包 Android `.so` 在 `.gnu.version_r` 把 `libc`/`libm` 标成 bionic 的 `LIBC` 版本节点，glibc 设备
没有 → `dlopen` 直接失败（`undefined symbol: free, version LIBC`，启动即崩）。**部署期**必须把它们
的 `LIBC` 标为 `WEAK`（零位移、可逆、最安全）：

```bash
# 方式 A（推荐）：本地暂存目录编排（含 patch + 推送指引）
bash deploy.sh <staging-dir>          # 默认 ./deploy；游戏 .so+assets 放入后再跑一次 patch
bash tools/patch_libs.sh <staging-dir>   # 显式对暂存目录 patch（幂等，可重复）

# 方式 B：直接在设备 $GAMEDIR 跑一次（幂等，FAT 可写文件，无 symlink 需求）
bash tools/patch_libs.sh "$GAMEDIR"

# 仅校验不写盘（返回 0=全部 WEAK / 非0=有未弱化）
bash tools/patch_libs.sh --verify "$GAMEDIR"
```

- `tools/patch_libs.sh` 默认只 patch **白名单**内 9 个随包 Android `.so`
  （`libSWD3E.so`/`libSDL2.so`/`libSDL2_image.so`/`libSDL2_mixer.so`/`libSDL2_ttf.so`/`libsmpeg2.so`/
  `libmpg123.so`/`libhidapi.so`/`libc++_shared.so`），避免误改设备库；`--all` 兜底任意含 `LIBC` 的 ELF。
- `libbionic_shim.so` / `liblog.so` 由我们在 glibc 下编译，无 `LIBC` verneed，**不**在 patch 之列。
- `main.c` 不执行 ELF 改写；遗漏 patch 时 `load_secondary_libs` 的 `dlopen` 会报 `version LIBC` 错误。

## 启动

> 本脚本由 **PortMaster** 经 `ports/` 调用，PortMaster 会注入 `$directory`（各固件不同：
> ROCKNIX 为 `/storage/roms`、其余多为 `/roms`），脚本据此得到
> `GAMEDIR="$directory/ports/sword3"`，因此**不要**脱离 PortMaster 手动 `bash` 本脚本
> （`$directory` 未定义会得到错误路径）。ROCKNIX 下经 PortMaster 启动即
> `/storage/roms/ports/sword3/sword3.sh`。

启动脚本负责：单实例清理、设 `LD_LIBRARY_PATH`（STAGING 软链设备 SDL2 全家桶置于最前、
`$GAMEDIR` 紧随其后，防 `ld.so` 误抓设备版 `libSDL2_image`）、设中文 locale
（默认 `zh_CN.UTF-8`，繁体改 `zh_TW.UTF-8`）、关调试光标（`SUMMERTIME_CURSOR=0`）、
performance governor。

资源路径前缀带 `assets/`：loader 把 `ANDROID_APP_PATH/ANDROID_ARGUMENT/ANDROID_PRIVATE`
设为 `$GAMEDIR/assets`，命中 `SDL_RWFromFile("Resource/...", ...)`。

## 已知事项 / 待实机验证

- [x] Bug A 根因定位：`SDL2_image` 误用设备版 → 黑屏；已改回随包 Android 版（待实机回归）
- [x] Bug B 根因定位：随包 Android `.so` 的 `LIBC` verneed 未弱化 → 启动即崩；部署期 `patch_libs.sh` 修复（待实机回归）
- [ ] Mali-G31 (wayland) 首帧渲染与输入
- [ ] RK3562/Mali-G52 (kmsdrm) 首帧渲染与输入
- [ ] 音频（OpenSLES→SDL2）与过场 SMPEG 视频
- [ ] 返回键 / 音量键（Java_* 回调丢失，必要时 gptokeyb 补）
- [ ] 中文显示：`LANG=zh_CN.UTF-8` 下繁简资源均可用；确认设备已生成对应 locale
- [ ] 确认 `libSWD3E.so` 内部是否硬编码资源路径（strings 未发现 `/data/`/`/sdcard/`）

详见 [`docs/HANDOFF.md`](docs/HANDOFF.md) 与架构清理方案 [`docs/REFACTOR_DESIGN.md`](docs/REFACTOR_DESIGN.md)。
