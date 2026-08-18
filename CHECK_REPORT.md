# 仙剑奇侠传三 (Sword3) ARM64 so-loader — 项目检查报告

> 检查范围：静态代码 / 文档 / 构建与部署脚本（沙箱无 docker、无掌机、无游戏 .so，未做实机构建）
> 检查时间：2026-08-18 17:44 (GMT+8)
> 基线代码：`main.c` 已含 #1 编码检测 / #2 鼠标模拟 / #3 退出热键下沉 + 修改器/菜单/署名/canary-patch 等

---

## 0. 总体结论

- **代码层健康**：`main.c` 引用的 13 个 hook 目标（`fileIO_GetResourcePath`、`GetAPKXPath`、`GetSDCardPath`、`GetInterPath`、`GetGAMESAVEPath`、`APKXAndroid_JNI_FileOpen`、`fileIO_GetVideoPath`、`fileIO_GetACTPath`、`GetAndroidFileIsExists`、`Android_APKX_SetFile`、`GetAPKXFileLenv`、`GetAPKXFileOffsetv` 等）在 `src/libbionic_shim.c` 中**均已有对应实现**，运行期 hook 不会因符号缺失而失败。
- **构建链路闭环完整**：`build_docker.sh` → 产出 `sword3`/`libbionic_shim.so`/`liblog.so` → `deploy.sh` → `tools/patch_libs.sh`（LIBC→WEAK）→ 推送；链接命令冻结、patch 幂等、白名单防误改设备库。架构设计到位。
- **主要问题是"脚本/文档与代码意图不一致"以及"诊断遗留产物"**，非编译错误。其中 1 个为确定性笔误（BUG-1），1 个为需你拍板的架构冲突（BUG-2），其余为文档滞后与遗留清理。

---

## 1. BUG-1（高 · 确定性笔误）`SUMMERTIME_CURSOR` 设置与文档意图相反

| 位置 | 内容 | 含义 |
|------|------|------|
| `sword3.sh:151` | `export SUMMERTIME_CURSOR=1` | **会画调试光标角标** |
| `src/egl_shim.c:671-673` `draw_port_cursor()` | `if (cursor && strcmp(cursor,"0")==0) return;` | 仅当 `=0` 才关闭光标绘制 |
| `README.md:83` / `docs/PROGRESS.md:40` / `docs/HANDOFF.md:108` | "关调试光标（`SUMMERTIME_CURSOR=0`）" | 文档统一要求关闭 |
| `src/egl_shim.c:122-124` 注释 | "启动脚本设 `SUMMERTIME_CURSOR=0` 关闭它" | 设计意图为 0 |

**结论**：`=1` 与所有文档及 `egl_shim` 注释冲突，实机会每帧多画一个光标十字角标。应改为 `export SUMMERTIME_CURSOR=0`。属无争议笔误。

---

## 2. BUG-2（高 · 需你拍板）STAGING 内的 `libSDL2_image.so` 设备版软链与"严禁设备版"铁律冲突

**现象**：
- `sword3.sh:118` `ln -sf /usr/lib/libSDL2_image-2.0.so.0 "$STAGING/libSDL2_image.so"`
- `sword3.sh:137` `LD_LIBRARY_PATH="$STAGING:$GAMEDIR:..."` —— STAGING 在最前。
- `src/main.c:1741-1742` 在基于 `basedir` 的随包版 `dlopen` 失败时，有 `alt="/usr/lib/libSDL2_image-2.0.so.0"` 兜底（**容忍设备版**）。

**文档铁律（冲突方）**：
- `README.md:45-49`、HANDOFF §3、注释 `main.c:21-24` 均强制：**必须用随包 Android 版 `libSDL2_image.so`，设备版 `IMG_Load` 解码失败 → 黑屏（Bug A）**。

**风险链路**：若部署期 `patch_libs.sh` 漏跑或随包版 LIBC 未弱化 → 基于 `basedir` 的随包版 `dlopen` 失败 → 全局域落入设备版 → 游戏内部经裸名 `libSDL2_image.so` 解析命中 STAGING 设备版软链 → 黑屏（即 Bug A 复发）。

**需你决策**：当前固件下设备版 `/usr/lib/libSDL2_image-2.0.so.0` 的 `IMG_Load` 是否真的会黑屏？
- 若仍黑屏（维持铁律）：应删除 `sword3.sh:118` 的 STAGING 软链（让裸名回退到基于 `basedir` 的随包版），并移去 `main.c:1741-1742` 的设备版 fallback。
- 若当前固件已可用（铁律过时）：则 `main.c` 的 fallback 与脚本软链合理，但须同步修订 README/HANDOFF 的铁律表述，避免误导。

---

## 3. 矛盾-3（中）启动器启动方式文档不清

