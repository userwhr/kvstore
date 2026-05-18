/*
 * ============================================================================
 * Slave 端启动服务
 *
 * 完整流程：
 *   1. 通过 TCP 连接 Master，发送 SYNC 命令
 *   2. 通过 RDMA 接收全量数据到内存缓冲区
 *   3. 加载数据到 KV 存储
 *   4. 将 SYNC 阶段的 TCP 连接 fd 保存到全局 g_slave_fd
 *   5. 启动 Reactor（将 g_slave_fd 注册到 epoll，统一处理写命令和客户端请求）
 *
 * 关键设计：
 *   - SYNC 阶段的 TCP 连接保持不断，后续直接用于接收写命令
 *   - 写命令接收由 Reactor 的 epoll 统一管理，不需要独立线程
 * ============================================================================
 */
#include "slave_service.h"
#include "config.h"
#include "kvs_persist.h"
#include "kvstore.h"
#include "rdma_replication.h"
#include "server.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int start_slave(void) {
  int sockfd = -1;
  int ret = -1;
  char *rdma_data = NULL;
  size_t rdma_size = 0;

  // printf("[Slave] Starting as slave, connecting to master %s:%d\n",
  //        g_config.slave_of_ip, g_config.slave_of_port);

  /* ========== 第1步：通过 TCP 连接 Master ========== */
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket failed");
    return -1;
  }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(g_config.slave_of_port);
  if (inet_pton(AF_INET, g_config.slave_of_ip, &server_addr.sin_addr) <= 0) {
    perror("inet_pton failed");
    close(sockfd);
    return -1;
  }

  if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("connect to master failed");
    close(sockfd);
    return -1;
  }
  // printf("[Slave] TCP connected to master %s:%d\n", g_config.slave_of_ip,
  //        g_config.slave_of_port);

  /* ========== 第2步：发送 RESP 格式的 SYNC 命令 ========== */
  const char *sync_cmd = "*1\r\n$4\r\nSYNC\r\n";
  int send_ret = send(sockfd, sync_cmd, strlen(sync_cmd), 0);
  if (send_ret <= 0) {
    perror("send SYNC failed");
    close(sockfd);
    return -1;
  }
  // printf("[Slave] SYNC command sent\n");

  /* ========== 第3步：接收 Master 的响应 ========== */
  char response[1024] = {0};
  int recv_ret = recv(sockfd, response, sizeof(response) - 1, 0);
  if (recv_ret <= 0) {
    perror("recv SYNC response failed");
    close(sockfd);
    return -1;
  }
  response[recv_ret] = '\0';
  // printf("[Slave] Master response: %s\n", response);

  if (strncmp(response, "+OK", 3) != 0) {
    fprintf(stderr, "[Slave] Master returned error: %s\n", response);
    close(sockfd);
    return -1;
  }

  /* ========== 注意：不关闭 sockfd！后续用于接收写命令 ========== */
  // printf("[Slave] SYNC complete, keeping TCP connection alive for write forwarding\n");

  /* ========== 第4步：通过 RDMA 接收全量数据到内存缓冲区 ========== */
  // printf("[Slave] Starting RDMA data receive from %s:%d\n",
  //        g_config.slave_of_ip, g_config.slave_rdma_port);

  ret = rdma_slave_recv_file(g_config.slave_of_ip, g_config.slave_rdma_port,
                             &rdma_data, &rdma_size);
  if (ret != 0) {
    fprintf(stderr, "[Slave] RDMA data receive failed\n");
    close(sockfd);
    return -1;
  }
  // printf("[Slave] RDMA data receive complete: %zu bytes\n", rdma_size);

  /* ========== 第5步：加载数据到 KV 存储 ========== */
  file_content_t fc;
  memset(&fc, 0, sizeof(fc));
  fc.filename = "name";
  fc.file_fd = -1;
  fc.file_data = rdma_data;
  fc.file_size = rdma_size;

  snapshot_restore_from_mmap(&fc);
  // printf("[Slave] DB loaded from RDMA data (%zu bytes)\n", rdma_size);

  /* ========== 第6步：保存 slave fd 到全局变量 ========== */
  /*
   * 将 SYNC 阶段的 TCP 连接 fd 保存到全局 g_slave_fd。
   * Reactor 启动时会检查 g_config.enable_slave，如果为 true
   * 则将 g_slave_fd 注册到 epoll，用 recv_cb 统一处理写命令。
   */
  g_slave_fd = sockfd;
  // printf("[Slave] Slave fd=%d saved to g_slave_fd\n", sockfd);

  /* ========== 第7步：启动 Reactor 服务 ========== */
  /*
   * Reactor 的 react_entrence 会检查 g_config.enable_slave，
   * 如果为 true，将 g_slave_fd 注册到 epoll。
   * 之后 Master 发来的写命令会通过 recv_cb 处理。
   */
  // printf("[Slave] Starting reactor on port %d\n", g_config.port);
  react_entrence(g_config.port, kvs_filter_protocol);

  return 0;
}
