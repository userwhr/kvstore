/*
 * ============================================================================
 * eBPF 增量同步转发程序（用户态守护进程）
 *
 * 功能：
 *   1. 加载 ebpf_inc_sync.o 到指定网卡
 *   2. 创建 perf buffer，接收 eBPF 上报的写命令
 *   3. 通过全局 g_slave_fd 将写命令转发给 Slave
 *
 * 本文件被编译为库，由主程序调用，不再作为独立可执行文件。
 * ============================================================================
 */
#include "ebpf_forwarder.h"
#include "kvstore.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_link.h>
#include <linux/types.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

/* BPF 对象全局句柄，用于卸载 */
static struct bpf_object *g_obj = NULL;
static int g_ifindex = -1;

/* perf buffer 全局句柄 */
static struct perf_buffer *g_pb = NULL;

/* 转发计数器 */
int g_forward_count = 0;

/*
 * 非阻塞 drain slave 的 TCP 读缓冲区
 * 返回值：0=正常, -1=slave 断连
 */
static int drain_slave_recv(void) {
  if (g_slave_fd < 0)
    return -1;

  char tmp[128];
  while (1) {
    int ret = recv(g_slave_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (ret > 0)
      continue; /* 读到了，继续 drain */
    if (ret == 0) {
      fprintf(stderr, "[Forwarder] Slave disconnected\n");
      return -1;
    }
    /* ret < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0; /* 缓冲区已空 */
    fprintf(stderr, "[Forwarder] Slave recv error: %s\n", strerror(errno));
    return -1;
  }
}

static void perf_lost_handler(void *ctx, int cpu, __u64 lost_cnt) {
  fprintf(stderr, "[Forwarder] *** Lost %llu events on CPU %d ***\n",
          (unsigned long long)lost_cnt, cpu);
}

/* RESP 协议缓冲，用于处理 TCP 分段 */
#define RESP_BUF_SIZE (1024 * 1024) /* 1MB */
static char resp_buf[RESP_BUF_SIZE];
int resp_buf_len = 0;
/* 写指令列表（与 eBPF 内核态保持一致） */
static const char *write_cmds[] = {
    "set",  "del",  "mod",  "rset",  "rdel",  "rmod",
    "hset", "hdel", "hmod", "skset", "skdel", "skmod",
};
#define WRITE_CMD_COUNT (sizeof(write_cmds) / sizeof(write_cmds[0]))
/*
 * 检查 RESP 缓冲区中是否有完整的命令
 * 返回完整命令的长度，0 表示不完整
 *
 * RESP 格式：*N\r\n$len1\r\narg1\r\n$len2\r\narg2\r\n...
 */
static int check_resp_complete(const char *buf, int len) {
  const char *p = buf;
  const char *end = buf + len;

  /* 解析 *N */
  if (p >= end || *p != '*')
    return 0;
  p++;
  int num_args = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    num_args = num_args * 10 + (*p - '0');
    p++;
  }
  if (p + 1 >= end || *p != '\r' || *(p + 1) != '\n')
    return 0;
  p += 2;

  /* 逐个解析参数 */
  for (int i = 0; i < num_args; i++) {
    /* 解析 $len */
    if (p >= end || *p != '$')
      return 0;
    p++;
    int arg_len = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      arg_len = arg_len * 10 + (*p - '0');
      p++;
    }
    if (p + 1 >= end || *p != '\r' || *(p + 1) != '\n')
      return 0;
    p += 2;

    /* 跳过参数数据 + \r\n */
    p += arg_len;
    if (p + 1 >= end)
      return 0; /* 数据还没收完 */
    if (*p != '\r' || *(p + 1) != '\n')
      return 0;
    p += 2;
  }

  return p - buf; /* 返回完整命令的长度 */
}
/*
 * 判断 RESP 命令是否为写指令
 * 从已解析的完整 RESP 数据中提取第一个参数（命令名）进行匹配
 */
static int is_write_command(const char *buf, int len) {
  const char *p = buf;
  const char *end = buf + len;

  /* 跳过 *N\r\n */
  if (p >= end || *p != '*')
    return 0;
  p++;
  while (p < end && *p >= '0' && *p <= '9')
    p++;
  if (p + 1 >= end || *p != '\r')
    return 0;
  p += 2;

  /* 解析 $cmd_len\r\n */
  if (p >= end || *p != '$')
    return 0;
  p++;
  int cmd_len = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    cmd_len = cmd_len * 10 + (*p - '0');
    p++;
  }
  if (p + 1 >= end || *p != '\r')
    return 0;
  p += 2;

  /* 提取命令名并转小写比较 */
  if (p + cmd_len > end)
    return 0;
  char cmd_name[16] = {0};
  if (cmd_len >= (int)sizeof(cmd_name))
    return 0;
  for (int i = 0; i < cmd_len; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z')
      c += 32;
    cmd_name[i] = c;
  }

  for (int i = 0; i < (int)WRITE_CMD_COUNT; i++) {
    if (strcmp(cmd_name, write_cmds[i]) == 0)
      return 1;
  }
  return 0;
}

