#include "kvstore.h"
#include "config.h"
#include "rbtree.h"
#include "traversal.h"
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int APPEND_STR = 1;
extern file_content_t *kvs_log; // 日志文件mmap以及信息存储的结构体
extern file_content_t *Icr_log;
extern char *command[];

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

// 原有kvs_init函数...
void kvs_init(void) {
  if (g_config.enable_slab_pool) {
    printf("初始化内存池成功\n");
    kvs_init_slots();
  }
  if (g_config.enable_array) {
    global_array = (kvs_array_t *)malloc(sizeof(kvs_array_t));
    if (global_array == NULL) {
      perror("malloc global_array failed");
      exit(-1);
    }
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

int kvs_split_token(char *msg, char *tokens[]) {
  if (msg == NULL || tokens == NULL) {
    return -1;
  }
  int idx = 0;
  char *token = strtok(msg, " ");
  while (token != NULL && idx < KV_MAX) {
    tokens[idx] = (char *)kvs_malloc(strlen(token) + 1);
    if (tokens[idx] == NULL) {
      perror("malloc token failed");
      for (int i = 0; i < idx; i++) {
        kvs_free(tokens[i]);
      }
      return -1;
    }
    strcpy(tokens[idx], token);
    idx += 1;
    token = strtok(NULL, " ");
  }
  return idx;
}

// 新增：按&&拆分多条指令，返回指令条数
int kvs_split_multi_cmd(char *msg, char *cmds[]) {
  if (msg == NULL || cmds == NULL) {
    return -1;
  }

  int idx = 0;
  char *cmd = strtok(msg, g_config.cmd_delim);
  while (cmd != NULL && idx < g_config.max_cmd_num) {
    // 去除指令前后的空格（兼容"set a b && get a"和"set a b&&get a"）
    char *trimmed_cmd = cmd;
    // 跳过前导空格
    while (*trimmed_cmd == ' ' && *trimmed_cmd != '\0')
      trimmed_cmd++;
    // 去除尾部空格
    char *end = trimmed_cmd + strlen(trimmed_cmd) - 1;
    while (end > trimmed_cmd && *end == ' ')
      end--;
    *(end + 1) = '\0';

    // 分配内存存储指令（避免栈指针失效）
    cmds[idx] = (char *)kvs_malloc(strlen(trimmed_cmd) + 1);
    if (cmds[idx] == NULL) {
      perror("malloc cmd failed");
      // 回滚已分配的指令内存
      for (int i = 0; i < idx; i++) {
        kvs_free(cmds[i]);
      }
      return -1;
    }
    strcpy(cmds[idx], trimmed_cmd);
    idx++;
    cmd = strtok(NULL, g_config.cmd_delim);
  }
  return idx;
}
// tokens中是不包括\r\n的
int kvs_filter_protocol(char *tokens[], char *msg, int count, char *response) {
  // 安全初始化响应缓冲区的前几个字节
  int len_rsp = 0;
  response[0] = '\0';

  if (tokens == NULL || response == NULL || count == 0 || msg == NULL) {
    strcpy(response, "error: invalid params\r\n");
    return strlen(response);
  }
  if (tokens[0] == NULL) {
    strcpy(response, "error: empty command\r\n");
    return strlen(response);
  }

  int cmd = KVS_CMD_START;
  for (cmd = KVS_CMD_START; cmd < KVS_CMD_MAX; cmd++) {
    if (strcmp(tokens[0], command[cmd]) == 0) {
      break;
    }
  }

  int ret = 0;
  int length = 0;
  char *async_buf;
  size_t msg_len;
  switch (cmd) {
  case KVS_CMD_SET:
    ret = kvs_array_set(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_GET:
    char *result_array = kvs_array_get(global_array, tokens[1]);
    if (result_array == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_array);
    }
    break;
  case KVS_CMD_DEL:
    ret = kvs_array_del(global_array, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_MOD:
    ret = kvs_array_mod(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {

      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_EXIST:
    ret = kvs_array_exist(global_array, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_RSET:
    ret = kvs_rbtree_set(global_rbtree, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      // 分配堆内存（异步IO必须用堆内存！）
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_RGET:
    char *result_2 = kvs_rbtree_get(global_rbtree, tokens[1]);
    if (result_2 == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_2);
    }
    kvs_free(result_2);
    break;
  case KVS_CMD_RDEL:
    ret = kvs_rbtree_del(global_rbtree, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {

        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_RMOD:
    ret = kvs_rbtree_mod(global_rbtree, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_REXIST:
    ret = kvs_rbtree_exist(global_rbtree, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_HSET:
    ret = kvs_hash_set(global_hash, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      // 分配堆内存（异步IO必须用堆内存！）
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }

    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_HGET:
    char *result_hash = kvs_hash_get(global_hash, tokens[1]);
    if (result_hash == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_hash);
    }
    break;
  case KVS_CMD_HDEL:
    ret = kvs_hash_del(global_hash, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_HMOD:
    ret = kvs_hash_mod(global_hash, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_HEXIST:
    ret = kvs_hash_exist(global_hash, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_SKSET:
    ret = kvs_skiplist_set(global_skiplist, tokens[1], tokens[2]);
    if (g_config.enable_incremental_persist == 1) {
      msg_len = strlen(msg);
      // 分配堆内存（异步IO必须用堆内存！）
      async_buf = (char *)kvs_malloc(msg_len + 1);
      if (async_buf) {
        memcpy(async_buf, msg, msg_len);
        async_buf[msg_len] = '\0';
        uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0, writeCallback,
                        async_buf);
      }
    }
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_SKGET:
    char *result_sk = kvs_skiplist_get(global_skiplist, tokens[1]);
    if (result_sk == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_sk);
    }
    kvs_free(result_sk);
    break;
  case KVS_CMD_SKDEL:
    ret = kvs_skiplist_del(global_skiplist, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_SKMOD:
    ret = kvs_skiplist_mod(global_skiplist, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      if (g_config.enable_incremental_persist == 1) {
        msg_len = strlen(msg);
        // 分配堆内存（异步IO必须用堆内存！）
        async_buf = (char *)kvs_malloc(msg_len + 1);
        if (async_buf) {
          memcpy(async_buf, msg, msg_len);
          async_buf[msg_len] = '\0';
          uringAsyncWrite(Icr_log->file_fd, async_buf, msg_len, 0,
                          writeCallback, async_buf);
        }
      }
      length = sprintf(response, "OK\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case KVS_CMD_SKEXIST:
    ret = kvs_skiplist_exist(global_skiplist, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
  case SAVE:
#if ENABLE_FULL_PERSIST
    dump_all_to_disk();
    sprintf(response, "OK\r\n");
#endif
    break;
  default:
    length = sprintf(response, "error cmd\r\n");
    break;
  }
  len_rsp = strlen(response);
  return len_rsp;
}