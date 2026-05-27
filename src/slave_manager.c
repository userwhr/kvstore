/*
 * ============================================================================
 * Slave 连接管理模块
 *
 * 管理多个 Slave 连接的 fd、同步状态
 * 支持一对多主从同步
 * ============================================================================
 */
#include "slave_manager.h"
#include "config.h"
#include "ebpf_forwarder.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* ==================== Forwarder 进程管理 ==================== */
static pid_t g_forwarder_pid = -1;
static int g_forwarder_started = 0;

/*
 * 启动 eBPF forwarder 子进程（按需启动，第一个 Slave 注册时调用）
 * 返回：0 成功，-1 失败
 */
static int start_forwarder(void) {
  if (g_forwarder_started)
    return 0;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    fprintf(stderr, "[SYNC] socketpair failed: %s\n", strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "[SYNC] fork failed: %s\n", strerror(errno));
    close(sv[0]);
    close(sv[1]);
    return -1;
  }

  if (pid == 0) {
    /* ===== 子进程：exec eBPF forwarder ===== */
    close(sv[0]);

    extern int g_listen_fd;
    if (g_listen_fd >= 0)
      close(g_listen_fd);

    char uds_fd_str[16];
    snprintf(uds_fd_str, sizeof(uds_fd_str), "%d", sv[1]);

    const char *ifname = "ens33";
    const char *forwarder_path = "./build/ebpf/ebpf_forwarder_main";

    execl(forwarder_path, forwarder_path, uds_fd_str, ifname, NULL);

    fprintf(stderr, "[Forwarder] exec failed: %s\n", strerror(errno));
    _exit(EXIT_FAILURE);
  } else {
    /* ===== 父进程 ===== */
    close(sv[1]);
    g_forwarder_pid = pid;
    forwarder_set_uds_fd(sv[0]);
    g_forwarder_started = 1;

    /* 加载 XDP 程序到网卡 */
    ebpf_init_rlimit();
    int xdp_ret = ebpf_load_xdp("ens33");
    if (xdp_ret != 0) {
      /* forwarder 会在自己的进程中加载 XDP，这里失败不影响 */
    }
  }

  return 0;
}

/* ==================== Slave fd 管理 ==================== */
#define MAX_SLAVES 16
static int slave_fds[MAX_SLAVES];        // Slave fd 数组（固定大小）
static int slave_syncing[MAX_SLAVES];    // per-slave 同步状态
static int slave_count_val = 0;          // 当前 Slave 数量
static int slave_initialized = 0;        // 是否已初始化
static pthread_mutex_t slave_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 初始化 slave 数组 */
static void slave_ensure_init(void) {
  if (!slave_initialized) {
    for (int i = 0; i < MAX_SLAVES; i++) {
      slave_fds[i] = -1;
      slave_syncing[i] = 0;
    }
    slave_initialized = 1;
  }
}

/*
 * 查找 fd 在 slave_fds 数组中的索引，返回 -1 表示未找到
 */
static int slave_find_index(int fd) {
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slave_fds[i] == fd)
      return i;
  }
  return -1;
}

/*
 * 注册 Slave 连接
 * 返回：0 成功，-1 超过上限，-2 已存在
 */
int slave_register(int fd) {
  pthread_mutex_lock(&slave_mutex);
  slave_ensure_init();

  /* 检查是否已存在 */
  if (slave_find_index(fd) >= 0) {
    pthread_mutex_unlock(&slave_mutex);
    return -2;
  }

  /* 检查是否超过上限 */
  if (slave_count_val >= MAX_SLAVES) {
    pthread_mutex_unlock(&slave_mutex);
    return -1;
  }

  /* 找一个空位插入 */
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slave_fds[i] == -1) {
      slave_fds[i] = fd;
      slave_syncing[i] = 0;
      slave_count_val++;
      pthread_mutex_unlock(&slave_mutex);

      /* 第一个 Slave 注册时，按需启动 forwarder 和加载 XDP */
      if (!g_forwarder_started) {
        start_forwarder();
      }

      return 0;
    }
  }

  pthread_mutex_unlock(&slave_mutex);
  return -1; /* 不应该到这里 */
}

/*
 * 移除 Slave 连接
 */
