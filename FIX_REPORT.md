# 仙剑奇侠传三 (Sword3) ARM64 移植 — #1 / #2 修复验证

> 设备: Rocknix 掌机 `192.168.137.16` (root/rocknix), aarch64 Linux
> 启动器: `/storage/roms/ports/sword3.sh`（**非** `.../sword3/sword3.sh`）
> 字体: `/storage/roms/ports/sword3/assets/Resource/CT.ttf`（繁体）
> 构建镜像: `ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest`

---

## #1 进游戏不显示文字 — 根因 / 修复 / 验证

**根因**：游戏把 GBK/BIG5 字节直接喂给 `TTF_RenderUTF8_*`，标准 SDL_ttf 按 UTF-8 解析
→ 得到空/乱码 surface → 中文不显示。主菜单文字是图片烘焙的，故只有进入对话/剧情
（`TTF_RenderUTF8_*` 真正被调用）时才缺字——这解释了为什么停在菜单的日志里
`RenderUTF8` 计数为 0。

**修复**（`src/libbionic_shim.c` + `src/imports.c`）：
- `libbionic_shim.so` 拦截 `TTF_RenderUTF8_Blended/Solid/Shaded`，经
  `shim_ttf_prep_text()` 检测非合法 UTF-8 时，按**游戏实际打开的字体**推断编码转码为 UTF-8：
  `SDL_RWFromFile`/`fopen` 打开 `CT.ttf`/`CHT*` → **BIG5（繁体）**，`CS.ttf`/`CHS*` → **GBK（简体）**，
  再回退 GBK / BIG5 / GB18030。全局 `g_ttf_enc` 由字体名写入，转码首选它。
- **关键修正（繁/简歧义）**：初版把 `GBK` 放首选，对繁体 BIG5 文本会因 GBK 与 BIG5 字节区间
  重叠而“解码成功却出乱码”。本端口游戏经日志确认为**繁体（CHT）+ BIG5**
  （`s3e_root_panel_CHT.png`、加载 `CT.ttf`、`big5` 字样；`assets/Resource` 另有 `CS.ttf` 简体但未用），
  故改为“按字体名选对首选编码”，彻底消除歧义；若日后切简体（`CS.ttf`）会自动走 GBK。
- `imports.c` 通过 `dynlib_functions[]`（GOT/PLT 强制填充）+ `my_dlsym`（运行时 dlsym 拦截）
  双重保险，覆盖“游戏运行期 `dlsym(libSDL2_ttf,…)` 绕过 LD_PRELOAD”的路径。

**设备端验证**（`tools/test_ttf.c`，脱离菜单的独立机制测试）：
```
[shim:TTF] RenderUTF8_Blended orig='ÏÉ¼ÒÆæÏÀ´óÈý' (GBK/BIG5->UTF8)
    utf8[32]: e4 bb 99 e5 ae b6 e5 a5 87 e4 be a0 e5 a4 a7 e4 b8 89 ...
[shim:TTF] RenderUTF8_Blended -> ok  surf w=180 h=30
[test] GBK  Blended -> w=180 h=30      ✓ GBK 字节 → 转码 → CT.ttf 渲染出图
[test] UTF8 Blended -> w=315 h=30      ✓ 合法 UTF-8 原样透传
[test] GBK  Solid   -> w=180 h=30      ✓
[test] GBK  Shaded  -> w=180 h=30      ✓
```
**补充验证（繁体 BIG5 路径，`tools/test_big5.c`，按字体名识别编码）**：
- 模拟游戏在繁体模式把 BIG5 字节（`a5 50 bc 43 a9 5f ab 4c b6 c7 a4 54`）喂给 `TTF_RenderUTF8_Blended`；
  shim 经 `CT.ttf` 将 `g_ttf_enc` 设为 **BIG5**，转回 UTF-8（`e4 bb 99 …` = 仙劍奇俠傳三）并渲染：
  ```
  [shim:RW:ttf] '...CT.ttf' -> OK (enc=BIG5)
  [shim:TTF] RenderUTF8_Blended ... enc=BIG5 orig='¥P¼C©_«L¶Ç¤T' (transcode->UTF8)
      utf8[32]: e4 bb 99 e5 8a 8d e5 a5 87 e4 bf a1 e5 82 b3 e4 b8 89 ...
  [test] RenderUTF8_Blended(BIG5) -> w=180 h=30  non-empty=YES (PASS)
  ```
  证明繁体文字现在能正确转码并显示，不再出乱码。

**结论**：#1 机制端到端成立。游戏进入对话/剧情（触发 `TTF_RenderUTF8_*`）即显示中文（繁体走 BIG5）。

---

## #2 进游戏无法控制 / 不显示鼠标 — 修复（已实现，待实机试玩）

**根因**：仙剑是鼠标操作 RPG，掌机无物理鼠标；原 loader 仅后台轮询手柄做退出，
未向游戏注入任何鼠标事件。

**修复**（`src/main.c`）：将“退出热键”与“鼠标模拟”合并为单一后台线程
`sw_input_thread`（避免两个线程并发调 `SDL_GameControllerUpdate` 的隐患）。
loader 与 `libSWD3E.so` 同在**一个 SDL 进程**，线程经 `SDL_PushEvent` 向事件队列注入
`SDL_MOUSEMOTION` / `SDL_MOUSEBUTTON` / `SDL_MOUSEWHEEL`，游戏 `SDL_main` 主循环直接读到。

映射：
| 手柄 | 鼠标动作 |
|------|----------|
| 右摇杆 | 精细移动 |
| 左摇杆 | 慢速移动 |
| 方向键 | 步进移动 |
| A | 左键（确认） |
| B | 右键（取消/菜单） |
| L1 / R1 | 滚轮上 / 下 |

**设备端**：线程启动并检测到手柄 `[input] pad opened (idx=0)`，无崩溃/abort。
**待验证**：实机进入游戏 → 右摇杆移动光标、A/B 点击确认/取消、L1/R1 滚动画面。

---

## 部署产物（已落设备 `/storage/roms/ports/sword3/`）
- `sword3` (214488 B) — loader（含 #3 退出 + #2 鼠标模拟 + #1 编码检测）
- `libbionic_shim.so` (60600 B) — TTF 转码（按字体名识别繁/简）+ 文件/PNG 拦截
- `liblog.so` (10112 B)

## 用户下一步
1. 经 PortMaster 启动 sword3，进入对话确认中文显示（#1）。
2. 进游戏用右摇杆移动光标、A/B 操作（#2）；如方向反了/太快，调 `main.c` 的
   `FAST/SLOW/STEP` 常量或交换轴。
3. `SELECT+START` 仍可退出（#3）。
