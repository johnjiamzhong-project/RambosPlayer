#!/usr/bin/env bash
# 将 ARM64 交叉编译产物部署到板卡（firefly@192.168.1.200）
# 用法：./deploy-arm64.sh [Debug|Release 提示信息可忽略，直接使用 build-arm64/RambosPlayer]

set -euo pipefail

BOARD_HOST="firefly@192.168.1.200"
BOARD_DIR="~/app/RambosPlayer"
LOCAL_BIN="build-arm64/RambosPlayer"

if [[ ! -f "$LOCAL_BIN" ]]; then
    echo "[ERROR] 未找到 $LOCAL_BIN，请先执行: cmake --build build-arm64" >&2
    exit 1
fi

echo "[deploy] 创建远程目录 ${BOARD_DIR} ..."
ssh "$BOARD_HOST" "mkdir -p ${BOARD_DIR}"

echo "[deploy] 拷贝可执行文件 ..."
scp "$LOCAL_BIN" "${BOARD_HOST}:${BOARD_DIR}/RambosPlayer"

echo "[deploy] 生成启动脚本 run.sh（设置 LD_LIBRARY_PATH 指向板卡 ffmpeg-rockchip）..."
ssh "$BOARD_HOST" "cat > ${BOARD_DIR}/run.sh" <<'EOF'
#!/usr/bin/env bash
# 编译时 RPATH 指向本机 sysroot 路径，板卡上需手动指定 ffmpeg-rockchip 库目录
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="/opt/ffmpeg-rockchip/lib:${LD_LIBRARY_PATH:-}"
exec "$DIR/RambosPlayer" "$@"
EOF

ssh "$BOARD_HOST" "chmod +x ${BOARD_DIR}/RambosPlayer ${BOARD_DIR}/run.sh"

echo "[deploy] 完成。板卡上运行："
echo "  ssh ${BOARD_HOST}"
echo "  ${BOARD_DIR}/run.sh"