void slave_unregister(int fd) {
  pthread_mutex_lock(&slave_mutex);
  int idx = slave_find_index(fd);
  if (idx >= 0) {
    slave_fds[idx] = -1;
    slave_syncing[idx] = 0;
    slave_count_val--;
    /* slave 注销 */
  }
  pthread_mutex_unlock(&slave_mutex);
}

/*
 * 检查 fd 是否为 Slave
 */
int is_slave_fd(int fd) {
  if (fd < 0)
    return 0;
  pthread_mutex_lock(&slave_mutex);
  int result = (slave_find_index(fd) >= 0) ? 1 : 0;
  pthread_mutex_unlock(&slave_mutex);
  return result;
}

/*
 * 获取当前 Slave 数量
 */
int slave_count(void) {
  pthread_mutex_lock(&slave_mutex);
  int cnt = slave_count_val;
  pthread_mutex_unlock(&slave_mutex);
  return cnt;
}

/*
 * 获取所有 Slave fd 的副本（调用者需要 free）
 * count：输出参数，返回 fd 数量
 */
int *slave_get_fds(int *count) {
  pthread_mutex_lock(&slave_mutex);
  slave_ensure_init();

  int *fds = (int *)malloc(slave_count_val * sizeof(int));
  if (!fds) {
    *count = 0;
    pthread_mutex_unlock(&slave_mutex);
    return NULL;
  }

  int idx = 0;
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slave_fds[i] >= 0) {
      fds[idx++] = slave_fds[i];
    }
  }
  *count = idx;
  pthread_mutex_unlock(&slave_mutex);
  return fds;
}

/*
 * 检查是否有任何 Slave 正在同步
 */
int is_any_slave_syncing(void) {
  pthread_mutex_lock(&slave_mutex);
  for (int i = 0; i < MAX_SLAVES; i++) {
    if (slave_fds[i] >= 0 && slave_syncing[i]) {
      pthread_mutex_unlock(&slave_mutex);
      return 1;
    }
  }
  pthread_mutex_unlock(&slave_mutex);
  return 0;
}

/*
 * 检查指定 Slave 是否正在同步
 */
int is_slave_syncing(int fd) {
  pthread_mutex_lock(&slave_mutex);
  int idx = slave_find_index(fd);
  int syncing = (idx >= 0) ? slave_syncing[idx] : 0;
  pthread_mutex_unlock(&slave_mutex);
  return syncing;
}

/*
 * 设置 per-slave 同步状态
 */
void slave_set_syncing(int fd, int val) {
  pthread_mutex_lock(&slave_mutex);
  int idx = slave_find_index(fd);
  if (idx >= 0) {
    slave_syncing[idx] = val;
  }
  pthread_mutex_unlock(&slave_mutex);
}

/* ==================== Unix Domain Socket 通信 ==================== */
int g_uds_fd = -1;  /* 主进程端的 UDS fd */

/*
 * 设置 UDS fd（由 main.c 创建 socketpair 后调用）
 */
void forwarder_set_uds_fd(int fd) {
  g_uds_fd = fd;
}

/*
 * 通过 UDS 向 forwarder 发送 REMOVE_SLAVE 命令
 */
void forwarder_send_remove_slave(int slave_fd) {
  if (g_uds_fd < 0)
    return;

  char buf[32];
  snprintf(buf, sizeof(buf), "REM %d", slave_fd);
  send(g_uds_fd, buf, strlen(buf), MSG_NOSIGNAL);
}

/*
 * 通过 UDS 向 forwarder 发送 ADD_SLAVE 命令（附带 fd）
 * 返回：0 成功，-1 失败
 */
int forwarder_send_add_slave(int slave_fd) {
  if (g_uds_fd < 0) {
    return -1;
  }

  char buf[] = "ADD";
  struct msghdr msg = {0};
  struct iovec iov = {buf, strlen(buf)};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  /* 使用 SCM_RIGHTS 传递 fd */
  char ctrl_buf[CMSG_SPACE(sizeof(int))];
  memset(ctrl_buf, 0, sizeof(ctrl_buf));
  msg.msg_control = ctrl_buf;
  msg.msg_controllen = sizeof(ctrl_buf);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  *(int *)CMSG_DATA(cmsg) = slave_fd;

  ssize_t ret = sendmsg(g_uds_fd, &msg, 0);
  if (ret < 0) {
    fprintf(stderr, "[SYNC] Failed to send ADD_SLAVE via UDS: %s\n",
            strerror(errno));
    return -1;
  }

  /* slave fd 已发送给 forwarder */
  return 0;
}

