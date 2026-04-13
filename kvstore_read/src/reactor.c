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

// ===================== 读节点固定配置 =====================
#define CONNECTION_SIZE 1024
#define TIME_SUB_MS(tv1, tv2)                                                  \
  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int g_master_listen_fd = -1;
// ===================== 【新增】全局FD：从节点 <-> 主节点 的连接
// =====================
int master_client_fd = -1; // 全局唯一：标记从节点与主节点的连接
// ================================================================================
extern file_content_t *kvs_log;

// 全局业务处理函数（双处理函数）
int (*client_handler)(
    char *, int, char *,
    int (*filer_protocol)(char *tokens[], int count,
                          char *response)); // 客户端：kvs_protocol
int (*master_handler)(
    char *, int, char *,
    int (*filer_protocol_master)(
        char *tokens[], int count,
        char *response)); // 主节点：kvs_filter_protocol_master

// 回调函数声明
int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

// epoll 全局句柄
int epfd = 0;
struct timeval begin;

conn_t conn_list[CONNECTION_SIZE] = {0};

// ===================== epoll 事件管理（不变） =====================
int set_event(int fd, int event, int flag) {
  struct epoll_event ev;
  ev.events = event;
  ev.data.fd = fd;
  if (flag) {
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
  } else {
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
  }
  return 0;
}

// 注册连接：指定连接类型
int event_register(int fd, int event, ConnType type) {
  if (fd < 0)
    return -1;

  conn_list[fd].fd = fd;
  conn_list[fd].type = type; // 绑定连接类型
  conn_list[fd].r_action.recv_callback = recv_cb;
  conn_list[fd].send_callback = send_cb;

  set_event(fd, event, 1);
  return 0;
}

int accept_cb(int listen_fd) {
  struct sockaddr_in clientaddr;
  socklen_t len = sizeof(clientaddr);
  int clientfd = accept(listen_fd, (struct sockaddr *)&clientaddr, &len);

  if (clientfd < 0) {
    printf("accept errno: %d --> %s\n", errno, strerror(errno));
    return -1;
  }

  ConnType type;
  if (listen_fd == g_master_listen_fd) {
    type = CONN_MASTER;
    // ===================== 【修改】赋值全局主节点连接FD =====================
    master_client_fd = clientfd; // 全局FD绑定主节点连接
    // ======================================================================
    printf("[成功] 主从同步端口建立连接 | 主节点(写端)已连接 | 客户端fd=%d\n",
           clientfd);
  } else {
    type = CONN_CLIENT;
    printf("[信息] 客户端端口建立连接 | 普通客户端已连接 | 客户端fd=%d\n",
           clientfd);
  }

  event_register(clientfd, EPOLLIN, type);

  return 0;
}

// ===================== 核心recv：主节点只收不发，客户端正常收发
// =====================
int recv_cb(int fd) {
  conn_t *conn = &conn_list[fd];
  int ret;

  // 1. 接收数据
  ret =
      recv(fd, conn->rbuffer + conn->rlength, BUFFER_LENGTH - conn->rlength, 0);
  if (ret <= 0) {
    close(fd);
    if (fd == master_client_fd) {
      printf("[警告] 主节点连接断开！fd=%d\n", fd);
      master_client_fd = -1;
    }
    sync_mmap_to_file(kvs_log);
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    return 0;
  }
  conn->rlength += ret;

  // 2. 循环解析粘包
  while (1) {
    // 临时废弃缓冲区：专门给master_handler用，不发送、不占用连接wbuf
    char temp_rsp[BUFFER_LENGTH] = {0};

    // 步骤1：读取4字节协议头
    if (conn->parse_state == 0) {
      if (conn->rlength < 4)
        break;

      uint32_t tmp_len;
      if (parse_head(conn->rbuffer, &tmp_len) == -1)
        goto err_close;
      conn->need_len = tmp_len;

      if (conn->need_len > BUFFER_LENGTH - 4)
        goto err_close;

      // 移除4字节头
      memmove(conn->rbuffer, conn->rbuffer + 4, conn->rlength - 4);
      conn->rlength -= 4;
      conn->parse_state = 1;
    }

    // 步骤2：读取完整消息体
    if (conn->parse_state == 1) {
      if (conn->rlength < conn->need_len)
        break;
      if (conn->need_len == 0) {
        conn->parse_state = 0;
        break;
      }

      // 协议校验（仅客户端校验，主节点跳过）
      int proto_error = 0;
      if (fd != master_client_fd) {
        if (conn->need_len == 1 || conn->rbuffer[conn->need_len - 2] != '\r' ||
            conn->rbuffer[conn->need_len - 1] != '\n') {
          proto_error = 1;
        }
      }
      if (proto_error)
        goto err_close;

      // 字符串收尾
      if (conn->need_len >= 2)
        conn->rbuffer[conn->need_len - 2] = '\0';
      else
        conn->rbuffer[conn->need_len] = '\0';

      // 打印日志
      // const char *type_str = (fd == master_client_fd) ? "主节点" : "客户端";
      // printf("[recv解析成功] fd=%d | 连接类型: %s | 收到数据: %s\n", fd,
      //        type_str, conn->rbuffer);

      // ===================== 【修复段错误】=====================
      // 1. 主节点连接：用临时缓冲区处理，不发送任何响应
      if (fd == master_client_fd) {
        // 传入临时缓冲区，解决NULL段错误
        master_handler(conn->rbuffer, strlen(conn->rbuffer), temp_rsp,
                       kvs_filter_protocol_master);
      }
      // 2. 客户端连接：正常处理 + 正常回复
      else {
        uint32_t curr_off = 0;
        int curr_len =
            client_handler(conn->rbuffer, strlen(conn->rbuffer),
                           conn->wbuffer + curr_off, kvs_filter_protocol);

        // 发送队列
        conn->send_off_queue[conn->send_queue_tail] = curr_off;
        conn->send_len_queue[conn->send_queue_tail] = curr_len;
        conn->send_queue_tail = (conn->send_queue_tail + 1) % MAX_SEND_QUEUE;
        conn->send_queue_cnt++;
      }
      // ======================================================

      // 清理已处理包
      memmove(conn->rbuffer, conn->rbuffer + conn->need_len,
              conn->rlength - conn->need_len);
      conn->rlength -= conn->need_len;
      conn->parse_state = 0;
      continue;
    }
  }

  // 只有客户端需要触发发送
  if (conn->send_queue_cnt > 0 && fd != master_client_fd) {
    set_event(fd, EPOLLOUT, 0);
  }

  return ret;

err_close:
  close(fd);
  if (fd == master_client_fd)
    master_client_fd = -1;
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
  return 0;
}

