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

  // 1. 接收数据（不变，解决粘包）
  ret =
      recv(fd, conn->rbuffer + conn->rlength, BUFFER_LENGTH - conn->rlength, 0);
  if (ret <= 0) {
    close(fd);
    sync_mmap_to_file(kvs_log);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
  }
  conn->rlength += ret;

  // 2. 循环解析粘包（不变）
  while (1) {
    if (conn->parse_state == 0) { // 读4字节头
      if (conn->rlength < 4)
        break;

      uint32_t tmp_len;
      if (parse_head(conn->rbuffer, &tmp_len) == -1) {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }
      conn->need_len = tmp_len;

      if (conn->need_len > BUFFER_LENGTH - 4) {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }

      memmove(conn->rbuffer, conn->rbuffer + 4, conn->rlength - 4);
      conn->rlength -= 4;
      conn->parse_state = 1;
    }

    if (conn->parse_state == 1) { // 读数据
      if (conn->rlength < conn->need_len)
        break;

      // ===================== 【修复：数据读完=0，break】 =====================
      if (conn->need_len == 0) {
        conn->parse_state = 0;
        break;
      }

      // 协议非法校验（不变）
      if (conn->need_len == 1) {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }
      if (conn->rbuffer[conn->need_len - 2] != '\r' ||
          conn->rbuffer[conn->need_len - 1] != '\n') {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }
      // =====================================================================

      conn->rbuffer[conn->need_len - 2] = '\0';
      printf("收到的数据为%s",conn->rbuffer);

      // ===================== 【核心：发送队列追加（你的设计）】
      // =====================
      // 1. 计算当前响应在wbuffer的【起始偏移】（队列尾的偏移+长度）
      uint32_t curr_off = 0;
      if (conn->send_queue_cnt > 0) {
        int last_idx = conn->send_queue_tail - 1;
        if (last_idx < 0)
          last_idx = MAX_SEND_QUEUE - 1;
        curr_off =
            conn->send_off_queue[last_idx] + conn->send_len_queue[last_idx];
      }

      // 2. 校验偏移越界
      if (curr_off >= BUFFER_LENGTH) {
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }

      // 3. handler零拷贝：直接写 wbuffer + 偏移（绝不覆盖！）
      int curr_len = handler(conn->rbuffer, strlen(conn->rbuffer),
                             conn->wbuffer + curr_off);

      // 4. 【你的要求】把【偏移+长度】存入数组队列
      if (conn->send_queue_cnt >= MAX_SEND_QUEUE) { // 队列满，拒绝服务
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
      }
      conn->send_off_queue[conn->send_queue_tail] = curr_off; // 存偏移（起点）
      conn->send_len_queue[conn->send_queue_tail] = curr_len; // 存长度
      conn->send_queue_tail = (conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
      conn->send_queue_cnt++;
      // ============================================================================

      // 移除已处理包（不变）
      memmove(conn->rbuffer, conn->rbuffer + conn->need_len,
              conn->rlength - conn->need_len);
      conn->rlength -= conn->need_len;
      conn->parse_state = 0;
      continue;
    }
  }

  // 有排队响应，切换EPOLLOUT
  if (conn->send_queue_cnt > 0) {
    set_event(fd, EPOLLOUT, 0);
  }

  return ret;
}
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
