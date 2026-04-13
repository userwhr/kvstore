#include "resp.h"
#include "server.h"
#include <arpa/inet.h>
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

// 固定从节点 8888 端口
#define SLAVE_IP "127.0.0.1"
#define SLAVE_PORT 8888

#define CONNECTION_SIZE 1024
#define MAX_PORTS 20
#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

// ===================== 【全局主从连接FD】初始化 =-1 =====================
int g_ms_conn_fd = -1; // 全局唯一：主节点 <-> 从节点 连接fd
// ======================================================================

static kvs_handler handler;
extern file_content_t *kvs_log;
int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);
int event_register(int fd, int event);
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
#if START_MASTER_NODE
int master_connect_slave() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_in slave_addr;
  slave_addr.sin_family = AF_INET;
  slave_addr.sin_addr.s_addr = inet_addr(SLAVE_IP);
  slave_addr.sin_port = htons(SLAVE_PORT); // 固定8888

  printf("[主节点] 连接从节点 %s:%d...\n", SLAVE_IP, SLAVE_PORT);
  if (connect(fd, (struct sockaddr *)&slave_addr, sizeof(slave_addr)) < 0) {
    printf("[主节点] 连接失败！%s\n", strerror(errno));
    close(fd);
    return -1;
  }

  // ===================== 赋值全局主从fd =====================
  g_ms_conn_fd = fd;
  // ==========================================================
  event_register(fd, EPOLLOUT);
  printf("[主节点] ✅ 主从连接成功！全局fd=%d\n", g_ms_conn_fd);
  return fd;
}
#endif
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
// rbuffer：[包1头(4)][包1体(13)][包2头(4)][包2体(13)]
int recv_cb(int fd) {
  conn_t *conn = &conn_list[fd];
  int ret;

  // 1. 接收客户端数据（4000端口连接）
  ret =
      recv(fd, conn->rbuffer + conn->rlength, BUFFER_LENGTH - conn->rlength, 0);
  if (ret <= 0) {
    close(fd);
    sync_mmap_to_file(kvs_log);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
  }
  conn->rlength += ret;

  // 2. 循环解析粘包
  while (1) {
    // 保存【原始完整包】(头4字节+体) → 专用于转发从节点
    char raw_packet[BUFFER_LENGTH] = {0};
    int raw_packet_len = 0;

    // 步骤1：解析4字节协议头
    if (conn->parse_state == 0) {
      if (conn->rlength < 4)
        break;

      uint32_t tmp_len;
      if (parse_head(conn->rbuffer, &tmp_len) == -1)
        goto err_close;
      conn->need_len = tmp_len;

      if (conn->need_len > BUFFER_LENGTH - 4)
        goto err_close;

      // 保存完整原始包（转发用）
      raw_packet_len = 4 + conn->need_len;
      memcpy(raw_packet, conn->rbuffer, raw_packet_len);

      // 移除协议头，处理业务
      memmove(conn->rbuffer, conn->rbuffer + 4, conn->rlength - 4);
      conn->rlength -= 4;
      conn->parse_state = 1;
    }

    // 步骤2：解析完整消息体
    if (conn->parse_state == 1) {
      if (conn->rlength < conn->need_len)
        break;
      if (conn->need_len == 0) {
        conn->parse_state = 0;
        break;
      }

      // 协议校验
      if (conn->need_len == 1 || conn->rbuffer[conn->need_len - 2] != '\r' ||
          conn->rbuffer[conn->need_len - 1] != '\n') {
        goto err_close;
      }

// =====================
// 【功能1：转发原始包给从节点（全局fd）】=====================
#if START_MASTER_NODE
      if (g_ms_conn_fd >= 0) {
        conn_t *ms_conn = &conn_list[g_ms_conn_fd];
        if (ms_conn->send_queue_cnt >= MAX_SEND_QUEUE) {
          printf("[主节点] 从节点发送队列满，丢弃包\n");
          goto forward_end;
        }

        // 计算转发缓冲区偏移
        uint32_t curr_off = 0;
        if (ms_conn->send_queue_cnt > 0) {
          int last_idx = ms_conn->send_queue_tail - 1;
          if (last_idx < 0)
            last_idx = MAX_SEND_QUEUE - 1;
          curr_off = ms_conn->send_off_queue[last_idx] +
                     ms_conn->send_len_queue[last_idx];
        }

        if (curr_off + raw_packet_len >= BUFFER_LENGTH) {
          memset(ms_conn->wbuffer, 0, BUFFER_LENGTH);
          curr_off = 0;
        }

        // 原始包写入全局fd的发送缓冲区(wbuf)
        memcpy(ms_conn->wbuffer + curr_off, raw_packet, raw_packet_len);
        ms_conn->send_off_queue[ms_conn->send_queue_tail] = curr_off;
        ms_conn->send_len_queue[ms_conn->send_queue_tail] = raw_packet_len;
        ms_conn->send_queue_tail =
            (ms_conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
        ms_conn->send_queue_cnt++;

        // 触发send_cb发给从节点
        set_event(g_ms_conn_fd, EPOLLOUT, 0);
        // printf("[主节点] 已转发客户端原始包到从节点 | 长度：%d\n",
        //        raw_packet_len);
      }
    forward_end:
#endif
      // ================================================================================

      // ===================== 【功能2：原有逻辑 -
      // 解析并回复客户端（参数fd）】===================== 字符串收尾
      conn->rbuffer[conn->need_len - 2] = '\0';
      uint32_t curr_off = 0;
      if (conn->send_queue_cnt > 0) {
        int last_idx = conn->send_queue_tail - 1;
        if (last_idx < 0)
          last_idx = MAX_SEND_QUEUE - 1;
        curr_off =
            conn->send_off_queue[last_idx] + conn->send_len_queue[last_idx];
      }

      if (curr_off >= BUFFER_LENGTH)
        goto err_close;

      // handler解析业务 → 生成回复数据
      int curr_len = handler(conn->rbuffer, strlen(conn->rbuffer),
                             conn->wbuffer + curr_off);

      // 加入客户端发送队列
      if (conn->send_queue_cnt >= MAX_SEND_QUEUE)
        goto err_close;
      conn->send_off_queue[conn->send_queue_tail] = curr_off;
      conn->send_len_queue[conn->send_queue_tail] = curr_len;
      conn->send_queue_tail = (conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
      conn->send_queue_cnt++;
      // ========================================================================================

      // 清理已处理包
      memmove(conn->rbuffer, conn->rbuffer + conn->need_len,
              conn->rlength - conn->need_len);
      conn->rlength -= conn->need_len;
      conn->parse_state = 0;
      continue;
    }
  }

  // 触发客户端回复发送
  if (conn->send_queue_cnt > 0) {
    set_event(fd, EPOLLOUT, 0);
  }

  return ret;

// 异常关闭
err_close:
  close(fd);
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
  return 0;
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
  master_connect_slave();

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
