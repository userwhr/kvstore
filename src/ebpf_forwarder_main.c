/*
 * ============================================================================
 * eBPF 增量同步转发程序（独立可执行文件）
 *
 * 用法：ebpf_forwarder_main <uds_fd> [interface_name]
 *
 * 功能：
 *   1. 加载 ebpf_inc_sync.o 到指定网卡
 *   2. 创建 perf buffer，接收 eBPF 上报的写命令
 *   3. 通过 UDS 接收 slave fd，将写命令扇出转发给所有 Slave
 *
 * 此文件编译为独立可执行文件，由主程序通过 fork+exec 启动
 * ============================================================================
 */
#include "config.h"
#include "ebpf_forwarder.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* 全局变量定义（独立进程需要自己的副本） */
int g_slave_fd = -1;  /* 保留，但不再使用 */
volatile int g_shutdown_requested = 0;

/*
 * 信号处理函数
 */
static void signal_handler(int sig) {
  g_shutdown_requested = 1;
}

/*
 * 打印用法
 */
static void print_usage(const char *prog_name) {
  fprintf(stderr, "Usage: %s <uds_fd> [interface_name]\n", prog_name);
  fprintf(stderr, "  uds_fd:         Unix Domain Socket fd for communication with master\n");
  fprintf(stderr, "  interface_name: Network interface to attach XDP (default: ens33)\n");
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* 解析参数 */
  int uds_fd = atoi(argv[1]);
  if (uds_fd < 0) {
    fprintf(stderr, "[Forwarder-Main] Invalid uds_fd: %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  const char *ifname = "ens33"; /* 默认网卡 */
  if (argc >= 3) {
    ifname = argv[2];
  }

  /* forwarder 启动 */

  /* 加载配置文件（ebpf_forwarder.c 需要 g_config.port） */
  if (load_config("./kvstore.conf") != 0) {
    fprintf(stderr, "[Forwarder-Main] Failed to load config file\n");
    return EXIT_FAILURE;
  }

  /* 注册信号处理 */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  /* 初始化 eBPF 环境 */
  ebpf_init_rlimit();

  /* 加载 XDP 程序 */
  if (!ebpf_is_xdp_loaded()) {
    int xdp_ret = ebpf_load_xdp(ifname);
    if (xdp_ret != 0) {
      fprintf(stderr, "[Forwarder-Main] Failed to load XDP program on %s\n", ifname);
      return EXIT_FAILURE;
    }
    /* XDP 加载成功 */
  }

  /* 运行 forwarder 主循环（内部处理 UDS + perf buffer） */
  ebpf_forwarder_run(uds_fd);

  /* 清理 */
  ebpf_unload_xdp();
  if (uds_fd >= 0)
    close(uds_fd);

  return EXIT_SUCCESS;
}
