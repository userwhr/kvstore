
#include "config.h"
#include "iouringTodesk.h"
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

// 客户端类型
#define CLIENT_TYPE_NORMAL 0
#define CLIENT_TYPE_SLAVE 1

// kvs_filter_protocol 返回值结构体
typedef struct {
  int len;         // 响应长度（原返回值）
  int client_type; // 客户端类型：CLIENT_TYPE_NORMAL 或 CLIENT_TYPE_SLAVE
  int need_aof;    // 是否需要 AOF 落盘（写命令成功时为 1）
} kvs_result_t;

// kvstore.c的函数
typedef kvs_result_t (*kvs_handler)(int fd, char *tokens[], char *msg, int count,
                                    char *response);
void kvs_init(void);
int kvs_protocol(char *msg, int length, char *response);
int execute_recovered_cmds(const char *file);
int react_entrence(unsigned short port, kvs_handler hd);
int nytco_start(int port, kvs_handler hd);
int kvs_split_multi_cmd(char *msg, char *cmds[]);
int kvs_split_token(char *msg, char *tokens[]);
kvs_result_t kvs_filter_protocol(int fd, char *tokens[], char *msg, int count,
                                 char *response);

// Reactor 的 listen fd（在 reactor.c 中定义）
extern int g_listen_fd;

// Slave 的 client fd（在 reactor.c 中定义，用于 eBPF forwarder 转发写命令）
extern int g_slave_fd;

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
  SAVE,
  COMMAND,
  SYNC,
  ECHO,
  FLUSHALL,
  KVS_CMD_MAX,
};
#endif