/*
 * 检查 RESP 缓冲区中是否有完整的命令
 * 返回完整命令的长度，0 表示不完整
 *
 * RESP 格式：*N\r\n$len1\r\narg1\r\n$len2\r\narg2\r\n...
 */

/*
 * perf_event 回调函数
 * 当 eBPF 程序通过 bpf_perf_event_output 上报数据时，此函数被调用
 *
 * TCP 可能分段发送，需要缓冲后按 RESP 协议切分完整命令再转发。
 * 使用全局 g_slave_fd（在 reactor.c 中定义）将写命令转发给 Slave。
 */
void perf_event_handler(void *ctx, int cpu, void *data, __u32 data_size) {
  if (data_size <= 0)
    return;

  // 1. 缓冲区防溢出
  if (resp_buf_len + data_size > RESP_BUF_SIZE) {
    fprintf(stderr, "[Forwarder] RESP buf overflow, reset buf\n");
    resp_buf_len = 0;
    return;
  }

  // 2. 剔除尾部 0 填充
  int actual_len = data_size;
  const char *p_data = (const char *)data;
  while (actual_len > 0 && p_data[actual_len - 1] == '\0')
    actual_len--;
  if (actual_len == 0)
    return;

  // 3. 追加到全局流式缓冲区
  memcpy(resp_buf + resp_buf_len, p_data, actual_len);
  resp_buf_len += actual_len;
  // if (g_forward_count >= 76325) {
  //   print_hex("【BPF 收到原始数据】", data, actual_len);
  //   printf("当前缓冲区长度 resp_buf_len = %d\n", resp_buf_len);
  // }
  // if (resp_buf_len > 45 || resp_buf[4] == '*') {
  //   printf("从resp_buf_len->%d,resp_buf->%s,data->%s\n", resp_buf_len, resp_buf,
  //          data);
  //   exit(1);
  // }

  // ===================== 核心：循环解析，处理半包/粘包 =====================
  while (resp_buf_len > 0) {
    // 缓冲区空了，退出
    if (resp_buf_len <= 0)
      break;

    if (resp_buf[0] != '*') {
      int skip = 0;
      // 找下一个命令开头 *
      while (skip < resp_buf_len && resp_buf[skip] != '*')
        skip++;

      if (skip < resp_buf_len) {
        // 找到 *，前面的垃圾丢掉
        resp_buf_len -= skip;
        if (resp_buf_len > 0) {
          memmove(resp_buf, resp_buf + skip, resp_buf_len);
        }
        continue;
      } else {
        // ==============================
        // ✅ 关键修复：没找到 *，说明是半包
        // ✅ 不丢数据，直接退出等下一段
        // ==============================
        break;
      }
    }
    // 检查是否能解析出一条完整 RESP
    int cmd_len = check_resp_complete(resp_buf, resp_buf_len);
    if (cmd_len <= 0) {
      // ✅ 以 * 开头，但解析失败 = 错误命令
      // ✅ 寻找下一个 *，如果找到，直接丢弃这段错误数据
      int next_star = 1;
      while (next_star < resp_buf_len && resp_buf[next_star] != '*')
        next_star++;

      if (next_star < resp_buf_len) {
        // 找到下一个新命令，删除当前错误的 * 片段
        resp_buf_len -= next_star;
        if (resp_buf_len > 0)
          memmove(resp_buf, resp_buf + next_star, resp_buf_len);
        continue;
      } else {
        // 没找到下一个命令，说明是半包，等待
        break;
      }
    }

    // 是读指令，直接丢弃
    if (!is_write_command(resp_buf, cmd_len)) {
      resp_buf_len -= cmd_len;
      if (resp_buf_len > 0) {
        memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
      }
      continue;
    }

    // 转发写指令给slave
    int send_ret =
        send(g_slave_fd, resp_buf, cmd_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (send_ret <= 0) {
      if (send_ret < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        fprintf(stderr, "[Forwarder] Slave connection lost\n");
        g_slave_fd = -1;
      }
      break;
    }

    g_forward_count++;

    // 清空slave接收缓冲区，防止窗口满
    if (drain_slave_recv() < 0) {
      g_slave_fd = -1;
      break;
    }

    // 滑动缓冲区：移除已处理的完整帧
    if (resp_buf_len > 0) {
      memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
    }
    resp_buf_len -= cmd_len;
    // if (g_forward_count >= 76325) {
    //   printf("cmd_len->%d\n", cmd_len);
    //   printf("解析完了第%d个指令,解析后的resp_buf->%s,resp_buf为->%d\n",
    //          g_forward_count, resp_buf, resp_buf_len);
    // }
  }
}

static int libbpf_print_cb(enum libbpf_print_level level, const char *fmt,
                           va_list args) {
  if (level == LIBBPF_WARN)
    return vfprintf(stderr, fmt, args);
  return 0;
}

/*
 * 检查 XDP 程序是否已加载
 */
int ebpf_is_xdp_loaded(void) { return g_obj != NULL; }

/*
 * 解除 RLIMIT_MEMLOCK 限制（BPF 加载必需）
 */
void ebpf_init_rlimit(void) {
  struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
  setrlimit(RLIMIT_MEMLOCK, &rlim);
}

/*
 * 加载 XDP 程序到指定网卡
 */
int ebpf_load_xdp(const char *ifname) {
  struct bpf_program *prog = NULL;
  int ifindex;
  int err;

  if (g_obj) {
    fprintf(stderr, "[Forwarder] XDP already loaded, unloading first\n");
    ebpf_unload_xdp();
  }

  /* 获取网卡索引 */
  ifindex = if_nametoindex(ifname);
  if (ifindex == 0) {
    fprintf(stderr, "[Forwarder] Invalid interface name: %s\n", ifname);
    return -1;
  }
  g_ifindex = ifindex;
  // printf("[Forwarder] Interface: %s (ifindex=%d)\n", ifname, ifindex);

  /* 设置 libbpf 日志级别（只显示错误） */
  libbpf_set_print(libbpf_print_cb);

  /* 加载 BPF 对象文件 */
  const char *bpf_obj_paths[] = {
      "./build/ebpf/ebpf_inc_sync.o",
      "/home/wanghuiran/KVSTORE/project/kvstore/build/ebpf/ebpf_inc_sync.o",
  };

  struct bpf_object *obj = NULL;
  for (int i = 0; i < 2; i++) {
    obj = bpf_object__open_file(bpf_obj_paths[i], NULL);
    if (!libbpf_get_error(obj)) {
      // printf("[Forwarder] Loaded BPF object: %s\n", bpf_obj_paths[i]);
      break;
    }
    obj = NULL;
  }

  if (!obj) {
    fprintf(stderr, "[Forwarder] Cannot open ebpf_inc_sync.o\n");
    return -1;
  }
  if (libbpf_get_error(obj)) {
    fprintf(stderr, "[Forwarder] Cannot open ebpf_inc_sync.o\n");
    return -1;
  }

  /* 加载 BPF 程序到内核 */
  err = bpf_object__load(obj);
  if (err) {
    fprintf(stderr, "[Forwarder] BPF load failed: %s\n", strerror(-err));
    if (err == -EPERM) {
      fprintf(stderr, "[Forwarder] Hint: eBPF requires root, run with sudo\n");
    }
    bpf_object__close(obj);
    return -1;
  }
  // printf("[Forwarder] BPF program loaded\n");

  /* 找到 XDP 程序 */
  prog = bpf_object__find_program_by_name(obj, "ebpf_inc_sync_func");
  if (!prog) {
    fprintf(stderr, "[Forwarder] Cannot find ebpf_inc_sync_func\n");
    bpf_object__close(obj);
    return -1;
  }

  /* 找到 cmd_events map 并设置 perf buffer */
  struct bpf_map *events_map = bpf_object__find_map_by_name(obj, "cmd_events");
  if (!events_map) {
    fprintf(stderr, "[Forwarder] Cannot find cmd_events map\n");
    bpf_object__close(obj);
    return -1;
  }

  int map_fd = bpf_map__fd(events_map);
  g_pb = perf_buffer__new(map_fd, 1024, perf_event_handler, perf_lost_handler,
                          NULL, NULL);
  if (libbpf_get_error(g_pb)) {
    fprintf(stderr, "[Forwarder] Failed to create perf buffer\n");
    g_pb = NULL;
    bpf_object__close(obj);
    return -1;
  }
  // printf("[Forwarder] Perf buffer created\n");

  /* 将 XDP 程序附加到网卡 */
  int prog_fd = bpf_program__fd(prog);
  __u32 flags = XDP_FLAGS_SKB_MODE;
  err = bpf_xdp_attach(ifindex, prog_fd, flags, NULL);
  if (err) {
    fprintf(stderr, "[Forwarder] XDP attach failed: %s\n", strerror(errno));
    perf_buffer__free(g_pb);
    g_pb = NULL;
    bpf_object__close(obj);
    return -1;
  }
  // printf("[Forwarder] XDP attached to %s\n", ifname);

  g_obj = obj;
  return 0;
}

/*
 * 卸载 XDP 程序
 */
void ebpf_unload_xdp(void) {
  if (g_ifindex >= 0) {
    bpf_xdp_detach(g_ifindex, 0, NULL);
    // printf("[Forwarder] XDP detached\n");
    g_ifindex = -1;
  }

  if (g_pb) {
    perf_buffer__free(g_pb);
    g_pb = NULL;
    // printf("[Forwarder] Perf buffer freed\n");
  }

  if (g_obj) {
    bpf_object__close(g_obj);
    g_obj = NULL;
    // printf("[Forwarder] BPF object closed\n");
  }
}

/* 把 resp_buf 里积压的命令尽量发出去 */
static void flush_resp_buf(void) {
  while (resp_buf_len > 0 && g_slave_fd >= 0) {
    /* 跳过非 RESP 数据 */
    if (resp_buf[0] != '*') {
      int skip = 0;
      while (skip < resp_buf_len && resp_buf[skip] != '*')
        skip++;
      resp_buf_len -= skip;
      if (resp_buf_len > 0)
        memmove(resp_buf, resp_buf + skip, resp_buf_len);
      if (resp_buf_len == 0)
        break;
      continue;
    }

    int cmd_len = check_resp_complete(resp_buf, resp_buf_len);
    if (cmd_len <= 0)
      break;

    if (!is_write_command(resp_buf, cmd_len)) {
      resp_buf_len -= cmd_len;
      if (resp_buf_len > 0)
        memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
      continue;
    }

    int send_ret =
        send(g_slave_fd, resp_buf, cmd_len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (send_ret <= 0) {
      if (send_ret < 0 && (errno == EPIPE || errno == ECONNRESET)) {
        fprintf(stderr, "[Forwarder] Slave connection lost (flush)\n");
        g_slave_fd = -1;
      }
      break;
    }
    g_forward_count++;

    if (drain_slave_recv() < 0) {
      g_slave_fd = -1;
      break;
    }

    resp_buf_len -= cmd_len;
    if (resp_buf_len > 0)
      memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
  }
}

int ebpf_forwarder_run(int slave_fd) {
  int err;

  if (!g_obj) {
    fprintf(stderr, "[Forwarder] XDP not loaded, call ebpf_load_xdp first\n");
    return -1;
  }

  // 用传入的slave_fd，覆盖全局，保证有效性
  g_slave_fd = slave_fd;
  int flags = fcntl(g_slave_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(g_slave_fd, F_SETFL, flags | O_NONBLOCK);

  int cmd_count_fd = -1;
  struct bpf_map *count_map = bpf_object__find_map_by_name(g_obj, "cmd_count");
  if (count_map)
    cmd_count_fd = bpf_map__fd(count_map);

  extern volatile int g_shutdown_requested;

  while (!g_shutdown_requested && g_slave_fd != -1) {
    if (g_slave_fd < 0) {
      fprintf(stderr, "[Forwarder] g_slave_fd invalid, exiting\n");
      break;
    }

    /* drain slave 的响应，防止其 TCP 发送缓冲区满导致 reactor 阻塞 */
    if (drain_slave_recv() < 0) {
      fprintf(stderr, "[Forwarder] Slave disconnected\n");
      break;
    }

    /* 把缓冲区里的数据尽量发出去 */
    flush_resp_buf();

    /* 反压：缓冲区超过一半时，暂停读取新事件 */
    if (resp_buf_len > RESP_BUF_SIZE / 2) {
      usleep(1000);
      continue;
    }

    /* 轮询eBPF事件，10ms超时 */
    err = perf_buffer__poll(g_pb, 10);
    if (err < 0 && err != -EINTR) {
      fprintf(stderr, "[Forwarder] perf_buffer__poll error: %d (%s)\n", err,
              strerror(-err));
      break;
    }
  }

  return 0;
}