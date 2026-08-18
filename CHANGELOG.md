# Changelog — 轩辕剑3天之痕 (Sword3) ARM64 so-loader

> 本文件汇总相对仓库基线（summertimesaga so-loader 范式）的修复与新功能。
> 详细的移植进度与架构见 `docs/PROGRESS.md` 与 `docs/HANDOFF.md`。

## 修复 (Fixes)

- **启动脚本调试光标笔误** (`sword3.sh`)：原 `SUMMERTIME_CURSOR=1` 会每帧绘制调试光标角标，
  与 README / `egl_shim` 注释"关闭调试光标"意图相反，改为 `SUMMERTIME_CURSOR=0`。
- **启动方式文档澄清** (`README.md`)：明确脚本须经 **PortMaster** 启动（由 `control.txt` 注入
  `$directory`），`GAMEDIR="$directory/ports/sword3"`；ROCKNIX 下即
  `/storage/roms/ports/sword3/sword3.sh`。脱离 PortMaster 手动 `bash` 会因 `$directory`
  未定义得到错误路径。
- **部署脚本 IP 可参数化** (`deploy_to_device.sh`)：`IP` 支持 `$1` 覆盖（默认 `192.168.31.16`），
  适配测试设备 IP 不固定的场景。

## 新功能 (Features，已在 `main.c` / `libbionic_shim.c` 实现)

- **#1 中文编码检测**：`libbionic_shim` 拦截 `TTF_RenderUTF8_*`，按游戏实际打开的字体名
  （`CT.ttf`→BIG5 繁体 / `CS.ttf`→GBK 简体）推断编码并转 UTF-8，修复对话/剧情缺字乱码。
- **#2 手柄→鼠标模拟**：`sw_input_thread` 将手柄映射为鼠标事件（右/左摇杆移动、方向键步进、
  A/B 左/右键、L1/R1 滚轮），轩辕剑3天之痕为鼠标 RPG，解决掌机无法操作。
- **#3 退出热键下沉**：SELECT+START 在 loader 内部每帧轮询物理手柄并 `_exit(0)`，不再依赖 gptokeyb。
- **内置修改器 (trainer)**：金钱 / 满血 / 不遇敌 / 一击必杀 / 抓怪 / 炼妖 / 符鬼 等开关。
- **菜单换人 / 物品栏**：解绑装备后 L1/R1 的残留绑定，避免 `DrawRoleIcon2` 崩溃。
- **标题署名**：标题画面加入 "ported by windstarry" 字样（避开大宇印章）。
- **`SDL_SS2D::Init` 栈 canary patch**：用 bind mount `/tmp/s3` 缩短绝对路径，规避
  `isAnySlotExist()` 定长栈缓冲溢出 abort（sig=6）。
- **`GetVideoPath` / `GetACTPath` hook**：扩展资源路径重定向，覆盖视频 / ACT 资源。

## 仓库清理 (Repo hygiene)

- `.gitignore` 补 `src/_unused_katanazero/`（误用脚手架反例，原称已忽略但实际被跟踪）与
  `libs/*.bak-*`（游戏 `.so` 备份，BYO-data 不入库）。
- 早期 ELF 符号侦察产物（`nm_apkx.txt` / `file_check.txt` / `check_refs.py`）归档至 `docs/diagnostics/`。
- `CHECK_REPORT.md`：本次静态审查报告（代码/文档/脚本一致性核查依据）。
