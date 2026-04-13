#include "kvstore.h"
#include "resp.h"
#ifndef __SERVER_H__
#define __SERVER_H__
#define MAX_SEND_QUEUE 64

typedef int (*RCALLBACK)(int fd);
#if START_SLAVE_NODE
#define MASTER_IP "192.168.140.129"
#define MASTER_PORT 8000
int master_conn_fd = -1; // 主→从 长连接的fd
#endif
typedef enum {
  CONN_CLIENT, // 客户端连接（处理get/exist）
  CONN_MASTER  // 主节点连接（处理set/del/mod同步）
} ConnType;
// 连接结构体：新增 type 标记区分连接类型
typedef struct conn {
  int fd;
  ConnType type; // 【关键】连接类型：客户端/主节点

  // 接收缓存（粘包用）
  char rbuffer[BUFFER_LENGTH];
  int rlength;
  uint32_t need_len;
  int parse_state;

  // 发送队列
  char wbuffer[BUFFER_LENGTH];
  uint32_t send_len_queue[MAX_SEND_QUEUE];
  uint32_t send_off_queue[MAX_SEND_QUEUE];
  int send_queue_head;
  int send_queue_tail;
  int send_queue_cnt;

  // 回调
  union {
    int (*recv_callback)(int fd);
    int (*accept_callback)(int fd);
  } r_action;
  int (*send_callback)(int fd);
} conn_t;

#endif