#include "array.h"
#include "config.h"
#include "kvs_persist.h"
#include "mempool.h"
#include "rbtree.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __KVSTORE_H
#define __KVSTORE_H
// kvstore.c 中的宏定义
#define KV_MAX 1024 // 单条指令支持的最大tokens数

// kvstore.c的函数
typedef int (*kvs_handler)(char *msg, int length, char *response);
void kvs_init(void);
int kvs_protocol(char *msg, int length, char *response);
int execute_recovered_cmds(const char *file);
int react_entrence(unsigned short port, kvs_handler hd);
int nytco_start(int port, kvs_handler hd);
int kvs_split_multi_cmd(char *msg, char *cmds[]);
int kvs_split_token(char *msg, char *tokens[]);
int kvs_filter_protocol(char *tokens[], int count, char *response);

// 不直接使用系统调用分配内存，优势:跨平台能力
void *kvs_malloc(size_t size);
void kvs_free(void *table);

enum {
  KVS_CMD_START = 0,
  KVS_CMD_SET = KVS_CMD_START,
  KVS_CMD_GET,
  KVS_CMD_DEL,
  KVS_CMD_MOD,
  KVS_CMD_EXIST,
  // rbtree
  KVS_CMD_RSET,
  KVS_CMD_RGET,
  KVS_CMD_RDEL,
  KVS_CMD_RMOD,
  KVS_CMD_REXIST,
  // hash
  KVS_CMD_HSET,
  KVS_CMD_HGET,
  KVS_CMD_HDEL,
  KVS_CMD_HMOD,
  KVS_CMD_HEXIST,
  // skiplist
  KVS_CMD_SKSET,
  KVS_CMD_SKGET,
  KVS_CMD_SKDEL,
  KVS_CMD_SKMOD,
  KVS_CMD_SKEXIST,

  KVS_CMD_MAX,
};
#endif