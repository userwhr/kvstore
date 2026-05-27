#include "kvstore.h"
#include "config.h"
#include "ebpf_forwarder.h"
#include "rbtree.h"
#include "rdma_replication.h"
#include "traversal.h"
#include <fcntl.h>
#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#else
#include <malloc.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

extern file_content_t *kvs_log; // 日志文件mmap以及信息存储的结构体
extern file_content_t *Icr_log;
extern char *command[];
extern int COMMAND_COUNT;

// 声明函数
int kvs_split_token(char *msg, char *tokens[]);
// 新增：按&&拆分多条指令
int kvs_split_multi_cmd(char *msg, char *cmds[]);

// 原有全局变量定义...
kvs_array_t *global_array = NULL;
kvs_rbtree_t *global_rbtree = NULL;
kvs_hash_t *global_hash = NULL;
kvs_skiplist_t *global_skiplist = NULL;

/*
 * SYNC 后处理线程：RDMA 传输 + XDP 加载 + fork forwarder
 * 在独立线程中执行，避免阻塞 Reactor event loop
 */
static void *sync_thread_func(void *arg) {
  // printf("[SYNC] Sync thread started\n");

  // 1. 重新打开文件读取最新内容，通过 RDMA 发送
  const void *sync_data = NULL;
  size_t sync_size = 0;

  FILE *fp = fopen(g_config.log_path, "rb");
  if (fp) {
    fseek(fp, 0, SEEK_END);
    sync_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sync_size > 0) {
      char *file_data = (char *)malloc(sync_size);
      if (file_data) {
        size_t read_ret = fread(file_data, 1, sync_size, fp);
        if (read_ret == sync_size) {
          sync_data = file_data;
        } else {
          free(file_data);
          sync_data = NULL;
          sync_size = 0;
        }
      }
    }
    fclose(fp);
  }

  if (sync_data && sync_size > 0) {
    // printf("[SYNC] Sending %zu bytes via RDMA on port %d...\n",
    //        sync_size, g_config.slave_rdma_port);

    int rdma_ret =
        rdma_master_send_mem(sync_data, sync_size, g_config.slave_rdma_port);
    free((void *)sync_data);
    if (rdma_ret != 0) {
      fprintf(stderr, "[SYNC] RDMA data transfer failed\n");
      g_syncing = 0;
      return NULL;
    }
    // printf("[SYNC] RDMA data transfer complete (%zu bytes)\n", sync_size);
  } else {
    fprintf(stderr, "[SYNC] No data to send via RDMA\n");
    g_syncing = 0;
    return NULL;
  }

  // 2. RDMA 传输完成，加载 XDP 程序到网卡
  // printf("[SYNC] RDMA complete, loading XDP program...\n");

  ebpf_init_rlimit();

  if (!ebpf_is_xdp_loaded()) {
    int xdp_ret = ebpf_load_xdp("ens33");
    if (xdp_ret != 0) {
      fprintf(stderr, "[SYNC] Failed to load XDP program\n");
      g_syncing = 0;
      return NULL;
    }
  }
  // printf("[SYNC] XDP loaded on ens33\n");

  /* 3. 等待 g_slave_fd 被设置（由 reactor 主进程设置） */
  // printf("[SYNC] Waiting for g_slave_fd...\n");
  int wait_count = 0;
  while (g_slave_fd < 0 && wait_count < 50) {
    usleep(100000); /* 100ms */
    wait_count++;
  }

  if (g_slave_fd < 0) {
    fprintf(stderr, "[SYNC] g_slave_fd not set, exiting\n");
    g_syncing = 0;
    return NULL;
  }

  // printf("[SYNC] g_slave_fd=%d, starting forwarder\n", g_slave_fd);

  /* 4. forwarder 启动前，先释放同步锁，恢复写操作 */
  g_syncing = 0;
  // printf("[SYNC] eBPF forwarder starting, write operations resumed\n");

  /* 5. 运行 eBPF forwarder（阻塞，直到收到 SIGINT/SIGTERM） */
  extern volatile int g_shutdown_requested;
  while (!g_shutdown_requested) {
    ebpf_forwarder_run(g_slave_fd);
    if (g_shutdown_requested) {
      break;
    }
    usleep(100000);
  }

  // printf("[SYNC] eBPF forwarder exited\n");

  // 同步完成，释放同步锁
  g_syncing = 0;
  // printf("[SYNC] Full sync complete, write operations resumed\n");

  return NULL;
}

