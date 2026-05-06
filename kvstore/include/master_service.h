#ifndef MASTER_SERVICE_H
#define MASTER_SERVICE_H

/**
 * @brief Master 端启动入口
 *
 * 完整流程：
 *   1. 从磁盘加载数据（全量持久化 + 增量日志）
 *   2. 等待 Slave 通过 TCP 连接并发送 SYNC 命令
 *   3. 通过 RDMA 传输全量数据给 Slave
 *   4. 加载 XDP 程序到网卡
 *   5. 在独立线程中启动 eBPF forwarder（连接到 Slave 转发写命令）
 *   6. 启动 Reactor 服务
 *
 * @return 0 成功, -1 失败
 */
int start_master(void);

#endif // MASTER_SERVICE_H
