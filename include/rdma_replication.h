#ifndef RDMA_REPLICATION_H
#define RDMA_REPLICATION_H

#include <stddef.h>

// 全局同步标志（由 kvstore.c 定义，用于在 SYNC 期间阻止写操作）
extern volatile int g_syncing;

/**
 * @brief Master 端：启动 RDMA 服务，等待 Slave 连接并传输文件
 * @param file_path 要传输的文件路径
 * @param rdma_port RDMA 监听端口
 * @return 0 成功, -1 失败
 */
int rdma_master_send_file(const char *file_path, int rdma_port);

/**
 * @brief Master 端：通过 RDMA 直接发送内存缓冲区数据给 Slave
 *        适用于 kvs_log 已经通过 mmap 加载到内存的场景
 * @param data      内存数据指针
 * @param data_size 数据大小
 * @param rdma_port RDMA 监听端口
 * @return 0 成功, -1 失败
 */
int rdma_master_send_mem(const void *data, size_t data_size, int rdma_port);

/**
 * @brief Master 端 RDMA 传输线程入口（文件路径版本，封装了加锁/解锁逻辑）
 * @param arg 指向 {file_path, rdma_port} 结构体的指针
 * @return NULL
 */
void *rdma_master_send_file_thread(void *arg);

/**
 * @brief Master 端 RDMA 传输线程入口（内存数据版本，封装了加锁/解锁逻辑）
 * @param arg 指向 {data, data_size, rdma_port} 结构体的指针
 * @return NULL
 */
void *rdma_master_send_mem_thread(void *arg);

/**
 * @brief Slave 端：连接 Master RDMA 服务，接收文件数据到内存缓冲区
 *        接收到的数据格式与 full_log.txt 完全一致（快照格式）
 * @param server_ip Master IP
 * @param rdma_port Master RDMA 端口
 * @param out_buf   输出参数，指向接收到的数据缓冲区（由调用者负责 free）
 * @param out_size  输出参数，接收到的数据大小
 * @return 0 成功, -1 失败
 */
int rdma_slave_recv_file(const char *server_ip, int rdma_port, char **out_buf,
                         size_t *out_size);

/**
 * @brief Master 端：等待 Slave 通过 TCP 连接并发送 SYNC 命令，
 *        然后通过 RDMA 传输内存数据给 Slave
 *
 * 这是一个阻塞函数，集成了以下流程：
 *   1. 在 listen_fd 上 accept 等待 Slave 连接（如果 listen_fd < 0 则自己创建）
 *   2. 接收 Slave 的 SYNC 命令，回复 +OK
 *   3. 通过 RDMA 发送内存数据给 Slave
 *   4. 返回 Slave 的 TCP 连接 fd（用于后续 eBPF 增量同步）
 *
 * @param data      要发送的内存数据指针
 * @param data_size 数据大小
 * @param listen_fd 外部传入的 listen socket fd（如果 < 0 则内部创建）
 * @param rdma_port RDMA 传输端口
 * @param slave_fd  输出参数，Slave 的 TCP 连接 fd
 * @return 0 成功, -1 失败
 */
int rdma_master_wait_slave_and_sync(const void *data, size_t data_size,
                                    int listen_fd, int rdma_port, int *slave_fd);

#endif // RDMA_REPLICATION_H
