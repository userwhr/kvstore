#include "kvstore.h"
#include "rbtree.h"
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int APPEND_STR = 1;
extern file_content_t *kvs_log; // 日志文件mmap以及信息存储的结构体
extern int filefd;

// 【仅修复：补全缺失的逗号！持久化/宏完全不动】
#if START_SLAVE_NODE
const char *command[] = {"get",    "exist",  "rget",
                         "rexist", // ✅ 补逗号
                         "hget",
                         "hexist", // ✅ 补逗号
                         "skget",  "skexist"};
#else
// 指令集（和宏下标严格对应）
const char *command[] = {"set",   "get",   "del",   "mod",   "exist",
                         "rset",  "rget",  "rdel",  "rmod",  "rexist",
                         "hset",  "hget",  "hdel",  "hmod",  "hexist",
                         "skset", "skget", "skdel", "skmod", "skexist"};
#endif
const char *command_master[] = {"set",  "del",   "mod",   "rset",
                                "rdel", "rmod",  "hset",  "hdel",
                                "hmod", "skset", "skdel", "skmod"};
// 声明函数
int kvs_split_token(char *msg, char *tokens[]);
int kvs_filter_protocol(char *tokens[], int count, char *response);
// 新增：按&&拆分多条指令
int kvs_split_multi_cmd(char *msg, char *cmds[]);

// 原有全局变量定义...
#if ENABLE_ARRAY
kvs_array_t *global_array = NULL;
#endif
#if ENABLE_RBTREE
kvs_rbtree_t *global_rbtree = NULL;
#endif
#if ENABLE_HASH
kvs_hash_t *global_hash = NULL;
#endif
#if ENABLE_SKIPLIST
kvs_skiplist_t *global_skiplist = NULL;
#endif

// 原有kvs_init函数...
void kvs_init(void) {
#if ENABLE_SLAB_POOL
  kvs_init_slots();
#endif
#if ENABLE_ARRAY
  global_array = (kvs_array_t *)malloc(sizeof(kvs_array_t));
  if (global_array == NULL) {
    perror("malloc global_array failed");
    exit(-1);
  }
  kvs_array_creat(global_array);
#endif
#if ENABLE_RBTREE
  global_rbtree = (kvs_rbtree_t *)malloc(sizeof(kvs_rbtree_t));
  if (global_rbtree == NULL) {
    perror("malloc global_rbtree failed");
    exit(-1);
  }
  kvs_rbtree_create(global_rbtree);
#endif
#if ENABLE_HASH
  global_hash = (kvs_hash_t *)malloc(sizeof(kvs_hash_t));
  if (global_hash == NULL) {
    perror("malloc global_hash failed");
    exit(-1);
  }
  kvs_hash_creat(global_hash);
#endif
#if ENABLE_SKIPLIST
  global_skiplist = (kvs_skiplist_t *)malloc(sizeof(kvs_skiplist_t));
  if (global_skiplist == NULL) {
    perror("malloc global_skiplist failed");
    exit(-1);
  }
  kvs_skiplist_creat(global_skiplist);
#endif
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
  char *cmd = strtok(msg, CMD_DELIM);
  while (cmd != NULL && idx < MAX_CMD_NUM) {
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
    cmd = strtok(NULL, CMD_DELIM);
  }
  return idx;
}