/* ==================== SYNC 同步线程 ==================== */

/*
 * SYNC 后处理线程参数
 */
typedef struct {
  int slave_fd;
} sync_thread_arg_t;

/*
 * SYNC 后处理线程：RDMA 传输
 * 在独立线程中执行，避免阻塞 Reactor event loop
 * 完成后将 slave fd 添加到 eBPF forwarder
 */
static void *sync_thread_func(void *arg) {
  sync_thread_arg_t *st_arg = (sync_thread_arg_t *)arg;
  int slave_fd = st_arg->slave_fd;
  free(st_arg);

  /* sync 线程启动 */

  /* 1. 读取日志文件，通过 RDMA 发送给 Slave */
  const void *sync_data = NULL;
  size_t sync_size = 0;

  FILE *fp = fopen(g_config.log_path, "rb");
  if (fp) {
    fseek(fp, 0, SEEK_END);
    sync_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sync_size > 0) {
      char *file_data = (char *)malloc(sync_size);
      if (file_data) {
        size_t read_ret = fread(file_data, 1, sync_size, fp);
        if (read_ret == sync_size) {
          sync_data = file_data;
        } else {
          free(file_data);
          sync_data = NULL;
          sync_size = 0;
        }
      }
    }
    fclose(fp);
  }

  if (sync_data && sync_size > 0) {
    /* RDMA 传输开始 */

    extern int rdma_master_send_mem(const void *data, size_t size, int port);
    int rdma_ret =
        rdma_master_send_mem(sync_data, sync_size, g_config.slave_rdma_port);
    free((void *)sync_data);
    if (rdma_ret != 0) {
      fprintf(stderr, "[SYNC] RDMA data transfer failed for slave fd=%d\n",
              slave_fd);
      slave_set_syncing(slave_fd, 0);
      slave_unregister(slave_fd);
      return NULL;
    }
    /* RDMA 传输完成 */
  } else {
    slave_set_syncing(slave_fd, 0);
    slave_unregister(slave_fd);
    return NULL;
  }

  /* 2. RDMA 完成，通过 UDS 将 slave fd 传递给 eBPF forwarder */
  int add_ret = forwarder_send_add_slave(slave_fd);
  if (add_ret != 0) {
    fprintf(stderr, "[SYNC] Failed to send slave fd=%d to forwarder\n", slave_fd);
    slave_set_syncing(slave_fd, 0);
    slave_unregister(slave_fd);
    return NULL;
  }

  /* 3. 释放同步锁，恢复写操作 */
  slave_set_syncing(slave_fd, 0);

  return NULL;
}

/*
 * 启动 Slave 同步（在独立线程中执行 RDMA 传输）
 * 供 kvstore.c 的 SYNC case 调用
 */
void slave_start_sync(int slave_fd) {
  sync_thread_arg_t *st_arg = (sync_thread_arg_t *)malloc(sizeof(sync_thread_arg_t));
  st_arg->slave_fd = slave_fd;
  pthread_t sync_thread;
  pthread_create(&sync_thread, NULL, &sync_thread_func, st_arg);
  pthread_detach(sync_thread);
}

/*
 * 信号处理：通知 forwarder 退出并等待
 */
void slave_manager_shutdown(void) {
  /* 通知 forwarder 子进程退出 */
  if (g_uds_fd >= 0) {
    send(g_uds_fd, "QUIT", 4, MSG_NOSIGNAL);
  }

  /* 等待 forwarder 子进程退出 */
  if (g_forwarder_pid > 0) {
    int status;
    for (int i = 0; i < 50; i++) {
      pid_t ret = waitpid(g_forwarder_pid, &status, WNOHANG);
      if (ret > 0 || ret < 0)
        break;
      usleep(100000); /* 100ms */
    }
  }

  /* 关闭 UDS */
  if (g_uds_fd >= 0) {
    close(g_uds_fd);
    g_uds_fd = -1;
  }
}