// ===================== 发送回调（完全不变） =====================
int send_cb(int fd) {
  conn_t *conn = &conn_list[fd];
  int send_ret = 0;

  while (conn->send_queue_cnt > 0) {
    uint32_t curr_off = conn->send_off_queue[conn->send_queue_head];
    uint32_t curr_len = conn->send_len_queue[conn->send_queue_head];

    send_ret = send(fd, conn->wbuffer + curr_off, curr_len, 0);
    if (send_ret <= 0) {
      close(fd);
      // 主节点断开，重置全局FD
      if (fd == master_client_fd) {
        master_client_fd = -1;
      }
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      return send_ret;
    }

    conn->send_queue_head = (conn->send_queue_head + 1) % MAX_SEND_QUEUE;
    conn->send_queue_cnt--;
  }

  memset(conn->wbuffer, 0, BUFFER_LENGTH);
  set_event(fd, EPOLLIN, 0);
  return send_ret;
}

// ===================== 初始化服务器：绑定端口 =====================
int init_server(unsigned short port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(port);

  if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
    printf("bind port %d failed: %s\n", port, strerror(errno));
  }
  listen(sockfd, 10);
  printf("[读节点] 监听端口: %d\n", port);
  return sockfd;
}

// ===================== 读节点主入口：双端口+双处理函数 =====================
int react_entrence(
    unsigned short client_port, unsigned short master_sync_port,
    int (*cli_hd)(char *, int, char *,
                  int (*filer_protocol)(char *tokens[], int count,
                                        char *response)),
    int (*mas_hd)(char *, int, char *,
                  int (*filer_protocol_master)(char *tokens[], int count,
                                               char *response))) {
  // 绑定全局处理函数
  client_handler = cli_hd;
  master_handler = mas_hd;

  epfd = epoll_create(1);

  // 1. 监听【客户端端口】：处理客户端读请求
  int client_listen_fd = init_server(client_port);
  conn_list[client_listen_fd].fd = client_listen_fd;
  conn_list[client_listen_fd].r_action.accept_callback = accept_cb;
  set_event(client_listen_fd, EPOLLIN, 1);

  // 2. 监听【主从同步端口】：接收写节点同步数据（全局变量）
  g_master_listen_fd = init_server(master_sync_port);
  conn_list[g_master_listen_fd].fd = g_master_listen_fd;
  conn_list[g_master_listen_fd].r_action.accept_callback = accept_cb;
  set_event(g_master_listen_fd, EPOLLIN, 1);

  gettimeofday(&begin, NULL);

  // epoll 主循环
  while (1) {
    struct epoll_event events[1024] = {0};
    int nready = epoll_wait(epfd, events, 1024, -1);

    for (int i = 0; i < nready; i++) {
      int connfd = events[i].data.fd;
      if (events[i].events & EPOLLIN) {
        // 监听fd走accept，连接fd走recv
        if (connfd == client_listen_fd || connfd == g_master_listen_fd) {
          accept_cb(connfd);
        } else {
          conn_list[connfd].r_action.recv_callback(connfd);
        }
      }
      if (events[i].events & EPOLLOUT) {
        conn_list[connfd].send_callback(connfd);
      }
    }
  }
}