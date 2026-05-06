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

static void perf_lost_handler(void *ctx, int cpu, __u64 lost_cnt) {
  fprintf(stderr, "[Forwarder] *** Lost %llu events on CPU %d ***\n",
          (unsigned long long)lost_cnt, cpu);
}

/* RESP 协议缓冲，用于处理 TCP 分段 */
#define RESP_BUF_SIZE 65536
static char resp_buf[RESP_BUF_SIZE];
static int resp_buf_len = 0;

/* 写指令列表（与 eBPF 内核态保持一致） */
static const char *write_cmds[] = {
    "set", "del", "mod", "rset", "rdel", "rmod",
    "hset", "hdel", "hmod", "skset", "skdel", "skmod",
};
#define WRITE_CMD_COUNT (sizeof(write_cmds) / sizeof(write_cmds[0]))

/*
 * 判断 RESP 命令是否为写指令
 * 从已解析的完整 RESP 数据中提取第一个参数（命令名）进行匹配
 */
static int is_write_command(const char *buf, int len) {
  const char *p = buf;
  const char *end = buf + len;

  /* 跳过 *N\r\n */
  if (p >= end || *p != '*') return 0;
  p++;
  while (p < end && *p >= '0' && *p <= '9') p++;
  if (p + 1 >= end || *p != '\r') return 0;
  p += 2;

  /* 解析 $cmd_len\r\n */
  if (p >= end || *p != '$') return 0;
  p++;
  int cmd_len = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    cmd_len = cmd_len * 10 + (*p - '0');
    p++;
  }
  if (p + 1 >= end || *p != '\r') return 0;
  p += 2;

  /* 提取命令名并转小写比较 */
  if (p + cmd_len > end) return 0;
  char cmd_name[16] = {0};
  if (cmd_len >= (int)sizeof(cmd_name)) return 0;
  for (int i = 0; i < cmd_len; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c += 32;
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
 * perf_event 回调函数
 * 当 eBPF 程序通过 bpf_perf_event_output 上报数据时，此函数被调用
 *
 * TCP 可能分段发送，需要缓冲后按 RESP 协议切分完整命令再转发。
 * 使用全局 g_slave_fd（在 reactor.c 中定义）将写命令转发给 Slave。
 */
void perf_event_handler(void *ctx, int cpu, void *data, __u32 data_size) {
  if (data_size <= 0)
    return;

  /* 追加到缓冲区 */
  if (resp_buf_len + data_size > RESP_BUF_SIZE) {
    fprintf(stderr, "[Forwarder] RESP buffer overflow, dropping %d bytes\n",
            data_size);
    resp_buf_len = 0;
    return;
  }
  // printf("[Forwarder] Received %d bytes, buf before: %d bytes\n", data_size, resp_buf_len);
  /* 打印收到的数据前 40 字节 */
  // printf("[Forwarder] Data hex: ");
  // for (int i = 0; i < (int)data_size && i < 40; i++)
  //   printf("%02x ", ((unsigned char *)data)[i]);
  // printf("\n");

  /* 使用 strlen 计算实际长度，排除末尾的 0x00 以太网填充 */
  int actual_len = strlen((const char *)data);
  if (actual_len == 0)
    return;
  memcpy(resp_buf + resp_buf_len, data, actual_len);
  resp_buf_len += actual_len;

  /* 尝试从缓冲区中提取完整命令 */
  while (resp_buf_len > 0) {
    // printf("[Forwarder] resp_buf_len=%d, first byte=0x%02x\n",
    //        resp_buf_len, (unsigned char)resp_buf[0]);
    int cmd_len = check_resp_complete(resp_buf, resp_buf_len);
    if (cmd_len <= 0) {
      // printf("[Forwarder] Incomplete RESP, waiting for more data (buf_len=%d)\n",
      //        resp_buf_len);
      break; /* 不完整，等待更多数据 */
    }

    /* 判断是否为写指令，不是则丢弃 */
    if (!is_write_command(resp_buf, cmd_len)) {
      // printf("[Forwarder] Non-write command, discarding (%d bytes)\n", cmd_len);
      resp_buf_len -= cmd_len;
      if (resp_buf_len > 0)
        memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
      continue;
    } 

    /* 写指令，转发给 slave */
    // printf("[Forwarder] Write command, forwarding (%d bytes)\n", cmd_len);
    int send_ret = send(g_slave_fd, resp_buf, cmd_len, MSG_NOSIGNAL);
    if (send_ret <= 0) {
      fprintf(stderr, "[Forwarder] Send to slave failed: %s\n", strerror(errno));
      break;
    }
    // printf("[Forwarder] Forwarded %d bytes to slave\n", send_ret);

    /* 从缓冲区移除已发送的数据 */
    resp_buf_len -= cmd_len;
    if (resp_buf_len > 0)
      memmove(resp_buf, resp_buf + cmd_len, resp_buf_len);
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
int ebpf_is_xdp_loaded(void) {
  return g_obj != NULL;
}

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
  g_pb = perf_buffer__new(map_fd, 64, perf_event_handler, perf_lost_handler,
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

/*
 * 进入 eBPF 转发主循环
 *
 * 使用全局 g_slave_fd（在 reactor.c 中定义）将 eBPF 捕获的写命令
 * 转发给 Slave。g_slave_fd 由 reactor 主进程在收到 SYNC 时设置。
 */
int ebpf_forwarder_run(int slave_fd) {
  int err;

  if (!g_obj) {
    fprintf(stderr, "[Forwarder] XDP not loaded, call ebpf_load_xdp first\n");
    return -1;
  }

  /* 设置 g_slave_fd 为非阻塞，防止 recv/send 阻塞 forwarder 进程 */
  int flags = fcntl(g_slave_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(g_slave_fd, F_SETFL, flags | O_NONBLOCK);

  // printf("[Forwarder] Forwarding write commands to Slave (fd=%d)...\n",
  //        slave_fd);

  /* 诊断：查找 cmd_count map 用于验证 eBPF 是否在捕获包 */
  int cmd_count_fd = -1;
  struct bpf_map *count_map = bpf_object__find_map_by_name(g_obj, "cmd_count");
  if (count_map)
    cmd_count_fd = bpf_map__fd(count_map);

  /* 导入外部退出标志 */
  extern volatile int g_shutdown_requested;

  /* ===== 主循环 ===== */
  while (!g_shutdown_requested) {
    /* 检查 g_slave_fd 是否有效 */
    if (g_slave_fd < 0) {
      fprintf(stderr, "[Forwarder] g_slave_fd invalid, exiting\n");
      break;
    }

    err = perf_buffer__poll(g_pb, 2000);
    /* 诊断：读取 eBPF cmd_count，验证内核态是否在捕获包 */
    if (cmd_count_fd >= 0) {
      __u32 key = 0;
      __u64 count = 0;
      bpf_map_lookup_elem(cmd_count_fd, &key, &count);
      // printf("[Forwarder] eBPF cmd_count = %llu\n", (unsigned long
      // long)count);
    }
    if (err < 0 && err != -EINTR) {
      fprintf(stderr, "[Forwarder] perf_buffer__poll error: %d (%s)\n", err,
              strerror(-err));
      break;
    }
  }

  // printf("\n[Forwarder] Exiting...\n");
  return 0;
}
