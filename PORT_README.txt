轩辕剑3天之痕 (Sword3 / SWD3E) — PortMaster 端口
================================================

本 zip 为标准 PortMaster 包，解压后整体落到设备的 ports/ 下即可：
  ports/swd3de.sh       启动脚本（与端口目录同名，在 ports/ 根）
  ports/swd3de/         端口目录（即 GAMEDIR）

包内含「引擎 + 游戏原生库」完整可运行部分；仅游戏资源 assets/ 需自备（BYO-data）。

包内文件：
  swd3de.sh        启动脚本（在 swd3de/ 之外，ports/ 根）
  swd3de/
    sword3             loader 二进制 (PIE)
    libbionic_shim.so  bionic -> glibc 兼容垫片（与 sword3 同目录，loader 按 dirname 加载）
    liblog.so          Android liblog 最小替身
    libSWD3E.so        游戏主模块（Android .so，已随包）
    libSDL2.so libSDL2_image.so libSDL2_mixer.so libSDL2_ttf.so
    libsmpeg2.so libmpg123.so libhidapi.so libc++_shared.so
                      （游戏自带 Android .so，已随包，并经 LIBC->WEAK patch）
    assets/            游戏资源目录（BYO-data，包内仅空占位，需自备填入）
    control.txt        PortMaster 元数据（portname=swd3de）
    swd3de.gptk        手柄映射（占位）
    readme.txt         本说明

部署步骤：
  1. 将本包解压，使 ports/swd3de.sh 与 ports/swd3de/ 就位。
  2. 拷贝游戏资源到 ports/swd3de/assets/（音乐/视频/贴图等大体量数据，不随包）。
  3. 游戏 .so 已随包并由打包流程自动 LIBC->WEAK 弱化，无需在设备端再打补丁
     （部署脚本 tools/patch_libs.sh 仍可 --verify 复核，幂等）。
  4. 经 PortMaster 启动（调用 swd3de.sh）即可。

注意：
  - 严禁用设备版 libSDL2_image（会黑屏）；包内 libSDL2_image.so 为随包 Android 版。
  - 设备需已生成 zh_CN.UTF-8（或 zh_TW.UTF-8）locale，否则中文走 C.UTF-8 兜底。
  - 游戏 .so 源文件位于仓库 libs/，由本地提供并入库；patch 在打包期对暂存副本执行。
  - 完整说明见上游仓库 README.md。
