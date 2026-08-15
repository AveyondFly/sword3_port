#!/usr/bin/env bash
# sword3 修复产物部署到 Rocknix 掌机 (默认 192.168.31.16)
# 用途: 把已验证的 3 个修复产物覆盖到设备 /storage/roms/ports/sword3/ 并复测。
# 运行环境: 在你【能连到掌机的本机】终端跑（不是 WorkBuddy 沙箱，它路由不到 192.168.31.x）。
#   - 推荐: 装了 sshpass 的 Linux/Mac/WSL/Git-Bash(choco install sshpass)
#   - 或: 纯手动 scp（脚本会在缺 sshpass 时打印命令）
set -u

IP="192.168.31.16"
USER="root"
PASS="rocknix"
REMOTE_DIR="/storage/roms/ports/sword3"
LOCAL_DIR="$(cd "$(dirname "$0")" && pwd)"
FILES=("sword3" "libbionic_shim.so" "liblog.so")

echo "本地产物目录: $LOCAL_DIR"
echo "目标: ${USER}@${IP}:${REMOTE_DIR}"
echo

# 自动路径: sshpass
if command -v sshpass >/dev/null 2>&1; then
  SCP="sshpass -p \"$PASS\" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
  SSH="sshpass -p \"$PASS\" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
  echo ">>> 检测到 sshpass，开始自动部署 ..."
  for f in "${FILES[@]}"; do
    if [ ! -f "$LOCAL_DIR/$f" ]; then echo "  ✗ 缺少本地文件 $f，跳过"; continue; fi
    echo "  scp $f -> 设备"
    eval "$SCP \"$LOCAL_DIR/$f\" \"${USER}@${IP}:${REMOTE_DIR}/$f\"" || { echo "  ✗ 拷贝 $f 失败"; exit 1; }
  done
  echo ">>> 设备上置可执行 + 抓一次运行日志(8s)"
  eval "$SSH ${USER}@${IP} \"chmod +x ${REMOTE_DIR}/sword3; cd ${REMOTE_DIR} && (./sword3 >/tmp/sword3_run.log 2>&1 &) ; sleep 8; echo '--- tail run log ---'; tail -40 /tmp/sword3_run.log\""
  echo ">>> 完成。请经 PortMaster 正常启动 sword3 复测 LIBC dlopen 与 PNG double-free。"
  exit 0
fi

# 手动路径: 打印 scp 命令
echo "⚠️ 未检测到 sshpass。请在本机执行以下命令手动部署（密码: $PASS）:"
for f in "${FILES[@]}"; do
  echo "  scp -o StrictHostKeyChecking=no \"$LOCAL_DIR/$f\" ${USER}@${IP}:${REMOTE_DIR}/$f"
done
echo
echo "部署后 SSH 登录设备并启动游戏，抓日志核验:"
echo "  ssh ${USER}@${IP}"
echo "  cd ${REMOTE_DIR} && ./sword3   # 或经 PortMaster 启动"
echo "复测要点:"
echo "  1) debug.log 中不应再出现 'undefined symbol: free, version LIBC' (libc++_shared/libsmpeg2/libhidapi dlopen 成功)"
echo "  2) 加载 BattleComm.png 等 PNG 不应再 'free(): double free' / SIGABRT"
echo "  3) 把新 debug.log 拷回本机交我们分析"
