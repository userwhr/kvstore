#ifndef EBPF_FORWARDER_H
#define EBPF_FORWARDER_H

#include <stddef.h>

/**
 * @brief 初始化 eBPF 环境（解除 RLIMIT_MEMLOCK 限制）
 * 必须在加载 BPF 程序之前调用
 */
void ebpf_init_rlimit(void);

/**
 * @brief 加载 XDP 程序到指定网卡
 *
 * @param ifname 网卡名（如 "ens33"）
 * @return 0 成功, -1 失败
 */
int ebpf_load_xdp(const char *ifname);

/**
 * @brief 卸载 XDP 程序
 */
void ebpf_unload_xdp(void);

/**
 * @brief 检查 XDP 程序是否已加载
 *
 * @return 1 已加载, 0 未加载
 */
int ebpf_is_xdp_loaded(void);

/**
 * @brief 进入 eBPF 转发主循环
 *
 * 此函数会：
 *   1. 使用已有的 slave_fd（SYNC 阶段建立的 TCP 连接）
 *   2. 进入主循环：轮询 perf buffer，将写命令转发给 Slave
 *   3. 在收到 SIGINT/SIGTERM 时退出
 *
 * @param slave_fd 已连接到 Slave 的 TCP socket fd
 * @return 0 正常退出, -1 错误退出
 */
int ebpf_forwarder_run(int slave_fd);

#endif // EBPF_FORWARDER_H