int kvs_protocol(char *msg, int length, char *response,
                 int (*filer_protocol)(char *tokens[], int count,
                                       char *response)) {
  if (msg == NULL || length == 0 || response == NULL) {
    strcpy(response, "error: invalid params\r\n");
    return -1;
  }

  // 1. 保存原始msg（strtok会修改原字符串），避免破坏输入
  char *msg_copy = (char *)kvs_malloc(length + 1);
  if (msg_copy == NULL) {
    perror("malloc msg_copy failed");
    strcpy(response, "error: malloc failed\r\n");
    return -1;
  }
  strncpy(msg_copy, msg, length);
  msg_copy[length] = '\0';

  // 2. 按&&拆分多条指令
  char *cmds[MAX_CMD_NUM] = {0};
  int cmd_count = kvs_split_multi_cmd(msg_copy, cmds);
  if (cmd_count <= 0) {
    kvs_free(msg_copy);
    if (cmd_count == 0) {
      strcpy(response, "error: empty command\r\n");
    } else {
      strcpy(response, "error: split cmd failed\r\n");
    }
    return -1;
  }

  // 3. 遍历每条指令，解析并执行
  char temp_response[LINE_MAX] = {0}; // 单条指令响应缓冲区
  strcpy(response, "");               // 初始化总响应
  for (int i = 0; i < cmd_count; i++) {
    if (cmds[i] == NULL || strlen(cmds[i]) == 0) {
      strcat(response, "error: empty sub cmd\r\n");
      continue;
    }

#if ENABLE_INCREMENTAL_PERSIST
    append_string_to_file("kvstore_increase_kvs_log.txt", cmds[i]);
#endif

    // 3.1 拆分当前指令为token
    char *cmd_copy = (char *)kvs_malloc(strlen(cmds[i]) + 1);
    if (cmd_copy == NULL) {
      perror("malloc cmd_copy failed");
      strcat(response, "error: malloc cmd_copy failed\r\n");
      continue;
    }
    strcpy(cmd_copy, cmds[i]);
    char *tokens[KV_MAX] = {0};
    int token_count = kvs_split_token(cmd_copy, tokens);

    // 3.2 执行指令
    memset(temp_response, 0, sizeof(temp_response));
    if (token_count > 0) {
      filer_protocol(tokens, token_count, temp_response);
    } else {
      strcpy(temp_response, "error: split token failed\r\n");
    }

    // 3.3 汇总响应（每条指令的响应换行分隔）
    strcat(response, temp_response);

    // 3.4 清理当前指令的token内存
    for (int j = 0; j < token_count; j++) {
      if (tokens[j] != NULL) {
        kvs_free(tokens[j]);
      }
    }
    kvs_free(cmd_copy);
  }

  // 4. 清理资源
  for (int i = 0; i < cmd_count; i++) {
    if (cmds[i] != NULL) {
      kvs_free(cmds[i]);
    }
  }
  kvs_free(msg_copy);

  return strlen(response);
}
int kvs_filter_protocol(char *tokens[], int count, char *response) {
  memset(response, 0, strlen(response) + 1);

  if (tokens == NULL || count == 0 || response == NULL) {
    strcpy(response, "error: invalid params\r\n");
    return -1;
  }
  if (tokens[0] == NULL) {
    strcpy(response, "error: empty command\r\n");
    return -1;
  }

  int cmd = KVS_CMD_START;
  for (cmd = KVS_CMD_START; cmd < KVS_CMD_MAXREAD; cmd++) {
    if (strcmp(tokens[0], command[cmd]) == 0) {
      break;
    }
  }

  int ret = 0;
  int length = 0;
  char cmd_buf[LINE_MAX] = {0};
  // printf("filefd=%d\n", filefd);
  switch (cmd) {
#if ENABLE_ARRAY
  case KVS_CMD_GET:
    char *result_array = kvs_array_get(global_array, tokens[1]);
    if (result_array == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_array);
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
#endif
#if ENABLE_RBTREE
  case KVS_CMD_RGET:
    char *result_2 = kvs_rbtree_get(global_rbtree, tokens[1]);
    if (result_2 == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_2);
    }
    kvs_free(result_2);
    break;
  case KVS_CMD_REXIST:
    ret = kvs_rbtree_exist(global_rbtree, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
#if ENABLE_HASH
  case KVS_CMD_HGET:
    char *result_hash = kvs_hash_get(global_hash, tokens[1]);
    if (result_hash == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_hash);
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
#endif
#if ENABLE_SKIPLIST
  case KVS_CMD_SKGET:
    char *result_sk = kvs_skiplist_get(global_skiplist, tokens[1]);
    if (result_sk == NULL) {
      length = sprintf(response, "no exist\r\n");
    } else {
      length = sprintf(response, "%s\r\n", result_sk);
    }
    kvs_free(result_sk);
    break;
  case KVS_CMD_SKEXIST:
    ret = kvs_skiplist_exist(global_skiplist, tokens[1]);
    if (ret == 0) {
      length = sprintf(response, "exist\r\n");
    } else {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
  default:
    length = sprintf(response, "error cmd\r\n");
    break;
  }
  return 0;
}
int kvs_filter_protocol_master(char *tokens[], int count, char *response) {
  memset(response, 0, LINE_MAX);

  if (tokens == NULL || count == 0 || response == NULL) {
    strcpy(response, "error: invalid params\r\n");
    return -1;
  }
  if (tokens[0] == NULL) {
    strcpy(response, "error: empty command\r\n");
    return -1;
  }

  // ===================== 【唯一修改：遍历写指令数组 command_master】
  // ===================== 原来的代码遍历的是
  // command（从节点无写指令），现在改成遍历 command_master（写指令专用）
  int cmd = -1;
  for (int i = 0; i < 12; i++) { // command_master 共12个写指令
    if (strcmp(tokens[0], command_master[i]) == 0) {
      // 映射到你的指令宏
      if (i == 0)
        cmd = KVS_CMD_SET;
      if (i == 1)
        cmd = KVS_CMD_DEL;
      if (i == 2)
        cmd = KVS_CMD_MOD;
      if (i == 3)
        cmd = KVS_CMD_RSET; // ✅ rset 命中
      if (i == 4)
        cmd = KVS_CMD_RDEL;
      if (i == 5)
        cmd = KVS_CMD_RMOD;
      if (i == 6)
        cmd = KVS_CMD_HSET;
      if (i == 7)
        cmd = KVS_CMD_HDEL;
      if (i == 8)
        cmd = KVS_CMD_HMOD;
      if (i == 9)
        cmd = KVS_CMD_SKSET;
      if (i == 10)
        cmd = KVS_CMD_SKDEL;
      if (i == 11)
        cmd = KVS_CMD_SKMOD;
      break;
    }
  }
  // ====================================================================================

  // ===================== 以下代码【完全不动】，保留你的原版（含持久化）
  // =====================
  int ret = 0;
  int length = 0;
  char cmd_buf[LINE_MAX] = {0};
  switch (cmd) {
#if ENABLE_ARRAY
  case KVS_CMD_SET:
    ret = kvs_array_set(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      append_mmap_cmd(kvs_log, "set", tokens[1], tokens[2]);
#endif
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_DEL:
    ret = kvs_array_del(global_array, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = delete_mmap_cmd(kvs_log, "del", tokens[1]);
      if (log_ret != 0) {
        fprintf(stderr, "警告：删除key=%s的内存数据成功，但mmap清理失败\n",
                tokens[1]);
      }
#endif
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_MOD:
    ret = kvs_array_mod(global_array, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = modify_mmap_cmd(kvs_log, "mod", tokens[1], tokens[2]);
      if (log_ret == 1) {
        fprintf(stderr, "未找到目标指令\n");
      }
#endif
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
#if ENABLE_RBTREE
  case KVS_CMD_RSET: // ✅ 现在能走到这里了！
    ret = kvs_rbtree_set(global_rbtree, tokens[1], tokens[2]);
    append_mmap_cmd(kvs_log, "rset", tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_RDEL:
    ret = kvs_rbtree_del(global_rbtree, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = delete_mmap_cmd(kvs_log, "rdel", tokens[1]);
      if (log_ret != 0) {
        fprintf(stderr, "警告：删除key=%s的内存数据成功，但mmap清理失败\n",
                tokens[1]);
      }
#endif
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_RMOD:
    ret = kvs_rbtree_mod(global_rbtree, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = modify_mmap_cmd(kvs_log, "rmod", tokens[1], tokens[2]);
      if (log_ret == 1) {
        fprintf(stderr, "未找到目标指令\n");
      }
#endif
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
// ... 剩下的hash、跳表代码【完全不动】
#if ENABLE_HASH
  case KVS_CMD_HSET:
    ret = kvs_hash_set(global_hash, tokens[1], tokens[2]);
    append_mmap_cmd(kvs_log, "hset", tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_HDEL:
    ret = kvs_hash_del(global_hash, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = delete_mmap_cmd(kvs_log, "hdel", tokens[1]);
      if (log_ret != 0) {
        fprintf(stderr, "警告：删除key=%s的内存数据成功，但mmap清理失败\n",
                tokens[1]);
      }
#endif
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_HMOD:
    ret = kvs_hash_mod(global_hash, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = modify_mmap_cmd(kvs_log, "hmod", tokens[1], tokens[2]);
      if (log_ret == 1) {
        fprintf(stderr, "未找到目标指令\n");
      }
#endif
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
#if ENABLE_SKIPLIST
  case KVS_CMD_SKSET:
    ret = kvs_skiplist_set(global_skiplist, tokens[1], tokens[2]);
    append_mmap_cmd(kvs_log, "skset", tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "exist\r\n");
    }
    break;
  case KVS_CMD_SKDEL:
    ret = kvs_skiplist_del(global_skiplist, tokens[1]);
    if (ret == -1) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = delete_mmap_cmd(kvs_log, "skdel", tokens[1]);
      if (log_ret != 0) {
        fprintf(stderr, "警告：删除key=%s的内存数据成功，但mmap清理失败\n",
                tokens[1]);
      }
#endif
      length = sprintf(response, "success\r\n");
    }
    break;
  case KVS_CMD_SKMOD:
    ret = kvs_skiplist_mod(global_skiplist, tokens[1], tokens[2]);
    if (ret < 0) {
      length = sprintf(response, "error\r\n");
    } else if (ret == 0) {
#if ENABLE_FULL_PERSIST
      int log_ret = modify_mmap_cmd(kvs_log, "skmod", tokens[1], tokens[2]);
      if (log_ret == 1) {
        fprintf(stderr, "未找到目标指令\n");
      }
#endif
      length = sprintf(response, "ok\r\n");
    } else if (ret > 0) {
      length = sprintf(response, "key not exist\r\n");
    }
    break;
#endif
  default:
    length = sprintf(response, "error cmd\r\n");
    break;
  }
  return 0;
}