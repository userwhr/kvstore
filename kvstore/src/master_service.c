/*
 * ============================================================================
 * Master 端启动服务
 *
 * 完整流程：
 *   1. 从磁盘加载数据（全量持久化 + 增量日志）
 *   2. 启动 Reactor（监听端口，处理客户端请求）
 *      - 收到 SYNC 命令时，在 kvs_filter_protocol 的 case SYNC 中：
 *        a. 回复 +OK
 *        b. 通过 RDMA 传输全量数据
 *        c. 加载 XDP 程序
 *        d. fork eBPF forwarder 子进程
 *      - reactor.c 的 recv_cb 检测到 SLAVE 节点时，保存 g_slave_fd
 *      - eBPF forwarder 子进程使用 g_slave_fd 转发写命令
 * ============================================================================
 */
#include "master_service.h"
#include "config.h"
#include "kvs_persist.h"
#include "kvstore.h"
#include <stdio.h>
#include <stdlib.h>

/* 全局变量声明（在 main.c 中定义） */
extern file_content_t *kvs_log;
extern file_content_t *Icr_log;

int start_master(void) {
  printf("[Master] Starting as master\n");

  /* ========== 第1步：从磁盘加载数据 ========== */
  if (g_config.enable_full_persist == 1) {
    kvs_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
    fcInit(g_config.log_path, kvs_log);
    map_file_to_struct(kvs_log);
    int ret = snapshot_restore_from_mmap(kvs_log);
    printf("[Master] DB loaded from disk (full persist)\n");
  }
  if (g_config.enable_incremental_persist == 1) {
    Icr_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
    fcInit(g_config.log_path_icr, Icr_log);
    map_file_to_struct(Icr_log);
    AOF_commands_from_mmap(Icr_log);
    printf("[Master] AOF loaded from disk\n");
  }

  /* ========== 第2步：启动 Reactor ========== */
  /*
   * Reactor 会创建 listen fd 并进入 event loop。
   * 当 Slave 连接并发送 SYNC 命令时：
   *   1. accept_cb 接受连接
   *   2. recv_cb 解析 RESP，调用 kvs_filter_protocol
   *   3. case SYNC 中执行 RDMA 传输 + XDP 加载 + fork forwarder
   *   4. recv_cb 检测到 CLIENT_TYPE_SLAVE，保存 g_slave_fd
   */
  printf("[Master] Starting reactor on port %d\n", g_config.port);
  react_entrence(g_config.port, kvs_filter_protocol);

  return 0;
}
