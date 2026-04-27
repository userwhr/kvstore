#include "config.h"
#include "kvstore.h"
#include "rdma_replication.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// TCP 连接相关头文件
#include <arpa/inet.h>
#include <sys/socket.h>

file_content_t *kvs_log = NULL;
file_content_t *Icr_log = NULL;

/*
 * Slave 启动函数
 * 1. 初始化数据结构
 * 2. 通过 TCP 连接 Master，发送 SYNC 命令
 * 3. 通过 RDMA 接收全量数据到内存缓冲区
 * 4. 直接构造 file_content_t，用 RDMA 数据加载到 KV 存储
 * 5. 启动只读 Reactor 服务
 */
static int start_slave(void) {
  int sockfd = -1;
  int ret = -1;
  char *rdma_data = NULL;
  size_t rdma_size = 0;

  printf("[Slave] Starting as slave, connecting to master %s:%d\n",
         g_config.slave_of_ip, g_config.slave_of_port);

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
  printf("[Slave] TCP connected to master %s:%d\n", g_config.slave_of_ip,
         g_config.slave_of_port);

  /* ========== 第2步：发送 RESP 格式的 SYNC 命令 ========== */
  const char *sync_cmd = "*1\r\n$4\r\nSYNC\r\n";
  int send_ret = send(sockfd, sync_cmd, strlen(sync_cmd), 0);
  if (send_ret <= 0) {
    perror("send SYNC failed");
    close(sockfd);
    return -1;
  }
  printf("[Slave] SYNC command sent\n");

  /* ========== 第3步：接收 Master 的响应 ========== */
  char response[1024] = {0};
  int recv_ret = recv(sockfd, response, sizeof(response) - 1, 0);
  if (recv_ret <= 0) {
    perror("recv SYNC response failed");
    close(sockfd);
    return -1;
  }
  response[recv_ret] = '\0';
  printf("[Slave] Master response: %s\n", response);

  if (strncmp(response, "+OK", 3) != 0) {
    fprintf(stderr, "[Slave] Master returned error: %s\n", response);
    close(sockfd);
    return -1;
  }

  close(sockfd);

  /* ========== 第4步：通过 RDMA 接收全量数据到内存缓冲区 ========== */
  printf("[Slave] Starting RDMA data receive from %s:%d\n",
         g_config.slave_of_ip, g_config.slave_rdma_port);

  ret = rdma_slave_recv_file(g_config.slave_of_ip, g_config.slave_rdma_port,
                             &rdma_data, &rdma_size);
  if (ret != 0) {
    fprintf(stderr, "[Slave] RDMA data receive failed\n");
    return -1;
  }
  printf("[Slave] RDMA data receive complete: %zu bytes\n", rdma_size);

  /* ========== 第5步：直接构造 file_content_t，加载数据到 KV 存储 ========== */
  /* 不写磁盘！直接用 RDMA 接收到的内存缓冲区 */
  file_content_t fc;
  memset(&fc, 0, sizeof(fc));
  fc.filename = "name";     // 不使用文件
  fc.file_fd = -1;          // 无效 fd
  fc.file_data = rdma_data; // RDMA 接收到的数据
  fc.file_size = rdma_size; // 数据大小

  /* snapshot_restore_from_mmap 只使用 file_data 和 file_size，不依赖
   * fd/filename */
  snapshot_restore_from_mmap(&fc);
  printf("[Slave] DB loaded from RDMA data (%zu bytes)\n", rdma_size);

  /* 注意：rdma_data 由 snapshot_restore_from_mmap 使用完毕后，
   * 在程序退出前不应释放，因为数据结构中可能还有指针引用这些数据。
   * 这里不 free(rdma_data)，让它在进程退出时由 OS 回收。 */

  /* ========== 第6步：启动只读 Reactor 服务 ========== */
  printf("[Slave] Starting read-only reactor on port %d\n", g_config.port);
  react_entrence(g_config.port, kvs_filter_protocol);

  return 0;
}

int main(int argc, char *argv[]) {
  if (load_config("./kvstore.conf") != 0) {
    exit(EXIT_FAILURE);
  }

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

    if (g_config.enable_full_persist == 1) {
      kvs_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
      fcInit(g_config.log_path, kvs_log);
      map_file_to_struct(kvs_log);
      int ret = snapshot_restore_from_mmap(kvs_log);
      printf("DB loaded from disk\n");
    }
    if (g_config.enable_incremental_persist == 1) {
      Icr_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
      fcInit(g_config.log_path_icr, Icr_log);
      map_file_to_struct(Icr_log);
      AOF_commands_from_mmap(Icr_log);
      printf("AOF load from disk\n");
    }

#define ENABLE_REATOR 1
#if ENABLE_REATOR
    react_entrence(g_config.port, kvs_filter_protocol);
#else
    nytco_start(port, kvs_protocol);
#endif
  }

  return 0;
}
