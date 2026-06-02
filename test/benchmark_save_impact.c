/*
 * benchmark_save_impact - 测试 save 命令对 QPS 的影响
 *
 * 用法: ./benchmark_save_impact -t <命令> -n <请求数> -s <save间隔> -H <主机>
 * -p <端口> -b <绑定地址>
 *
 * 功能：每发送 n 个指定命令后，发送一个 save 命令，测量整体 QPS
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RECV_BUF_SIZE 262144 /* 256KB */
#define CMD_BUF_SIZE 256

/* ── 全局配置 ── */
static const char *g_host = "127.0.0.1";
static const char *g_bind = NULL;
static int g_port = 6379;
static int g_total = 100000;       /* -n 总请求数 */
static int g_save_interval = 1000; /* -s 每隔多少个命令发送一次 save */

/* ── 命令类型 ── */
typedef enum {
  CMD_SET,
  CMD_GET,
  CMD_DEL,
  CMD_MOD,
  CMD_EXIST,
  CMD_RSET,
  CMD_RGET,
  CMD_RDEL,
  CMD_RMOD,
  CMD_REXIST,
  CMD_HSET,
  CMD_HGET,
  CMD_HDEL,
  CMD_HMOD,
  CMD_HEXIST,
  CMD_SKSET,
  CMD_SKGET,
  CMD_SKDEL,
  CMD_SKMOD,
  CMD_SKEXIST,
  CMD_ECHO,
  CMD_CMD_COUNT
} cmd_type_t;

static const char *cmd_names[] = {
    "set",    "get",   "del",    "mod",   "exist", "rset",    "rget",
    "rdel",   "rmod",  "rexist", "hset",  "hget",  "hdel",    "hmod",
    "hexist", "skset", "skget",  "skdel", "skmod", "skexist", "echo",
};

/* ── 统计信息 ── */
static long long g_cmd_sent = 0;  /* 已发送的普通命令数 */
static long long g_save_sent = 0; /* 已发送的 save 命令数 */
static long long g_cmd_ok = 0;    /* 普通命令成功数 */
static long long g_save_ok = 0;   /* save 成功数 */
static long long g_cmd_err = 0;   /* 普通命令失败数 */
static long long g_save_err = 0;  /* save 失败数 */
static struct timespec g_start, g_end;

/* ── 延迟统计 ── */
static double g_save_latency_sum = 0.0;
static double g_save_latency_max = 0.0;
static double g_save_latency_min = 1e9;

/* ── 判断命令是否需要 value 参数 ── */
static int cmd_needs_val(const char *cmd) {
  /* 只有写命令需要 val，读/删除命令不需要 */
  return strcmp(cmd, "set") == 0 || strcmp(cmd, "rset") == 0 ||
         strcmp(cmd, "hset") == 0 || strcmp(cmd, "skset") == 0 ||
         strcmp(cmd, "mod") == 0 || strcmp(cmd, "rmod") == 0 ||
         strcmp(cmd, "hmod") == 0 || strcmp(cmd, "skmod") == 0;
}

/* ── RESP 编码 ── */
static int encode_resp(char *buf, int bufsz, const char *cmd, const char *key,
                       const char *val) {
  if (val && cmd_needs_val(cmd))
    return snprintf(buf, bufsz,
                    "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                    strlen(cmd), cmd, strlen(key), key, strlen(val), val);
  else
    return snprintf(buf, bufsz, "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                    strlen(cmd), cmd, strlen(key), key);
}

/* ── 创建 TCP 连接 ── */
static int create_conn(const char *host, int port, const char *bind_addr) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };
  inet_pton(AF_INET, host, &addr.sin_addr);

  if (bind_addr) {
    struct sockaddr_in src = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
    };
    inet_pton(AF_INET, bind_addr, &src.sin_addr);
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
      close(fd);
      return -1;
    }
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* ── 发送数据 ── */
static int send_all(int fd, const char *buf, int len) {
  int sent = 0;
  while (sent < len) {
    int n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0)
      return -1;
    sent += n;
  }
  return 0;
}

/* ── 读取 n 个字节 ── */
static int recv_nbytes(int fd, char *buf, int n) {
  int total = 0;
  while (total < n) {
    int r = recv(fd, buf + total, n - total, 0);
    if (r <= 0)
      return -1;
    total += r;
  }
  return total;
}

