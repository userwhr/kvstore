#include "resp.h"
#include "server.h"
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CONNECTION_SIZE 1024
#define MAX_PORTS 20
#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)
char global_rbuff[1024 * 1024];
static kvs_handler handler;
extern file_content_t *kvs_log;
int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

int epfd = 0;
struct timeval begin;

struct conn conn_list[CONNECTION_SIZE] = {0};
// fd
int set_event(int fd, int event, int flag) {

  if (flag) { // non-zero add

    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

  } else { // zero mod

    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
  }
}

int event_register(int fd, int event) {

  if (fd < 0)
    return -1;

  conn_list[fd].fd = fd;
  conn_list[fd].r_action.recv_callback = recv_cb;
  conn_list[fd].send_callback = send_cb;

  set_event(fd, event, 1);
}

// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

  struct sockaddr_in clientaddr;
  socklen_t len = sizeof(clientaddr);

  int clientfd = accept(fd, (struct sockaddr *)&clientaddr, &len);
  // printf("accept finshed: %d\n", clientfd);
  if (clientfd < 0) {
    printf("accept errno: %d --> %s\n", errno, strerror(errno));
    return -1;
  }
  // printf("建立链接成功,fd=%d的\n", clientfd);

  event_register(clientfd, EPOLLIN); // | EPOLLET

  if ((clientfd % 1000) == 0) {

    struct timeval current;
    gettimeofday(&current, NULL);

    int time_used = TIME_SUB_MS(current, begin);
    memcpy(&begin, &current, sizeof(struct timeval));

    printf("accept finshed: %d, time_used: %d\n", clientfd, time_used);
  }

  return 0;
}

// 工具函数：计算字符串长度（遇到 \r 停止，不修改原数据）
static int str_len_raw(char *s) {
  int len = 0;
  while (s[len] != '\r' && s[len] != '\0')
    len++;
  return len;
}

int recv_cb(int fd) {
  // printf("进入recv_cb\n");
  conn_t *conn = &conn_list[fd];
  int ret;

  ret =
      recv(fd, conn->rbuffer + conn->rlength, BUFFER_LENGTH - conn->rlength, 0);
  if (ret <= 0) {
    close(fd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
  }
  conn->rlength += ret;

  while (1) {
    if (conn->rlength <= 0)
      break;

    char *cmd = NULL;
    char *params[10] = {0};
    int paramCnt = 0;
    StoreType type;

    // 调用新的parse：不修改rbuffer，cmd/params自带\0
    int parseRet =
        parseRespKv(conn->rbuffer, &cmd, &params[0], &paramCnt, &type);
    if (parseRet == -1)
      break;
    if (parseRet < 0) {
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return 0;
    }

    // ====================== 你的原有逻辑 ======================
    // ✅ msg 正常拷贝完整的原始报文，没有任何破坏！
    int oneRespLen = getOneRespLen(conn->rbuffer);
    char msg[oneRespLen + 1];
    memcpy(msg, conn->rbuffer, oneRespLen); // 原始数据完整！
    msg[oneRespLen] = '\0';

    // 直接使用 cmd/params（自带\0，无乱码）
    char *tokens[20] = {0};
    tokens[0] = cmd;
    for (int i = 0; i < paramCnt; i++)
      tokens[i + 1] = params[i];

    // 你的handler逻辑（完全不变）
    uint32_t curr_off = 0;
    if (conn->send_queue_cnt > 0) {
      int last_idx = conn->send_queue_tail - 1;
      if (last_idx < 0)
        last_idx = MAX_SEND_QUEUE - 1;
      curr_off =
          conn->send_off_queue[last_idx] + conn->send_len_queue[last_idx];
    }
    int curr_len = handler(tokens, msg, paramCnt + 1, conn->wbuffer + curr_off);

    // ====================== 释放解析分配的内存 ======================
    free(cmd);
    for (int i = 0; i < paramCnt; i++)
      free(params[i]);
    // ==============================================================

    // 你的发送队列、缓冲区移动逻辑（完全不变）
    if (conn->send_queue_cnt >= MAX_SEND_QUEUE) {
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return 0;
    }
    conn->send_off_queue[conn->send_queue_tail] = curr_off;
    conn->send_len_queue[conn->send_queue_tail] = curr_len;
    conn->send_queue_tail = (conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
    conn->send_queue_cnt++;

    memmove(conn->rbuffer, conn->rbuffer + oneRespLen,
            conn->rlength - oneRespLen);
    conn->rlength -= oneRespLen;
  }

  if (conn->send_queue_cnt > 0)
    set_event(fd, EPOLLOUT, 0);
  return ret;
}

// send_cb 完全不用改！用你原来的即可
int send_cb(int fd) {
  conn_t *conn = &conn_list[fd];
  int send_ret = 0;

  // 循环发送：直到队列为空
  while (conn->send_queue_cnt > 0) {
    // 1. 取队列头：当前要发送的【偏移】+【长度】
    uint32_t curr_off = conn->send_off_queue[conn->send_queue_head];
    uint32_t curr_len = conn->send_len_queue[conn->send_queue_head];

    // 2. 从偏移位置开始发送（分包发送）
    // printf("send->%s", conn->wbuffer);
    send_ret = send(fd, conn->wbuffer + curr_off, curr_len, 0);

    if (send_ret <= 0) { // 发送失败
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return send_ret;
    }
    // printf("发送成功，长度：%d\n", curr_len);
    // printf("wbuf=%s\n", conn->wbuffer);

    // 3. 发完一个，队列头后移，计数-1
    conn->send_queue_head = (conn->send_queue_head + 1) % MAX_SEND_QUEUE;
    conn->send_queue_cnt--;
  }

  // 4. 所有响应发完：清空缓存，切回接收
  memset(conn->wbuffer, 0, BUFFER_LENGTH);
  set_event(fd, EPOLLIN, 0);

  return send_ret;
}
int init_server(unsigned short port) {

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);

  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
  servaddr.sin_port = htons(port);              // 0-1023,

  if (-1 ==
      bind(sockfd, (struct sockaddr *)&servaddr, sizeof(struct sockaddr))) {
    printf("bind failed: %s\n", strerror(errno));
  }

  listen(sockfd, 10);
  // printf("listen finshed: %d\n", sockfd); // 3

  return sockfd;
}

int react_entrence(unsigned short port, kvs_handler hd) {

  handler = hd;
  epfd = epoll_create(1);

  int i = 0;

  for (i = 0; i < MAX_PORTS; i++) {

    int sockfd = init_server(port + i);

    conn_list[sockfd].fd = sockfd;
    conn_list[sockfd].r_action.recv_callback = accept_cb;

    set_event(sockfd, EPOLLIN, 1);
  }

  gettimeofday(&begin, NULL);

  while (1) { // mainloop

    struct epoll_event events[1024] = {0};
    int nready = epoll_wait(epfd, events, 1024, -1);

    int i = 0;
    for (i = 0; i < nready; i++) {

      int connfd = events[i].data.fd;

#if 0
			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} else if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

#else
      if (events[i].events & EPOLLIN) {
        conn_list[connfd].r_action.recv_callback(connfd);
      }

      if (events[i].events & EPOLLOUT) {
        conn_list[connfd].send_callback(connfd);
      }
#if ENABLE_INCREMENTAL_PERSIST
      uringProcessCqes(); // 执行回调函数,释放堆上资源
#endif
#endif
    }
  }
}