// 主从同步标志：SYNC 进行中时，阻止写操作
extern volatile int g_syncing;

// 原有kvs_init函数...
void kvs_init(void) {
  if (g_config.enable_array) {
    global_array = (kvs_array_t *)malloc(sizeof(kvs_array_t));
    if (global_array == NULL) {
      perror("malloc global_array failed");
      exit(-1);
    }
    memset(global_array, 0, sizeof(kvs_array_t));
    array_creat(global_array);
  }
  if (g_config.enable_rbtree) {
    global_rbtree = (kvs_rbtree_t *)malloc(sizeof(kvs_rbtree_t));
    if (global_rbtree == NULL) {
      perror("malloc global_rbtree failed");
      exit(-1);
    }
    rbtree_create(global_rbtree);
  }
  if (g_config.enable_hash) {
    global_hash = (kvs_hash_t *)malloc(sizeof(kvs_hash_t));
    if (global_hash == NULL) {
      perror("malloc global_hash failed");
      exit(-1);
    }
    hash_creat(global_hash, DEFAULT_TABLE_SIZE);
  }
  if (g_config.enable_skiplist) {
    global_skiplist = (kvs_skiplist_t *)malloc(sizeof(kvs_skiplist_t));
    if (global_skiplist == NULL) {
      perror("malloc global_skiplist failed");
      exit(-1);
    }
    skiplist_creat(global_skiplist);
  }
}
// tokens中是不包括\r\n的
kvs_result_t kvs_filter_protocol(int fd, char *tokens[], char *msg, int count,
                                 char *response) {
  kvs_result_t result = {0, CLIENT_TYPE_NORMAL};
  int len_rsp = 0;
  response[0] = '\0';
  // printf("msg->%s", msg);
  // printf("tokens[0]=%s\n", tokens[0]);
  // printf("tokens[1]=%s\n", tokens[1]);
  // printf("tokens[2]=%s\n", tokens[2]);
  // printf("tokens[3]=%s\n", tokens[3]);
  int length = 0;

  // 参数校验
  if (tokens == NULL || response == NULL || count == 0 || msg == NULL) {
    length = sprintf(response, "-ERR invalid params\r\n");
    result.len = strlen(response);
    return result;
  }
  if (tokens[0] == NULL) {
    length = sprintf(response, "-ERR empty command\r\n");
    result.len = strlen(response);
    return result;
  }

  // 匹配命令
  int cmd = KVS_CMD_START;
  for (cmd = KVS_CMD_START; cmd < KVS_CMD_MAX; cmd++) {
    if (strcasecmp(tokens[0], command[cmd]) == 0) {
      break;
    }
  }

  // ====================== 主从同步锁检查 ======================
  // 如果 SYNC 正在进行中，阻止所有写操作
  if (g_syncing) {
    int is_write = 0;
    if ((cmd >= KVS_CMD_SET && cmd <= KVS_CMD_DEL) ||   // array: SET, DEL
        (cmd == KVS_CMD_MOD) ||                         // array: MOD
        (cmd >= KVS_CMD_RSET && cmd <= KVS_CMD_RDEL) || // rbtree: RSET, RDEL
        (cmd == KVS_CMD_RMOD) ||                        // rbtree: RMOD
        (cmd >= KVS_CMD_HSET && cmd <= KVS_CMD_HDEL) || // hash: HSET, HDEL
        (cmd == KVS_CMD_HMOD) ||                        // hash: HMOD
        (cmd >= KVS_CMD_SKSET &&
         cmd <= KVS_CMD_SKDEL) || // skiplist: SKSET, SKDEL
        (cmd == KVS_CMD_SKMOD) || // skiplist: SKMOD
        (cmd == SAVE) ||          // SAVE
        (cmd == FLUSHALL)) {      // FLUSHALL
      is_write = 1;
    }
    if (is_write) {
      length = sprintf(response, "-SYNC Syncing in progress\r\n");
      result.len = strlen(response);
      return result;
    }
  }
  // ==========================================================

  int ret = 0;
  // printf("cmd=%d", cmd);
  switch (cmd) {
  // 数组 SET（兼容 Redis：key 存在时覆盖）
  case KVS_CMD_SET:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = array_set(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-MSG ERR\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      // key 已存在，覆盖旧值
      int mod_ret = array_mod(global_array, tokens[1], tokens[2]);
      if (mod_ret == 0) {
        result.need_aof = 1;
        length = sprintf(response, "+OK\r\n");
      } else {
        length = sprintf(response, "-ERR overwrite failed\r\n");
      }
    }
    break;

  // 数组 GET
  case KVS_CMD_GET: {
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    char *result_array = array_get(global_array, tokens[1]);
    if (result_array == NULL) {
      length = sprintf(response, "-KEY NO EXIST\r\n");
    } else {
      int val_len = strlen(result_array);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_array);
    }
    break;
  }

  // 数组 DEL
  case KVS_CMD_DEL:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = array_del(global_array, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-Param error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret == 1) {
      length = sprintf(response, "-ERR error\r\n");
    }
    break;

  // 数组 MOD
  case KVS_CMD_MOD:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = array_mod(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-Param Error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-KEY NO EXIST\r\n");
    }
    break;

  // 数组 EXIST
  case KVS_CMD_EXIST:
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = array_exist(global_array, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+EXIST\r\n");
    } else {
      length = sprintf(response, "-KEY NO EXIST\r\n");
    }
    break;

  // 红黑树 RSET
  case KVS_CMD_RSET:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = rbtree_set(global_rbtree, tokens[1], tokens[2]);
    result.need_aof = 1;
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "+exist\r\n");
    }
    break;

  // 红黑树 RGET
  case KVS_CMD_RGET: {
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    char *result_2 = rbtree_get(global_rbtree, tokens[1]);
    if (result_2 == NULL) {
      length = sprintf(response, "-key no exist\r\n");
    } else {
      int val_len = strlen(result_2);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_2);
    }
    free(result_2);
    break;
  }

  // 红黑树 RDEL
  case KVS_CMD_RDEL:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = rbtree_del(global_rbtree, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 红黑树 RMOD
  case KVS_CMD_RMOD:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = rbtree_mod(global_rbtree, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 红黑树 REXIST
  case KVS_CMD_REXIST:
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = rbtree_exist(global_rbtree, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+exist\r\n");
    } else {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 哈希 HSET
  case KVS_CMD_HSET:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = hash_set(global_hash, tokens[1], tokens[2]);
    result.need_aof = 1;
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "+exist\r\n");
    }
    break;

  // 哈希 HGET
  case KVS_CMD_HGET: {
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    char *result_hash = hash_get(global_hash, tokens[1]);
    if (result_hash == NULL) {
      length = sprintf(response, "-ERR no exist\r\n");
    } else {
      int val_len = strlen(result_hash);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_hash);
    }
    break;
  }

  // 哈希 HDEL
  case KVS_CMD_HDEL:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = hash_del(global_hash, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 哈希 HMOD
  case KVS_CMD_HMOD:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = hash_mod(global_hash, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 哈希 HEXIST
  case KVS_CMD_HEXIST:
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = hash_exist(global_hash, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+exist\r\n");
    } else {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 跳表 SKSET
  case KVS_CMD_SKSET:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = skiplist_set(global_skiplist, tokens[1], tokens[2]);
    result.need_aof = 1;
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-ERR KEY EXIST\r\n");
    }
    break;

  // 跳表 SKGET
  case KVS_CMD_SKGET: {
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    char *result_sk = skiplist_get(global_skiplist, tokens[1]);
    if (result_sk == NULL) {
      length = sprintf(response, "-ERR no exist\r\n");
    } else {
      int val_len = strlen(result_sk);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_sk);
    }
    free(result_sk);
    break;
  }

  // 跳表 SKDEL
  case KVS_CMD_SKDEL:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = skiplist_del(global_skiplist, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 跳表 SKMOD
  case KVS_CMD_SKMOD:
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (g_config.enable_slave && fd != g_slave_fd) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = skiplist_mod(global_skiplist, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      result.need_aof = 1;
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 跳表 SKEXIST
  case KVS_CMD_SKEXIST:
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = skiplist_exist(global_skiplist, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+exist\r\n");
    } else {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // SAVE
  case SAVE: {
    if (count != 1) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    if (g_config.enable_full_persist == 1) {
      dump_all_to_disk();
      sprintf(response, "+OK\r\n");
    }
    break;
  }
  case COMMAND: {
    // 每个命令 6 个元素：name, arity, flags, first-key, last-key, step
    struct {
      const char *name;
      int arity;
      const char *flags;
      int first_key;
      int last_key;
      int step;
    } cmd_info[] = {
        {"set", 3, "write", 1, 1, 1},     {"get", 2, "readonly", 1, 1, 1},
        {"del", -2, "write", 1, -1, 1},   {"mod", 3, "write", 1, 1, 1},
        {"exist", 2, "readonly", 1, 1, 1}, {"rset", 3, "write", 1, 1, 1},
        {"rget", 2, "readonly", 1, 1, 1},  {"rdel", 2, "write", 1, 1, 1},
        {"rmod", 3, "write", 1, 1, 1},     {"rexist", 2, "readonly", 1, 1, 1},
        {"hset", 3, "write", 1, 1, 1},     {"hget", 2, "readonly", 1, 1, 1},
        {"hdel", 2, "write", 1, 1, 1},     {"hmod", 3, "write", 1, 1, 1},
        {"hexist", 2, "readonly", 1, 1, 1}, {"skset", 3, "write", 1, 1, 1},
        {"skget", 2, "readonly", 1, 1, 1},  {"skdel", 2, "write", 1, 1, 1},
        {"skmod", 3, "write", 1, 1, 1},    {"skexist", 2, "readonly", 1, 1, 1},
        {"save", 1, "write", 0, 0, 0},     {"command", 0, "readonly", 0, 0, 0},
        {"flushall", 1, "write", 0, 0, 0}, {"dbcount", 1, "readonly", 0, 0, 0},
        {"ping", -1, "readonly", 0, 0, 0}, {"config", -2, "readonly", 0, 0, 0}};
    int cmd_count = sizeof(cmd_info) / sizeof(cmd_info[0]);

    length = sprintf(response, "*%d\r\n", cmd_count);

    for (int i = 0; i < cmd_count; i++) {
      // 6 个元素：name, arity, flags, first-key, last-key, step
      length += sprintf(response + length, "*6\r\n");

      // name
      int name_len = strlen(cmd_info[i].name);
      length += sprintf(response + length, "$%d\r\n%s\r\n", name_len,
                        cmd_info[i].name);

      // arity
      length += sprintf(response + length, ":%d\r\n", cmd_info[i].arity);

      // flags (array of strings)
      int flag_len = strlen(cmd_info[i].flags);
      length += sprintf(response + length, "*1\r\n$%d\r\n%s\r\n", flag_len,
                        cmd_info[i].flags);

      // first-key
      length += sprintf(response + length, ":%d\r\n", cmd_info[i].first_key);

      // last-key
      length += sprintf(response + length, ":%d\r\n", cmd_info[i].last_key);

      // step
      length += sprintf(response + length, ":%d\r\n", cmd_info[i].step);
    }
    break;
  }
  case SYNC: {
    if (g_config.enable_slave && fd != g_slave_fd) {
      length = sprintf(response, "-ERR SYNC command is for master only\r\n");
      break;
    }

    /* 标记此连接为 Slave 节点 */
    result.client_type = CLIENT_TYPE_SLAVE;

    // 设置同步标志，阻止写操作
    g_syncing = 1;
    // printf("[SYNC] Starting full sync, write operations blocked\n");

    // 1. 执行 SAVE（将当前内存数据持久化到磁盘）
    if (g_config.enable_full_persist == 1) {
      dump_all_to_disk();
    }

    // 2. 回复 +OK（先返回，让 Reactor 发送响应给 Slave）
    length = sprintf(response, "+OK\r\n");

    // 3. 在独立线程中执行 RDMA 传输 + XDP 加载 + fork forwarder
    //    不能阻塞 Reactor event loop，否则 +OK 响应无法发送
    pthread_t sync_thread;
    pthread_create(&sync_thread, NULL, &sync_thread_func, NULL);
    pthread_detach(sync_thread);

    break;
  }
  case ECHO: {
    if (count < 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    int echo_len = strlen(tokens[1]);
    length = sprintf(response, "$%d\r\n%s\r\n", echo_len, tokens[1]);
    break;
  }
  case FLUSHALL: {
    if (count != 1) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    if (g_config.enable_array) {
      array_destory(global_array);
      array_creat(global_array);
    }
    if (g_config.enable_rbtree) {
      rbtree_destroy(global_rbtree);
      rbtree_create(global_rbtree);
    }
    if (g_config.enable_hash) {
      hash_destory(global_hash);
      hash_creat(global_hash, DEFAULT_TABLE_SIZE);
    }
    if (g_config.enable_skiplist) {
      skiplist_destory(global_skiplist);
      skiplist_creat(global_skiplist);
    }
    // 强制归还 free 的内存给 OS，降低 RSS
    #ifdef USE_JEMALLOC
    {
      unsigned narenas = 0;
      size_t len = sizeof(narenas);
      if (mallctl("arenas.narenas", &narenas, &len, NULL, 0) == 0) {
        for (unsigned i = 0; i < narenas; i++) {
          char cmd[64];
          snprintf(cmd, sizeof(cmd), "arena.%u.purge", i);
          mallctl(cmd, NULL, NULL, NULL, 0);
        }
      }
    }
    #else
    malloc_trim(0);
    #endif
    length = sprintf(response, "+OK\r\n");
    break;
  }
  case DBCOUNT: {
    if (count != 1) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    int array_cnt = global_array ? global_array->total : 0;
    int rbtree_cnt = global_rbtree ? global_rbtree->count : 0;
    int hash_cnt = global_hash ? global_hash->count : 0;
    int skiplist_cnt = global_skiplist ? global_skiplist->count : 0;
    int total = array_cnt + rbtree_cnt + hash_cnt + skiplist_cnt;
    char info[256];
    int info_len = snprintf(
        info, sizeof(info), "array:%d rbtree:%d hash:%d skiplist:%d total:%d",
        array_cnt, rbtree_cnt, hash_cnt, skiplist_cnt, total);
    length = sprintf(response, "$%d\r\n%s\r\n", info_len, info);
    break;
  }
  case PING: {
    length = sprintf(response, "+PONG\r\n");
    break;
  }
  case CONFIG_CMD: {
    // CONFIG GET <param> — 返回基本配置信息
    if (count >= 3 && strcasecmp(tokens[1], "GET") == 0) {
      const char *param = tokens[2];
      if (strcasecmp(param, "maxmemory") == 0) {
        length = sprintf(response, "*2\r\n$10\r\nmaxmemory\r\n$3\r\n0\r\n");
      } else if (strcasecmp(param, "save") == 0) {
        length = sprintf(response, "*2\r\n$4\r\nsave\r\n$0\r\n\r\n");
      } else if (strcasecmp(param, "appendonly") == 0) {
        length = sprintf(response, "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n");
      } else {
        length = sprintf(response, "*0\r\n");
      }
    } else {
      length = sprintf(response, "+OK\r\n");
    }
    break;
  }
  // 未知命令
  default:
    length = sprintf(response, "-ERR unknown command\r\n");
    break;
  }

  len_rsp = strlen(response);
  result.len = len_rsp;
  return result;
}