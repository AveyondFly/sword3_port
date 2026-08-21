# libs/ — 游戏自带 Android .so（入库）

本目录存放轩辕剑3天之痕游戏自带的 Android native 库，**由本地提供并随仓库提交（非 BYO-data）**。

## 必须放置的文件（9 个，需经 LIBC→WEAK patch）

- `libSWD3E.so`
- `libSDL2.so`
- `libSDL2_image.so`   （必须是随包 Android 版，**严禁设备版**，否则黑屏）
- `libSDL2_mixer.so`
- `libSDL2_ttf.so`
- `libsmpeg2.so`
- `libmpg123.so`
- `libhidapi.so`
- `libc++_shared.so`

## 说明

- 这些 `.so` 由 `tools/patch_libs.py` 在【打包期】自动 WEAK 化（把 `.gnu.version_r` 的
  `LIBC` verneed 标为 WEAK），**源码保持原始、不被改写**；打包脚本
  `tools/package_portmaster.sh` 会先把它们拷贝到端口根目录（与 `sword3`/loader 同目录），
  再对暂存副本打补丁。
- main.c 的 `load_secondary_libs()` 用 `dirname(argv[0])` 加载 secondary `.so`，
  故这些库在包内必须与 `sword3` **同目录**（端口根目录），不能放进 `libs/` 子目录。
- 不要在本目录放引擎 shim（`libbionic_shim.so` / `liblog.so`）——它们由 `build_docker.sh`
  编译生成、已被 `.gitignore` 忽略，不在本目录。
- 本地备份文件以 `*.bak-*` 命名，已被 `.gitignore` 忽略，不入库。

## patch 流程（幂等）

本地提供已解包的原始 Android `.so` 即可。CI / 打包脚本会调用：

    python3 tools/patch_libs.py dist/sword3

对暂存副本做 LIBC→WEAK（已 WEAK 的会跳过，可重复执行）。
若漏跑该 patch，glibc 的 `ld.so` 会报 `undefined symbol: free, version LIBC` 并拒绝
`dlopen`，导致启动即崩。
