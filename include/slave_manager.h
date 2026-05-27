#ifndef SLAVE_MANAGER_H
#define SLAVE_MANAGER_H

/*
 * ============================================================================
 * Slave 连接管理模块
 *
 * 管理多个 Slave 连接的 fd、同步状态
 * 支持一对多主从同步
 * ============================================================================
 */

#include <pthread.h>

/* UDS fd（在 slave_manager.c 中定义） */
extern int g_uds_fd;

/* Slave 管理 API */
int slave_register(int fd);       // 注册 Slave，返回 0 成功，-1 超过上限，-2 已存在
void slave_unregister(int fd);    // 移除 Slave
int is_slave_fd(int fd);          // 检查 fd 是否为 Slave
int slave_count(void);            // 当前 Slave 数量
int *slave_get_fds(int *count);   // 获取所有 Slave fd 数组（调用者 free）
int is_any_slave_syncing(void);   // 检查是否有 Slave 正在同步
int is_slave_syncing(int fd);     // 检查指定 Slave 是否正在同步
void slave_set_syncing(int fd, int val);  // 设置 per-slave 同步状态

/* UDS 通信 API */
void forwarder_set_uds_fd(int fd);         // 设置与 forwarder 的 UDS fd
int forwarder_send_add_slave(int fd);      // 通知 forwarder 添加 slave fd
void forwarder_send_remove_slave(int fd);  // 通知 forwarder 移除 slave fd

/* SYNC 同步 API */
void slave_start_sync(int slave_fd);  // 启动 Slave 同步（RDMA 传输）

/* 信号处理 */
void slave_manager_shutdown(void);  // 通知 forwarder 退出并等待

#endif // SLAVE_MANAGER_H
