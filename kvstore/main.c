#include "config.h"
#include "ebpf_forwarder.h"
#include "kvs_persist.h"
#include "kvstore.h"
#include "master_service.h"
#include "slave_service.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

file_content_t *kvs_log = NULL;
file_content_t *Icr_log = NULL;

/* 全局退出标志，用于通知 sync_thread 退出 */
volatile int g_shutdown_requested = 0;

/*
 * 信号处理函数
 * 捕获 SIGINT/SIGTERM，设置退出标志，清理 eBPF 资源后退出
 */
static void signal_handler(int sig) {
  printf("\n[Main] Caught signal %d, cleaning up...\n", sig);

  /* 设置退出标志，通知 sync_thread 退出 */
  g_shutdown_requested = 1;

  /* 等待 sync_thread 退出（给它一点时间） */
  usleep(500000); /* 500ms */

  /* 卸载 XDP 程序 */
  ebpf_unload_xdp();

  printf("[Main] Cleanup complete, exiting.\n");
  _exit(0);
}

int main(int argc, char *argv[]) {
  if (load_config("./kvstore.conf") != 0) {
    exit(EXIT_FAILURE);
  }

  /* 注册信号处理函数 */
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // 初始化数据结构
  kvs_init();
  uringInit();

  if (g_config.enable_slave) {
    // ====================== Slave 模式 ======================
    printf("========================================\n");
    printf("  KVStore Slave Mode\n");
    printf("  Master: %s:%d\n", g_config.slave_of_ip, g_config.slave_of_port);
    printf("  RDMA Port: %d\n", g_config.slave_rdma_port);
    printf("  Service Port: %d\n", g_config.port);
    printf("========================================\n");

    start_slave();
  } else {
    // ====================== Master 模式 ======================
    printf("========================================\n");
    printf("  KVStore Master Mode\n");
    printf("  Port: %d\n", g_config.port);
    printf("  RDMA Sync Port: %d\n", g_config.slave_rdma_port);
    printf("========================================\n");

    start_master();
  }

  return 0;
}