/* ── 接收完整 RESP 响应 ── */
static int recv_response(int fd, char *buf, int bufsz) {
  /* 先读第一行: +OK / -ERR / $len / :int */
  int total = 0;
  while (total < bufsz - 1) {
    int n = recv(fd, buf + total, 1, 0);
    if (n <= 0)
      return -1;
    total++;
    if (total >= 2 && buf[total - 2] == '\r' && buf[total - 1] == '\n') {
      break;
    }
  }

  /* bulk string: $<len>\r\n<data>\r\n */
  if (buf[0] == '$') {
    int slen = atoi(buf + 1);
    if (slen < 0)
      return total; /* $-1 null */
    /* 读 slen 字节数据 + \r\n */
    if (recv_nbytes(fd, buf + total, slen + 2) < 0)
      return -1;
    total += slen + 2;
  }

  buf[total] = '\0';
  return total;
}

/* ── 发送并等待一个命令的响应 ── */
static int send_command(int fd, const char *cmd, const char *key,
                        const char *val, int is_save) {
  char buf[CMD_BUF_SIZE];
  int len;

  if (is_save) {
    len = snprintf(buf, sizeof(buf), "*1\r\n$4\r\nsave\r\n");
  } else {
    len = encode_resp(buf, sizeof(buf), cmd, key, val);
  }

  if (send_all(fd, buf, len) < 0)
    return -1;

  /* 接收响应 */
  char resp[256];
  if (recv_response(fd, resp, sizeof(resp)) < 0)
    return -1;

  /* 检查响应 */
  if (resp[0] == '+')
    return 1; /* +OK */
  if (resp[0] == '-')
    return 0; /* -ERR */
  if (resp[0] == '$')
    return 1; /* $len data */
  return 0;
}

