#include "kvstore.h"
#include "resp.h"
#ifndef __SERVER_H__
#define __SERVER_H__

#define BUFFER_LENGTH (1024 * 10)
#define MAX_SEND_QUEUE 256
#define RBUF_INIT_CAP 4096
#define RBUF_MAX_CAP (256 * 1024)

typedef int (*RCALLBACK)(int fd);

// 连接结构体：动态接收缓冲区 + 零拷贝发送队列
typedef struct conn {
  int fd;

  // 接收缓存（动态分配，懒分配+翻倍扩容）
  char *rbuffer;   // 动态分配的接收缓冲区
  int rbuf_cap;    // 当前容量
  int rpos;        // 读指针（已解析到的位置）
  int rlength;     // 有效数据末尾位置

  // 发送队列（零拷贝设计，完全保留）
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
int set_event(int fd, int event, int flag);
int event_register(int fd, int event);
#endif