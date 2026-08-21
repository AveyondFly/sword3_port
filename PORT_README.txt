轩辕剑3天之痕 (Sword3 / SWD3E) — PortMaster 端口
================================================

本 zip 包含「引擎 + 游戏原生库」完整可运行部分；仅游戏资源 assets/ 需自备（BYO-data）。

包内文件：
  sword3             loader 二进制 (PIE)
  libbionic_shim.so  bionic -> glibc 兼容垫片（与 sword3 同目录，loader 按 dirname 加载）
  liblog.so          Android liblog 最小替身
  libSWD3E.so        游戏主模块（Android .so，已随包）
  libSDL2.so libSDL2_image.so libSDL2_mixer.so libSDL2_ttf.so
  libsmpeg2.so libmpg123.so libhidapi.so libc++_shared.so
                    （游戏自带 Android .so，已随包，并经 LIBC->WEAK patch）
  swd3de.sh          实际启动脚本
  sword3.sh          PortMaster 约定启动入口（exec swd3de.sh）
  control.txt        PortMaster 元数据
  swd3de.gptk        手柄映射（占位）

部署步骤：
  1. 将本包解压到设备的 ports/sword3/（即 GAMEDIR）。
  2. 拷贝游戏资源 assets/ 到同目录（音乐/视频/贴图等大体量数据，不随包）。
  3. 游戏 .so 已随包并由打包流程自动 LIBC->WEAK 弱化，无需在设备端再打补丁
     （部署脚本 tools/patch_libs.sh 仍可 --verify 复核，幂等）。
  4. 经 PortMaster 启动（调用 sword3.sh -> swd3de.sh）即可。

注意：
  - 严禁用设备版 libSDL2_image（会黑屏）；包内 libSDL2_image.so 为随包 Android 版。
  - 设备需已生成 zh_CN.UTF-8（或 zh_TW.UTF-8）locale，否则中文走 C.UTF-8 兜底。
  - 游戏 .so 源文件位于仓库 libs/，由本地提供并入库；patch 在打包期对暂存副本执行。
  - 完整说明见上游仓库 README.md。
