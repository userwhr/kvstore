#include "kvstore.h"
#include "resp.h"
#ifndef __SERVER_H__
#define __SERVER_H__
#define BUFFER_LENGTH 1024 * 10
#define MAX_SEND_QUEUE 64

typedef int (*RCALLBACK)(int fd);

typedef struct conn {
  int fd;

  // 接收缓存（粘包用，不变）
  char rbuffer[BUFFER_LENGTH];
  int rlength;
  uint32_t need_len;
  int parse_state;

  // ===================== 【你的设计：发送队列】 =====================
  char wbuffer[BUFFER_LENGTH];             // 总发送缓存（零拷贝）
  uint32_t send_len_queue[MAX_SEND_QUEUE]; // 数组：每个响应的长度
  uint32_t send_off_queue[MAX_SEND_QUEUE]; // 数组：每个响应的偏移（起点）
  int send_queue_head;                     // 队列头（待发送）
  int send_queue_tail;                     // 队列尾（待追加）
  int send_queue_cnt;                      // 队列中待发送响应数量
  union {
    RCALLBACK recv_callback;
    RCALLBACK accept_callback;
  } r_action;
  RCALLBACK send_callback;
} conn_t;
#endif