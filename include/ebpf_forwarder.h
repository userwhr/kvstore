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
 * @brief 进入 eBPF 转发主循环（一对多模式）
 *
 * 此函数会：
 *   1. 进入主循环：轮询 perf buffer，将写命令转发给所有已注册的 Slave
 *   2. 同时监听 UDS，接收主进程传递的 slave fd
 *   3. 支持动态添加/移除 Slave fd
 *   4. 在收到 SIGINT/SIGTERM 时退出
 *
 * @param uds_fd Unix Domain Socket fd（与主进程通信），<0 则不监听
 * @return 0 正常退出, -1 错误退出
 */
int ebpf_forwarder_run(int uds_fd);

/**
 * @brief 添加 Slave fd 到 forwarder 的转发列表
 *
 * @param slave_fd 已连接到 Slave 的 TCP socket fd
 * @return 0 成功, -1 失败
 */
int ebpf_forwarder_add_slave(int slave_fd);

/**
 * @brief 从 forwarder 的转发列表移除 Slave fd
 *
 * @param slave_fd 要移除的 Slave fd
 */
void ebpf_forwarder_remove_slave(int slave_fd);

#endif // EBPF_FORWARDER_H
