/*
 * benchmark_pipeline - Pipeline 批量发送性能测试工具
 *
 * 用法: ./benchmark_pipeline [-H 主机] [-p 端口] [-c 并发数] [-n 请求数] [-P pipeline大小] [-t 命令] [-q]
 *
 * 示例:
 *   ./benchmark_pipeline -t set -n 100000 -c 50 -P 100 -q
 *   ./benchmark_pipeline -t echo -n 100000 -c 50 -P 64 -H 192.168.5.16 -b 192.168.5.17
 *   ./benchmark_pipeline -t set,get,del -n 50000 -c 20 -P 50
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <getopt.h>

#define MAX_CONNS       1024
#define RECV_BUF_SIZE   262144      /* 256KB，pipeline 模式需要更大缓冲区 */
#define CMD_BUF_SIZE    256
#define MAX_PIPELINE    1024        /* 最大 pipeline 大小 */

/* ── 全局配置 ── */
static const char *g_host    = "127.0.0.1";
static const char *g_bind    = NULL;      /* -b 绑定源地址 */
static int         g_port    = 2000;
static int         g_conns   = 50;       /* -c */
static int         g_total   = 100000;   /* -n */
static int         g_pipeline = 64;      /* -P pipeline 大小 */
static int         g_quiet   = 0;        /* -q */

/* ── 命令类型 (4 存储后端 x 增删改查存在) ── */
typedef enum {
    /* array */
    CMD_SET, CMD_GET, CMD_DEL, CMD_MOD, CMD_EXIST,
    /* rbtree */
    CMD_RSET, CMD_RGET, CMD_RDEL, CMD_RMOD, CMD_REXIST,
    /* hash */
    CMD_HSET, CMD_HGET, CMD_HDEL, CMD_HMOD, CMD_HEXIST,
    /* skiplist */
    CMD_SKSET, CMD_SKGET, CMD_SKDEL, CMD_SKMOD, CMD_SKEXIST,
    /* special */
    CMD_ECHO,
    CMD_CMD_COUNT
} cmd_type_t;

static const char *cmd_names[] = {
    "set",   "get",     "del",   "mod",   "exist",
    "rset",  "rget",    "rdel",  "rmod",  "rexist",
    "hset",  "hget",    "hdel",  "hmod",  "hexist",
    "skset", "skget",   "skdel", "skmod", "skexist",
    "echo",
};
static int cmd_selected[CMD_CMD_COUNT];

/* ── 单连接状态 ── */
typedef struct {
    int      fd;
    int      id;            /* 连接编号 0..conns-1 */
    int      sent;          /* 已发送请求数 */
    int      recv_ok;       /* 收到 OK 的数量 */
    int      recv_err;      /* 收到错误的数量 */
    int      pending;       /* 已发未收的数量 */
    char    *recv_buf;
    int      recv_len;
    int      done;          /* 此连接是否完成全部发送 */
    int      key_start;     /* 此连接负责的 key 起始编号 */
    int      key_count;     /* 此连接负责的 key 数量 */
    int      key_next;      /* 下一个要发送的 key 编号 */
    char    *pipeline_buf;  /* pipeline 发送缓冲区 */
    int      pipeline_len;  /* 当前 pipeline 缓冲区中的数据长度 */
    int      pipeline_count;/* 当前 pipeline 中的命令数 */
} conn_t;

/* ── 全局统计 ── */
static int         g_finished = 0;
static long long   g_total_ok = 0;
static long long   g_total_err = 0;
static long long   g_progress_count = 0;
static struct timespec g_start, g_end;

