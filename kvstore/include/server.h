#include "kvstore.h"
#include "resp.h"
#ifndef __SERVER_H__
#define __SERVER_H__

#define BUFFER_LENGTH 1024 * 10
#define MAX_SEND_QUEUE 64

typedef int (*RCALLBACK)(int fd);

// 连接结构体：保留你原有设计，仅废弃旧包头字段，复用为RESP解析
typedef struct conn {
  int fd;

  // 接收缓存（TCP粘包通用缓冲区）
  char rbuffer[BUFFER_LENGTH];
  int rlength;     // 缓冲区有效数据长度
  int parse_state; // RESP解析状态：0=初始 1=解析中
  uint32_t need_len;

  // 发送队列（你的零拷贝设计，完全保留）
  char wbuffer[BUFFER_LENGTH];
  uint32_t send_len_queue[MAX_SEND_QUEUE];
  uint32_t send_off_queue[MAX_SEND_QUEUE];
  int send_queue_head;
  int send_queue_tail;
  int send_queue_cnt;

  union {
    RCALLBACK recv_callback;
    RCALLBACK accept_callback;
  } r_action;
  RCALLBACK send_callback;
} conn_t;

#endif