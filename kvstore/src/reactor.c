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
  printf("建立链接成功,fd=%d的\n", clientfd);

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

int recv_cb(int fd) {
  conn_t *conn = &conn_list[fd];
  int ret;

  // 1. 接收数据（和旧逻辑完全一样）
  ret =
      recv(fd, conn->rbuffer + conn->rlength, BUFFER_LENGTH - conn->rlength, 0);
  if (ret <= 0) {
    close(fd);
    sync_mmap_to_file(kvs_log);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
  }
  conn->rlength += ret;

  // 2. 循环解析粘包 → **完全复刻你旧代码的循环结构**
  while (1) {
    // ============== 修复：缓冲区空，直接退出（和旧逻辑一致） ==============
    if (conn->rlength <= 0) {
      break;
    }

    // ===================== RESP 协议解析（替换旧的4字节头）
    // =====================
    char *cmd = NULL;
    char *params[10] = {0};
    int paramCnt = 0;
    StoreType type;

    int parseRet =
        parseRespKv(conn->rbuffer, &cmd, &params[0], &paramCnt, &type);
    // 半包 → 直接break，不return（和旧逻辑完全一致！）
    if (parseRet == -1) {
      break;
    }
    // 协议错误 → 关闭连接（和旧逻辑一致）
    if (parseRet < 0) {
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return 0;
    }

    // ===================== 【核心：发送队列逻辑 → 100% 复用你旧代码】
    // =====================
    // 1. 计算偏移（和你原来的代码一字不差）
    uint32_t curr_off = 0;
    if (conn->send_queue_cnt > 0) {
      int last_idx = conn->send_queue_tail - 1;
      if (last_idx < 0)
        last_idx = MAX_SEND_QUEUE - 1;
      curr_off =
          conn->send_off_queue[last_idx] + conn->send_len_queue[last_idx];
    }

    // 2. 越界校验（一字不差）
    if (curr_off >= BUFFER_LENGTH) {
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return 0;
    }

    // 3. 构建tokens + 调用业务函数
    char *tokens[20] = {0};
    int tokenIdx = 0;
    tokens[tokenIdx++] = cmd;
    for (int i = 0; i < paramCnt; i++) {
      tokens[tokenIdx++] = params[i];
    }
    tokens[tokenIdx] = NULL;

    // 调用handler（适配你的函数签名）
    int curr_len =
        kvs_filter_protocol(tokens, paramCnt + 1, conn->wbuffer + curr_off);
    printf("kvs_filter_protocol执行完成，响应长度：%d\n", curr_len);

    // 4. 入发送队列（**和你旧代码完全一样**）
    if (conn->send_queue_cnt >= MAX_SEND_QUEUE) {
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return 0;
    }
    conn->send_off_queue[conn->send_queue_tail] = curr_off;
    conn->send_len_queue[conn->send_queue_tail] = curr_len;
    conn->send_queue_tail = (conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
    conn->send_queue_cnt++;
    // ============================================================================

    // ===================== 移除已解析数据（RESP版，和旧逻辑一致）
    // =====================
    int oneRespLen = getOneRespLen(conn->rbuffer);
    memmove(conn->rbuffer, conn->rbuffer + oneRespLen,
            conn->rlength - oneRespLen);
    conn->rlength -= oneRespLen;

    // ============== 关键修复：解析完一条 → CONTINUE！和你旧代码完全一致
    // ==============
    continue;
  }

  // 有响应待发送 → 切换EPOLLOUT（**和旧逻辑一字不差**）
  if (conn->send_queue_cnt > 0) {
    set_event(fd, EPOLLOUT, 0);
  }

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
    send_ret = send(fd, conn->wbuffer + curr_off, curr_len, 0);

    if (send_ret <= 0) { // 发送失败
      close(fd);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return send_ret;
    }
    printf("发送成功，长度：%d\n", curr_len);

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
#endif
    }
  }
}
