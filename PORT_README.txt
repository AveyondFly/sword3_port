轩辕剑3天之痕 (Sword3 / SWD3E) — PortMaster 端口
================================================

本 zip 为「引擎部分」，不含游戏数据（BYO-data，不随包）：

  sword3             loader 二进制 (PIE)
  libbionic_shim.so  bionic -> glibc 兼容垫片（与 sword3 同目录，loader 按 dirname 加载）
  liblog.so          Android liblog 最小替身
  swd3de.sh          实际启动脚本
  sword3.sh          PortMaster 约定启动入口（exec swd3de.sh）
  control.txt        PortMaster 元数据
  swd3de.gptk        手柄映射（占位）

部署步骤：
  1. 将本包解压到设备的 ports/sword3/（即 GAMEDIR）。
  2. 拷贝游戏自带 Android .so 与 assets/ 到同目录：
       libSWD3E.so libSDL2.so libSDL2_image.so libSDL2_mixer.so libSDL2_ttf.so
       libsmpeg2.so libmpg123.so libhidapi.so libc++_shared.so  assets/
  3. 首次运行前，对随包 Android .so 执行 LIBC->WEAK 弱化（仓库 tools/patch_libs.sh），
     否则启动报 "undefined symbol: free, version LIBC"。
  4. 经 PortMaster 启动（会调用 sword3.sh -> swd3de.sh）即可。

注意：
  - 严禁用设备版 libSDL2_image（会黑屏）；必须用随包 Android 版 libSDL2_image.so。
  - 设备需已生成 zh_CN.UTF-8（或 zh_TW.UTF-8）locale，否则中文走 C.UTF-8 兜底。
  - 完整说明见上游仓库 README.md。