- `README.md:78` 写 `bash /storage/roms/ports/sword3/sword3.sh`，该路径在 ROCKNIX 下
  （`$directory=/storage/roms`、`GAMEDIR=$directory/ports/sword3`）是**正确**的绝对路径，
  脚本确实位于 `ports/sword3/sword3.sh`，并非多了一层。
- 真正问题在于 README **未说明必须经 PortMaster 启动**：脚本的 `$directory` 由 PortMaster
  `control.txt` 注入，脱离 PortMaster 手动 `bash` 会因 `$directory` 未定义得到错误 `GAMEDIR`。
- `FIX_REPORT.md:4` 称启动器是 `/storage/roms/ports/sword3.sh`（少一层 `sword3/`），与脚本实际
  部署位置（`ports/sword3/sword3.sh`）不符，属该报告笔误。

建议：README 保留 ROCKNIX 绝对路径作示例，但补注"须经 PortMaster 启动、`$directory` 由
`control.txt` 提供、路径随固件而变（`$directory/ports/sword3/sword3.sh`）"。

---

## 4. 矛盾-4（中）部署脚本设备 IP 不一致

- `deploy_to_device.sh:9` `IP="192.168.31.16"`
- `FIX_REPORT.md:3` 记录的设备 `192.168.137.16`

两者网段不同（`31` vs `137`），必有一方过时（你记忆亦提"测试设备 IP 不固定"）。建议以当前掌机实际 IP 为准统一；或参照 Summer Time Saga 做法，改脚本支持命令行传参（`$1` 覆盖 IP）。

---

## 5. 滞后-5（中）文档严重落后于代码

`docs/PROGRESS.md`（末次 2026-07-28）/ `docs/HANDOFF.md`（2026-07-31）**未记录**：
- #1 编码检测（BIG5/GBK 按字体名识别）、#2 手柄→鼠标模拟、#3 SELECT+START 退出下沉（FIX_REPORT 已声明验证）；
- 后续大量新功能：内置修改器（金钱/满血/不遇敌/一击必杀/抓怪/炼妖/符鬼）、菜单换人/物品栏、标题署名（ported by windstarry）、`SDL_SS2D::Init` canary patch、`GetVideoPath`/`GetACTPath` hook、`commButton` 选框等（均已在 `main.c` 实现）。

HANDOFF §4 实机测试清单仍有多个 `[ ]`，部分已被 FIX_REPORT 覆盖。建议把 FIX_REPORT 内容 + 新功能 + 当前已知问题并入 PROGRESS/HANDOFF，或新写 `CHANGELOG.md`。

---

## 6. 遗留-6（低）诊断产物与反例目录

- `libs/libSWD3E.so.bak-2021`：2021 旧游戏 `.so` 备份，属 BYO-data 但误留在 `libs/`。会被 `.gitignore` 的 `*.so` 忽略不入库，但可能干扰目录整洁，建议移出或确认非"当前"版本。
- `nm_apkx.txt` / `file_check.txt` / `check_refs.py`：早期 ELF 符号侦察产物；配对的 `verify_apkx.py` 已删除，三者失配。建议归档到 `docs/`（历史依据）或清理。
- `_unused_katanazero/`（5 文件 `.c/.h`）：PROGRESS/HANDOFF 称其 git-ignored 反例，但 `.gitignore` 仅有 `*.so`，**不含该目录** → 这些反例 `.c/.h` **会被 git 跟踪**。建议 `.gitignore` 补 `src/_unused_katanazero/`。

---

## 7. 无法验证项（环境限制）

- 构建需 `docker` + aarch64 交叉镜像 + 游戏 `.so`（BYO-data，不在仓库）；本机沙箱无 docker、无掌机。
- 以上结论均为**静态代码与文档审查**，`main.c` 是否仍能通过 `build_docker.sh` 编译需在 docker 环境实跑确认（重点核验 `so_util.c`/`imports.c`/`util.c` 提供的 `so_load`/`so_resolve`/`hook_arm64`/`text_base` 等声明与 `main.c` 调用一致）。

---

## 8. 建议处置清单（待你授权）

| 项 | 动作 | 风险 |
|----|------|------|
| BUG-1 | `sword3.sh` `SUMMERTIME_CURSOR=1` → `0` | 低（确定笔误） |
| 矛盾-3 | 修正 README 启动命令 + 注明 PortMaster 依赖 | 低 |
| 矛盾-4 | 统一部署 IP 或改脚本支持 `$1` 传参 | 低 |
| 遗留-6 | `.gitignore` 加 `src/_unused_katanazero/`；归档诊断产物 | 低 |
| BUG-2 | 依你拍板移除/保留 STAGING 软链与 `main.c` fallback | 中（影响实机黑屏风险） |
| 滞后-5 | 文档同步（PROGRESS/HANDOFF 或 CHANGELOG） | 低（纯文档） |
