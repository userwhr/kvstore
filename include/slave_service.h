#ifndef SLAVE_SERVICE_H
#define SLAVE_SERVICE_H

/**
 * @brief Slave 端启动入口
 *
 * 完整流程：
 *   1. 通过 TCP 连接 Master，发送 SYNC 命令
 *   2. 通过 RDMA 接收全量数据到内存缓冲区
 *   3. 加载数据到 KV 存储
 *   4. 连接 Master 的 eBPF 转发端口，接收写命令
 *   5. 启动 Reactor 服务
 *
 * @return 0 成功, -1 失败
 */
int start_slave(void);

#endif // SLAVE_SERVICE_H
