#include "kvstore.h"
#include "config.h"
#include "rbtree.h"
#include "rdma_replication.h"
#include "traversal.h"
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int APPEND_STR = 1;
extern file_content_t *kvs_log; // 日志文件mmap以及信息存储的结构体
extern file_content_t *Icr_log;
extern char *command[];
extern int COMMAND_COUNT;

// 声明函数
int kvs_split_token(char *msg, char *tokens[]);
int kvs_filter_protocol(char *tokens[], char *msg, int count, char *response);
// 新增：按&&拆分多条指令
int kvs_split_multi_cmd(char *msg, char *cmds[]);

// 原有全局变量定义...
kvs_array_t *global_array = NULL;
kvs_rbtree_t *global_rbtree = NULL;
kvs_hash_t *global_hash = NULL;
kvs_skiplist_t *global_skiplist = NULL;

// 主从同步标志：SYNC 进行中时，阻止写操作
extern volatile int g_syncing;

// 原有kvs_init函数...
void kvs_init(void) {
  if (g_config.enable_slab_pool) {
    kvs_init_slots();
  }
  if (g_config.enable_array) {
    global_array = (kvs_array_t *)malloc(sizeof(kvs_array_t));
    if (global_array == NULL) {
      perror("malloc global_array failed");
      exit(-1);
    }
    memset(global_array, 0, sizeof(kvs_array_t));
    kvs_array_creat(global_array);
  }
  if (g_config.enable_rbtree) {
    global_rbtree = (kvs_rbtree_t *)malloc(sizeof(kvs_rbtree_t));
    if (global_rbtree == NULL) {
      perror("malloc global_rbtree failed");
      exit(-1);
    }
    kvs_rbtree_create(global_rbtree);
  }
  if (g_config.enable_hash) {
    global_hash = (kvs_hash_t *)malloc(sizeof(kvs_hash_t));
    if (global_hash == NULL) {
      perror("malloc global_hash failed");
      exit(-1);
    }
    kvs_hash_creat(global_hash);
  }
  if (g_config.enable_skiplist) {
    global_skiplist = (kvs_skiplist_t *)malloc(sizeof(kvs_skiplist_t));
    if (global_skiplist == NULL) {
      perror("malloc global_skiplist failed");
      exit(-1);
    }
    kvs_skiplist_creat(global_skiplist);
  }
}
// tokens中是不包括\r\n的
int kvs_filter_protocol(char *tokens[], char *msg, int count, char *response) {
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
    return strlen(response);
  }
  if (tokens[0] == NULL) {
    length = sprintf(response, "-ERR empty command\r\n");
    return strlen(response);
  }

  // 匹配命令
  int cmd = KVS_CMD_START;
  for (cmd = KVS_CMD_START; cmd < KVS_CMD_MAX; cmd++) {
    if (strcmp(tokens[0], command[cmd]) == 0) {
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
        (cmd == SAVE)) {          // SAVE
      is_write = 1;
    }
    if (is_write) {
      length = sprintf(response, "-SYNC Syncing in progress\r\n");
      return strlen(response);
    }
  }
  // ==========================================================

  int ret = 0;
  char *async_buf;
  size_t msg_len;
  // printf("cmd=%d", cmd);
  switch (cmd) {
  // 数组 SET
  case KVS_CMD_SET:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_array_set(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-MSG ERR\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "+OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "-KEY EXIST\r\n");
    }
    break;

  // 数组 GET
  case KVS_CMD_GET: {
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    char *result_array = kvs_array_get(global_array, tokens[1]);
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
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_array_del(global_array, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-Param error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      } else if (ret == 1) {
        length = sprintf(response, "-ERR error\r\n");
      }
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 数组 MOD
  case KVS_CMD_MOD:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_array_mod(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-Param Error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
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
    ret = kvs_array_exist(global_array, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+EXIST\r\n");
    } else {
      length = sprintf(response, "-KEY NO EXIST\r\n");
    }
    break;

  // 红黑树 RSET
  case KVS_CMD_RSET:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_rbtree_set(global_rbtree, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }
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
    char *result_2 = kvs_rbtree_get(global_rbtree, tokens[1]);
    if (result_2 == NULL) {
      length = sprintf(response, "-key no exist\r\n");
    } else {
      int val_len = strlen(result_2);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_2);
    }
    kvs_free(result_2);
    break;
  }

  // 红黑树 RDEL
  case KVS_CMD_RDEL:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_rbtree_del(global_rbtree, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 红黑树 RMOD
  case KVS_CMD_RMOD:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_rbtree_mod(global_rbtree, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
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
    ret = kvs_rbtree_exist(global_rbtree, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+exist\r\n");
    } else {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 哈希 HSET
  case KVS_CMD_HSET:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_hash_set(global_hash, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }
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
    char *result_hash = kvs_hash_get(global_hash, tokens[1]);
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
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_hash_del(global_hash, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 哈希 HMOD
  case KVS_CMD_HMOD:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_hash_mod(global_hash, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
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
    ret = kvs_hash_exist(global_hash, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "+exist\r\n");
    } else {
      length = sprintf(response, "-ERR key not exist\r\n");
    }
    break;

  // 跳表 SKSET
  case KVS_CMD_SKSET:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_skiplist_set(global_skiplist, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }
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
    char *result_sk = kvs_skiplist_get(global_skiplist, tokens[1]);
    if (result_sk == NULL) {
      length = sprintf(response, "-ERR no exist\r\n");
    } else {
      int val_len = strlen(result_sk);
      length = sprintf(response, "$%d\r\n%s\r\n", val_len, result_sk);
    }
    kvs_free(result_sk);
    break;
  }

  // 跳表 SKDEL
  case KVS_CMD_SKDEL:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 2) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_skiplist_del(global_skiplist, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "+OK\r\n");
    }
    break;

  // 跳表 SKMOD
  case KVS_CMD_SKMOD:
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (g_config.enable_slave) {
      length =
          sprintf(response,
                  "-READONLY You can't write against a read only replica.\r\n");
      break;
    }
    if (count != 3) {
      length = sprintf(response, "-syntax error\r\n");
      break;
    }
    ret = kvs_skiplist_mod(global_skiplist, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "-ERR error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
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
    ret = kvs_skiplist_exist(global_skiplist, tokens[1]);
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
    struct {
      const char *name;
      int arity;         // 参数个数（含命令本身）
      const char *flags; // 命令属性
    } cmd_info[] = {{"set", 3, "write"},       {"get", 2, "readonly"},
                    {"del", 2, "write"},       {"mod", 3, "write"},
                    {"exist", 2, "readonly"},  {"rset", 3, "write"},
                    {"rget", 2, "readonly"},   {"rdel", 2, "write"},
                    {"rmod", 3, "write"},      {"rexist", 2, "readonly"},
                    {"hset", 3, "write"},      {"hget", 2, "readonly"},
                    {"hdel", 2, "write"},      {"hmod", 3, "write"},
                    {"hexist", 2, "readonly"}, {"skset", 3, "write"},
                    {"skget", 2, "readonly"},  {"skdel", 2, "write"},
                    {"skmod", 3, "write"},     {"skexist", 2, "readonly"},
                    {"save", 1, "write"},      {"command", 1, "readonly"}};
    int cmd_count = sizeof(cmd_info) / sizeof(cmd_info[0]);

    // 1. 数组头部
    length = sprintf(response, "*%d\r\n", cmd_count);

    // 2. 逐个添加完整命令描述子数组
    for (int i = 0; i < cmd_count; i++) {
      // 子数组包含 3 个元素：命令名、参数个数、属性数组
      length += sprintf(response + length, "*3\r\n");

      // 2.1 命令名（批量字符串）
      int name_len = strlen(cmd_info[i].name);
      length += sprintf(response + length, "$%d\r\n%s\r\n", name_len,
                        cmd_info[i].name);

      // 2.2 参数个数（整数）
      length += sprintf(response + length, ":%d\r\n", cmd_info[i].arity);

      // 2.3 属性数组（子数组）
      int flag_len = strlen(cmd_info[i].flags);
      length += sprintf(response + length, "*1\r\n$%d\r\n%s\r\n", flag_len,
                        cmd_info[i].flags);
    }
    break;
  }
  case SYNC: {
    if (g_config.enable_slave) {
      length = sprintf(response, "-ERR SYNC command is for master only\r\n");
      break;
    }
    // 设置同步标志，阻止写操作
    g_syncing = 1;
    printf("[SYNC] Starting full sync, write operations blocked\n");

    // 1. 执行 SAVE（将当前内存数据持久化到磁盘）
    if (g_config.enable_full_persist == 1) {
      dump_all_to_disk();
    }

    // 2. 回复 +OK（表示 SAVE 完成，Slave 可以开始 RDMA 接收）
    length = sprintf(response, "+OK\r\n");

    // 3. 重新打开文件读取最新内容，通过 RDMA 发送
    // 注意：不能直接用 kvs_log->file_data，因为那是旧的 mmap 映射，
    // dump_all_to_disk() 写入新数据后，mmap 内容可能未同步。
    pthread_t rdma_thread;
    struct rdma_mem_thread_arg {
      const void *data;
      size_t data_size;
      int rdma_port;
    };
    struct rdma_mem_thread_arg *arg = malloc(sizeof(struct rdma_mem_thread_arg));
    if (arg != NULL) {
      // 重新打开文件读取最新内容
      FILE *fp = fopen(g_config.log_path, "rb");
      if (fp) {
        fseek(fp, 0, SEEK_END);
        size_t file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (file_size > 0) {
          char *file_data = (char *)malloc(file_size);
          if (file_data) {
            size_t read_ret = fread(file_data, 1, file_size, fp);
            if (read_ret == file_size) {
              arg->data = file_data;
              arg->data_size = file_size;
              arg->rdma_port = g_config.slave_rdma_port;
              printf("[SYNC] Sending file data: %zu bytes via RDMA port %d\n",
                     arg->data_size, arg->rdma_port);
              pthread_create(&rdma_thread, NULL, &rdma_master_send_mem_thread, arg);
              pthread_detach(rdma_thread);
              fclose(fp);
              break; // 跳出，不执行下面的错误处理
            }
            free(file_data);
          }
        }
        fclose(fp);
      }
      free(arg);
      fprintf(stderr, "[SYNC] Failed to read file for RDMA send\n");
    } else {
      fprintf(stderr, "[SYNC] Failed to allocate RDMA thread arg\n");
    }
    g_syncing = 0;
    break;
  }
  // 未知命令
  default:
    length = sprintf(response, "-ERR unknown command\r\n");
    break;
  }

  len_rsp = strlen(response);
  return len_rsp;
}