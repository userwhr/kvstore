#!/bin/bash
# =============================================================================
# 绑定 ens33 网卡到 rxe0 RDMA 设备
# 前提：rdma_rxe 内核模块已加载
# =============================================================================

NET_IF="ens33"
RXE_NAME="rxe0"

if [ "$EUID" -ne 0 ]; then
    echo "需要 root 权限: sudo $0"
    exit 1
fi

# 如果已存在则跳过
if rdma link show "$RXE_NAME" &>/dev/null; then
    echo "[OK] $RXE_NAME 已绑定"
    rdma link show "$RXE_NAME"
    exit 0
fi

echo "绑定 $RXE_NAME -> $NET_IF ..."
rdma link add "$RXE_NAME" type rxe netdev "$NET_IF"
echo "[OK] 绑定完成"
rdma link show "$RXE_NAME"