/* ── RESP 编码（追加到缓冲区） ── */
static int encode_resp_append(char *buf, int bufsz, int offset,
                               const char *cmd, const char *key, const char *val) {
    if (val)
        return snprintf(buf + offset, bufsz - offset,
            "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(cmd), cmd, strlen(key), key, strlen(val), val);
    else
        return snprintf(buf + offset, bufsz - offset,
            "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(cmd), cmd, strlen(key), key);
}

/* ── 创建 TCP 连接 ── */
static int create_conn(const char *host, int port, const char *bind_addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);

    /* 指定 -b 时绑定源地址，确保不走 lo 网卡 */
    if (bind_addr) {
        struct sockaddr_in src = {
            .sin_family = AF_INET,
            .sin_port   = htons(0),
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

/* ── 构建一条命令，追加到 pipeline 缓冲区 ── */
static int append_cmd(conn_t *c, cmd_type_t type, int key_idx) {
    char key[64], val[64];
    int len;

    snprintf(key, sizeof(key), "key%d", key_idx);
    snprintf(val, sizeof(val), "%d", key_idx);

    switch (type) {
    case CMD_SET:  case CMD_RSET:  case CMD_HSET:  case CMD_SKSET:
    case CMD_MOD:  case CMD_RMOD:  case CMD_HMOD:  case CMD_SKMOD:
        len = encode_resp_append(c->pipeline_buf, CMD_BUF_SIZE * g_pipeline,
                                  c->pipeline_len, cmd_names[type], key, val);
        break;
    case CMD_GET:  case CMD_RGET:  case CMD_HGET:  case CMD_SKGET:
    case CMD_DEL:  case CMD_RDEL:  case CMD_HDEL:  case CMD_SKDEL:
    case CMD_EXIST: case CMD_REXIST: case CMD_HEXIST: case CMD_SKEXIST:
        len = encode_resp_append(c->pipeline_buf, CMD_BUF_SIZE * g_pipeline,
                                  c->pipeline_len, cmd_names[type], key, NULL);
        break;
    case CMD_ECHO:
        len = encode_resp_append(c->pipeline_buf, CMD_BUF_SIZE * g_pipeline,
                                  c->pipeline_len, "echo", val, NULL);
        break;
    default:
        return -1;
    }

    if (len < 0 || c->pipeline_len + len > CMD_BUF_SIZE * g_pipeline)
        return -1;  /* 缓冲区满 */

    c->pipeline_len += len;
    c->pipeline_count++;
    return 0;
}

/* ── 发送 pipeline 缓冲区 ── */
static int flush_pipeline(conn_t *c) {
    if (c->pipeline_len == 0) return 0;

    int total = c->pipeline_len;
    int sent = 0;

    while (sent < total) {
        int n = send(c->fd, c->pipeline_buf + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }

    c->pending += c->pipeline_count;
    c->sent += c->pipeline_count;
    c->pipeline_len = 0;
    c->pipeline_count = 0;
    return 0;
}

/* ── 处理接收数据，返回解析出的响应数 ── */
static int process_recv(conn_t *c) {
    int parsed = 0;
    char *p = c->recv_buf;
    int remain = c->recv_len;

    while (remain >= 4) {
        if (p[0] == '+') {
            /* +OK\r\n */
            char *end = memmem(p, remain, "\r\n", 2);
            if (!end) break;
            c->recv_ok++;
            int consumed = (int)(end - p) + 2;
            p += consumed;
            remain -= consumed;
            parsed++;
            c->pending--;
        } else if (p[0] == '-') {
            /* -ERR ...\r\n */
            char *end = memmem(p, remain, "\r\n", 2);
            if (!end) break;
            c->recv_err++;
            int consumed = (int)(end - p) + 2;
            p += consumed;
            remain -= consumed;
            parsed++;
            c->pending--;
        } else if (p[0] == '$') {
            /* $len\r\ndata\r\n */
            char *end = memmem(p, remain, "\r\n", 2);
            if (!end) break;
            int vlen = atoi(p + 1);
            int header = (int)(end - p) + 2;
            if (vlen < 0) {
                /* $-1 (nil) */
                c->recv_ok++;
                p += header;
                remain -= header;
                parsed++;
                c->pending--;
            } else if (remain >= header + vlen + 2) {
                c->recv_ok++;
                p += header + vlen + 2;
                remain -= header + vlen + 2;
                parsed++;
                c->pending--;
            } else {
                break;
            }
        } else {
            p++;
            remain--;
        }
    }

    if (remain > 0 && p != c->recv_buf)
        memmove(c->recv_buf, p, remain);
    c->recv_len = remain;

    return parsed;
}

/* ── 打印进度 ── */
static void print_progress(int total) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - g_start.tv_sec) +
                     (now.tv_nsec - g_start.tv_nsec) / 1e9;
    double qps = g_progress_count / elapsed;
    fprintf(stderr, "\r  进度: %lld/%d  QPS: %.0f  ",
            g_progress_count, total, qps);
}

/* ── 执行一次 benchmark ── */
static void run_bench(const char *cmd_name, cmd_type_t type) {
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return; }

    conn_t conns[MAX_CONNS];
    memset(conns, 0, sizeof(conns));

    /* 计算每个连接分到多少请求 */
    int base = g_total / g_conns;
    int extra = g_total % g_conns;

    fprintf(stderr, "\n  %-6s  请求: %d  并发: %d  pipeline: %d  命令: %s\n",
            cmd_name, g_total, g_conns, g_pipeline, cmd_name);

    /* 创建所有连接 */
    for (int i = 0; i < g_conns; i++) {
        int fd = create_conn(g_host, g_port, g_bind);
        if (fd < 0) {
            fprintf(stderr, "  连接失败 #%d: %s\n", i, strerror(errno));
            for (int j = 0; j < i; j++) {
                close(conns[j].fd);
                free(conns[j].recv_buf);
                free(conns[j].pipeline_buf);
            }
            close(epfd);
            return;
        }
        conns[i].fd = fd;
        conns[i].id = i;
        conns[i].recv_buf = malloc(RECV_BUF_SIZE);
        conns[i].recv_len = 0;
        conns[i].pipeline_buf = malloc(CMD_BUF_SIZE * g_pipeline);
        conns[i].pipeline_len = 0;
        conns[i].pipeline_count = 0;
        conns[i].key_start = i * base + (i < extra ? i : extra);
        conns[i].key_count = base + (i < extra ? 1 : 0);
        conns[i].key_next = conns[i].key_start;
        conns[i].sent = 0;
        conns[i].pending = 0;
        conns[i].done = 0;

        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.ptr = &conns[i],
        };
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }

    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_finished = 0;
    g_total_ok = 0;
    g_total_err = 0;
    g_progress_count = 0;

    struct epoll_event events[MAX_CONNS];

    while (g_finished < g_conns) {
        /* 尝试给每个连接填充 pipeline 并发送 */
        for (int i = 0; i < g_conns; i++) {
            conn_t *c = &conns[i];
            if (c->done) continue;

            /* 构建 pipeline */
            while (c->key_next < c->key_start + c->key_count && c->pipeline_count < g_pipeline) {
                int key_idx = c->key_next;
                if (append_cmd(c, type, key_idx) < 0)
                    break;
                c->key_next++;
            }

            /* 发送 pipeline */
            if (c->pipeline_count > 0) {
                if (flush_pipeline(c) < 0) {
                    c->done = 1;
                    g_finished++;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    continue;
                }
            }

            /* 所有请求已发出且响应已收到 */
            if (c->key_next >= c->key_start + c->key_count && c->pending == 0 && !c->done) {
                c->done = 1;
                g_finished++;
            }
        }

        /* 等待事件 */
        int nfds = epoll_wait(epfd, events, MAX_CONNS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            conn_t *c = events[i].data.ptr;

            if (events[i].events & EPOLLIN) {
                int n = recv(c->fd, c->recv_buf + c->recv_len,
                             RECV_BUF_SIZE - c->recv_len, 0);
                if (n <= 0) {
                    if (!c->done) g_finished++;
                    if (c->recv_ok == 0 && c->recv_err == 0) {
                        fprintf(stderr, "  连接 #%d 立即断开 (recv=%d, pending=%d, sent=%d, errno=%s)\n",
                                c->id, n, c->pending, c->sent, n < 0 ? strerror(errno) : "EOF");
                    }
                    c->done = 1;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    continue;
                }
                c->recv_len += n;
                int parsed = process_recv(c);
                g_progress_count += parsed;

                if (c->key_next >= c->key_start + c->key_count && c->pending <= 0 && !c->done) {
                    c->done = 1;
                    g_finished++;
                }
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (!c->done) g_finished++;
                c->done = 1;
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd);
            }
        }

        /* 定期打印进度 */
        if (!g_quiet) {
            static int tick = 0;
            if (++tick % 200 == 0)
                print_progress(g_total);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &g_end);

    /* 收尾 */
    for (int i = 0; i < g_conns; i++) {
        g_total_ok += conns[i].recv_ok;
        g_total_err += conns[i].recv_err;
        close(conns[i].fd);
        free(conns[i].recv_buf);
        free(conns[i].pipeline_buf);
    }
    close(epfd);

    /* 打印结果 */
    double elapsed = (g_end.tv_sec - g_start.tv_sec) +
                     (g_end.tv_nsec - g_start.tv_nsec) / 1e9;
    double qps = g_total_ok / elapsed;
    double avg_latency = (g_total_ok > 0) ? (elapsed / g_total_ok) * 1000.0 : 0;

    if (g_quiet) {
        printf("%-6s: %.2f requests per second (成功: %lld)\n", cmd_name, qps, g_total_ok);
    } else {
        fprintf(stderr, "\n");
        printf("  %-6s  成功: %lld  失败: %lld\n", cmd_name, g_total_ok, g_total_err);
        printf("  耗时:   %.2f 秒\n", elapsed);
        printf("  QPS:    %.0f 请求/秒\n", qps);
        printf("  延迟:   %.4f ms/请求 (平均)\n", avg_latency);
    }
}

/* ── 帮助信息 ── */
static void usage(const char *prog) {
    printf("用法: %s [选项]\n", prog);
    printf("选项:\n");
    printf("  -H <主机>       服务器地址 (默认: 127.0.0.1)\n");
    printf("  -b <地址>       绑定源地址，用于指定出口网卡\n");
    printf("  -p <端口>       服务器端口 (默认: 2000)\n");
    printf("  -c <并发数>     并发连接数 (默认: 50)\n");
    printf("  -n <请求数>     总请求数   (默认: 100000)\n");
    printf("  -P <pipeline>   Pipeline 大小 (默认: 64)\n");
    printf("  -t <命令>       测试命令，逗号分隔 (默认: set)\n");
    printf("  -q              安静模式，只输出 QPS\n");
    printf("\n命令列表:\n");
    printf("  array:     set, get, del, mod, exist\n");
    printf("  rbtree:    rset, rget, rdel, rmod, rexist\n");
    printf("  hash:      hset, hget, hdel, hmod, hexist\n");
    printf("  skiplist:  skset, skget, skdel, skmod, skexist\n");
    printf("  echo:      echo\n");
    printf("\n示例:\n");
    printf("  %s -t set -n 100000 -c 50 -P 100 -q\n", prog);
    printf("  %s -t echo -n 100000 -c 50 -P 64 -H 192.168.5.16 -b 192.168.5.17\n", prog);
}

/* ── 解析命令列表 ── */
static int parse_commands(const char *arg) {
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int count = 0;
    char *tok = strtok(buf, ",");
    while (tok) {
        int found = 0;
        for (int i = 0; i < CMD_CMD_COUNT; i++) {
            if (strcmp(tok, cmd_names[i]) == 0) {
                cmd_selected[i] = 1;
                found = 1;
                count++;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "未知命令: %s\n", tok);
            return -1;
        }
        tok = strtok(NULL, ",");
    }
    return count;
}

int main(int argc, char *argv[]) {
    const char *cmd_arg = "set";

    int opt;
    while ((opt = getopt(argc, argv, "H:b:p:c:n:P:t:q")) != -1) {
        switch (opt) {
        case 'H': g_host     = optarg;       break;
        case 'b': g_bind     = optarg;       break;
        case 'p': g_port     = atoi(optarg); break;
        case 'c': g_conns    = atoi(optarg); break;
        case 'n': g_total    = atoi(optarg); break;
        case 'P': g_pipeline = atoi(optarg); break;
        case 't': cmd_arg    = optarg;       break;
        case 'q': g_quiet    = 1;            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (g_conns <= 0 || g_conns > MAX_CONNS) {
        fprintf(stderr, "并发数必须在 1~%d 之间\n", MAX_CONNS);
        return 1;
    }
    if (g_total <= 0) {
        fprintf(stderr, "请求数必须大于 0\n");
        return 1;
    }
    if (g_pipeline <= 0 || g_pipeline > MAX_PIPELINE) {
        fprintf(stderr, "Pipeline 大小必须在 1~%d 之间\n", MAX_PIPELINE);
        return 1;
    }

    memset(cmd_selected, 0, sizeof(cmd_selected));
    if (parse_commands(cmd_arg) <= 0) {
        usage(argv[0]);
        return 1;
    }

    printf("目标服务器: %s:%d\n", g_host, g_port);
    printf("请求数量:   %d\n", g_total);
    printf("并发连接:   %d\n", g_conns);
    printf("Pipeline:   %d\n", g_pipeline);

    for (int i = 0; i < CMD_CMD_COUNT; i++) {
        if (cmd_selected[i])
            run_bench(cmd_names[i], (cmd_type_t)i);
    }

    return 0;
}