/* ── 执行测试 ── */
static void run_test(cmd_type_t type) {
  int fd = create_conn(g_host, g_port, g_bind);
  if (fd < 0) {
    fprintf(stderr, "连接失败: %s\n", strerror(errno));
    return;
  }

  printf("目标服务器: %s:%d\n", g_host, g_port);
  printf("总请求数:   %d\n", g_total);
  printf("Save 间隔:  每 %d 个命令发送一次 save\n", g_save_interval);
  printf("测试命令:   %s\n", cmd_names[type]);
  printf("\n开始测试...\n");

  clock_gettime(CLOCK_MONOTONIC, &g_start);

  int cmd_count = 0;
  int save_count = 0;
  struct timespec cmd_start, cmd_end;

  for (int i = 0; i < g_total; i++) {
    /* 构造命令参数 */
    char key[64], val[64];
    snprintf(key, sizeof(key), "key%d", i);
    snprintf(val, sizeof(val), "%d", i);

    /* 发送普通命令 */
    clock_gettime(CLOCK_MONOTONIC, &cmd_start);
    int ret = send_command(fd, cmd_names[type], key, val, 0);
    clock_gettime(CLOCK_MONOTONIC, &cmd_end);

    if (ret < 0) {
      fprintf(stderr, "\n发送命令失败: %s\n", strerror(errno));
      break;
    }

    g_cmd_sent++;
    if (ret > 0)
      g_cmd_ok++;
    else
      g_cmd_err++;

    cmd_count++;

    /* 检查是否需要发送 save */
    if (g_save_interval > 0 && cmd_count >= g_save_interval) {
      struct timespec save_start, save_end;
      clock_gettime(CLOCK_MONOTONIC, &save_start);

      int save_ret = send_command(fd, NULL, NULL, NULL, 1);

      clock_gettime(CLOCK_MONOTONIC, &save_end);

      if (save_ret < 0) {
        fprintf(stderr, "\n发送 save 命令失败: %s\n", strerror(errno));
        break;
      }

      double save_latency = (save_end.tv_sec - save_start.tv_sec) * 1000.0 +
                            (save_end.tv_nsec - save_start.tv_nsec) / 1e6;

      g_save_sent++;
      if (save_ret > 0)
        g_save_ok++;
      else
        g_save_err++;

      g_save_latency_sum += save_latency;
      if (save_latency > g_save_latency_max)
        g_save_latency_max = save_latency;
      if (save_latency < g_save_latency_min)
        g_save_latency_min = save_latency;

      save_count++;
      cmd_count = 0;

      /* 打印进度 */
      fprintf(stderr, "\r  进度: %d/%d  save次数: %d  ", i + 1, g_total,
              save_count);
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &g_end);
  close(fd);

  /* 计算统计结果 */
  double total_elapsed =
      (g_end.tv_sec - g_start.tv_sec) + (g_end.tv_nsec - g_start.tv_nsec) / 1e9;
  double total_qps = (g_cmd_sent + g_save_sent) / total_elapsed;
  double cmd_qps = g_cmd_sent / (total_elapsed - g_save_latency_sum / 1000.0);
  double avg_save_latency =
      g_save_sent > 0 ? g_save_latency_sum / g_save_sent : 0;

  /* 打印结果 */
  printf("\n\n========== 测试结果 ==========\n");
  printf("总耗时:       %.2f 秒\n", total_elapsed);
  printf("\n");
  printf("普通命令:\n");
  printf("  发送数:     %lld\n", g_cmd_sent);
  printf("  成功数:     %lld\n", g_cmd_ok);
  printf("  失败数:     %lld\n", g_cmd_err);
  printf("\n");
  printf("Save 命令:\n");
  printf("  发送数:     %lld\n", g_save_sent);
  printf("  成功数:     %lld\n", g_save_ok);
  printf("  失败数:     %lld\n", g_save_err);
  printf("\n");
  printf("QPS:\n");
  printf("  整体 QPS:   %.0f 请求/秒\n", total_qps);
  printf("  命令 QPS:   %.0f 请求/秒 (不含 save 等待时间)\n", cmd_qps);
  printf("\n");
  printf("Save 延迟:\n");
  printf("  平均延迟:   %.2f ms\n", avg_save_latency);
  printf("  最大延迟:   %.2f ms\n", g_save_latency_max);
  printf("  最小延迟:   %.2f ms\n", g_save_latency_min);
  printf("\n");

  /* 计算 save 对 QPS 的影响 */
  if (g_save_sent > 0) {
    double save_time_ratio =
        (g_save_latency_sum / 1000.0) / total_elapsed * 100;
    printf("Save 时间占比: %.1f%%\n", save_time_ratio);
    printf("每 %d 个命令的 save 导致 QPS 下降约 %.1f%%\n", g_save_interval,
           (1.0 - total_qps / cmd_qps) * 100);
  }
  printf("==============================\n");
}

/* ── 帮助信息 ── */
static void usage(const char *prog) {
  printf("用法: %s [选项]\n", prog);
  printf("选项:\n");
  printf("  -H <主机>       服务器地址 (默认: 127.0.0.1)\n");
  printf("  -b <地址>       绑定源地址\n");
  printf("  -p <端口>       服务器端口 (默认: 2000)\n");
  printf("  -n <请求数>     总请求数 (默认: 100000)\n");
  printf("  -s <间隔>       每隔多少个命令发送一次 save (默认: 1000)\n");
  printf("  -t <命令>       测试命令 (默认: set)\n");
  printf("\n命令列表:\n");
  printf("  array:     set, get, del, mod, exist\n");
  printf("  rbtree:    rset, rget, rdel, rmod, rexist\n");
  printf("  hash:      hset, hget, hdel, hmod, hexist\n");
  printf("  skiplist:  skset, skget, skdel, skmod, skexist\n");
  printf("  echo:      echo\n");
  printf("\n示例:\n");
  printf("  %s -t set -n 10000 -s 1000\n", prog);
  printf("  %s -t hset -n 10000 -s 500 -H 192.168.5.17 -p 2000\n", prog);
  printf(
      "  %s -t echo -n 50000 -s 2000 -H 192.168.5.18 -b 192.168.5.17 -p 6379\n",
      prog);
}

/* ── 主函数 ── */
int main(int argc, char *argv[]) {
  const char *cmd_arg = "set";
  cmd_type_t type = CMD_SET;

  int opt;
  while ((opt = getopt(argc, argv, "H:b:p:n:s:t:")) != -1) {
    switch (opt) {
    case 'H':
      g_host = optarg;
      break;
    case 'b':
      g_bind = optarg;
      break;
    case 'p':
      g_port = atoi(optarg);
      break;
    case 'n':
      g_total = atoi(optarg);
      break;
    case 's':
      g_save_interval = atoi(optarg);
      break;
    case 't':
      cmd_arg = optarg;
      break;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  if (g_total <= 0) {
    fprintf(stderr, "请求数必须大于 0\n");
    return 1;
  }
  if (g_save_interval <= 0) {
    fprintf(stderr, "Save 间隔必须大于 0\n");
    return 1;
  }

  /* 解析命令类型 */
  int found = 0;
  for (int i = 0; i < CMD_CMD_COUNT; i++) {
    if (strcmp(cmd_arg, cmd_names[i]) == 0) {
      type = (cmd_type_t)i;
      found = 1;
      break;
    }
  }
  if (!found) {
    fprintf(stderr, "未知命令: %s\n", cmd_arg);
    usage(argv[0]);
    return 1;
  }

  run_test(type);
  return 0;
}
