/*
 * benchmark - 类似 redis-benchmark 的批量性能测试工具
 *
 * 用法: ./benchmark [-H 主机] [-p 端口] [-c 并发数] [-n 请求数] [-t 命令] [-q]
 *
 * 示例:
 *   ./benchmark -t set -n 100000 -c 50 -q
 *   ./benchmark -t get -n 100000 -c 100
 *   ./benchmark -t set,get,del -n 50000 -c 20
 *   ./benchmark -t hset,rset,skset -n 100000 -c 100 -q
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
#define RECV_BUF_SIZE   65536
#define CMD_BUF_SIZE    256

/* ── 全局配置 ── */
static const char *g_host    = "127.0.0.1";
static int         g_port    = 2000;
static int         g_conns   = 50;       /* -c */
static int         g_total   = 100000;   /* -n */
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
    int      pending;       /* 已发未收的数量 (用于限流) */
    char    *recv_buf;
    int      recv_len;
    int      done;          /* 此连接是否完成全部发送 */
    int      key_start;     /* 此连接负责的 key 起始编号 */
    int      key_count;     /* 此连接负责的 key 数量 */
} conn_t;

/* ── 全局统计 ── */
static int         g_finished = 0;       /* 已完成的连接数 */
static long long   g_total_ok = 0;
static long long   g_total_err = 0;
static long long   g_progress_count = 0; /* 进度计数（仅用于显示） */
static struct timespec g_start, g_end;

/* ── RESP 编码 ── */
static int encode_resp(char *buf, int bufsz, const char *cmd, const char *key, const char *val) {
    if (val)
        return snprintf(buf, bufsz,
            "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(cmd), cmd, strlen(key), key, strlen(val), val);
    else
        return snprintf(buf, bufsz,
            "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(cmd), cmd, strlen(key), key);
}

/* ── 创建非阻塞 TCP 连接 ── */
static int create_conn(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── 发送一条命令 ── */
static int send_cmd(conn_t *c, cmd_type_t type, int key_idx) {
    char buf[CMD_BUF_SIZE];
    int len;
    char key[64], val[64];

    snprintf(key, sizeof(key), "key%d", key_idx);
    snprintf(val, sizeof(val), "%d", key_idx);

    /* set/rset/hset/skset/mod/rmod/hmod/skmod 需要 value，其余只需 key */
    switch (type) {
    case CMD_SET:  case CMD_RSET:  case CMD_HSET:  case CMD_SKSET:
    case CMD_MOD:  case CMD_RMOD:  case CMD_HMOD:  case CMD_SKMOD:
        len = encode_resp(buf, sizeof(buf), cmd_names[type], key, val);
        break;
    case CMD_GET:  case CMD_RGET:  case CMD_HGET:  case CMD_SKGET:
    case CMD_DEL:  case CMD_RDEL:  case CMD_HDEL:  case CMD_SKDEL:
    case CMD_EXIST: case CMD_REXIST: case CMD_HEXIST: case CMD_SKEXIST:
        len = encode_resp(buf, sizeof(buf), cmd_names[type], key, NULL);
        break;
    case CMD_ECHO:
        len = encode_resp(buf, sizeof(buf), "echo", val, NULL);
        break;
    default:
        return -1;
    }

    int n = send(c->fd, buf, len, MSG_NOSIGNAL);
    return (n == len) ? 0 : -1;
}

/* ── 处理接收数据，返回解析出的响应数 ── */
static int process_recv(conn_t *c) {
    int parsed = 0;
    char *p = c->recv_buf;
    int remain = c->recv_len;

    while (remain >= 4) {  /* 最短响应: "+OK\r\n" = 5, 但 "-ERR\r\n" 可能更短 */
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
            /* 未知字节，跳过一个 */
            p++;
            remain--;
        }
    }

    /* 移动未处理的数据到缓冲区头部 */
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

    fprintf(stderr, "\n  %-6s  请求: %d  并发: %d  命令: %s\n",
            cmd_name, g_total, g_conns, cmd_name);

    /* 创建所有连接 */
    for (int i = 0; i < g_conns; i++) {
        int fd = create_conn(g_host, g_port);
        if (fd < 0) {
            fprintf(stderr, "  连接失败 #%d: %s\n", i, strerror(errno));
            /* 清理已创建的 */
            for (int j = 0; j < i; j++) {
                close(conns[j].fd);
                free(conns[j].recv_buf);
            }
            close(epfd);
            return;
        }
        conns[i].fd = fd;
        conns[i].id = i;
        conns[i].recv_buf = malloc(RECV_BUF_SIZE);
        conns[i].recv_len = 0;
        conns[i].key_start = i * base + (i < extra ? i : extra);
        conns[i].key_count = base + (i < extra ? 1 : 0);
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
    int max_pending = 64;  /* 每连接最大在途请求数 */

    while (g_finished < g_conns) {
        /* 尝试给每个连接填充请求 */
        for (int i = 0; i < g_conns; i++) {
            conn_t *c = &conns[i];
            if (c->done) continue;

            while (c->sent < c->key_count && c->pending < max_pending) {
                int key_idx = c->key_start + c->sent;
                if (send_cmd(c, type, key_idx) < 0) {
                    c->done = 1;
                    break;
                }
                c->sent++;
                c->pending++;
            }

            /* 所有请求已发出，标记发送完成 */
            if (c->sent >= c->key_count) {
                /* 如果所有响应也已收到，才标记 done */
                if (c->pending == 0) {
                    c->done = 1;
                    g_finished++;
                    /* 修改 epoll 不再监听写，但继续监听读以消费残余数据 */
                    struct epoll_event ev = {
                        .events = EPOLLIN,
                        .data.ptr = c,
                    };
                    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
                }
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
                    c->done = 1;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                    close(c->fd);
                    continue;
                }
                c->recv_len += n;
                int parsed = process_recv(c);
                g_progress_count += parsed;

                /* 检查是否全部完成 */
                if (c->sent >= c->key_count && c->pending <= 0 && !c->done) {
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

    /* 收尾：统一累加每个连接的统计 */
    for (int i = 0; i < g_conns; i++) {
        g_total_ok += conns[i].recv_ok;
        g_total_err += conns[i].recv_err;
        close(conns[i].fd);
        free(conns[i].recv_buf);
    }
    close(epfd);

    /* 打印结果 */
    double elapsed = (g_end.tv_sec - g_start.tv_sec) +
                     (g_end.tv_nsec - g_start.tv_nsec) / 1e9;
    double qps = g_total / elapsed;
    double avg_latency = (elapsed / g_total) * 1000.0;  /* ms */

    if (g_quiet) {
        printf("%-6s: %.2f requests per second\n", cmd_name, qps);
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
    printf("  -H <主机>     服务器地址 (默认: 127.0.0.1)\n");
    printf("  -p <端口>     服务器端口 (默认: 2000)\n");
    printf("  -c <并发数>   并发连接数 (默认: 50)\n");
    printf("  -n <请求数>   总请求数   (默认: 100000)\n");
    printf("  -t <命令>     测试命令，逗号分隔 (默认: set)\n");
    printf("  -q            安静模式，只输出 QPS\n");
    printf("\n命令列表:\n");
    printf("  array:     set, get, del, mod, exist\n");
    printf("  rbtree:    rset, rget, rdel, rmod, rexist\n");
    printf("  hash:      hset, hget, hdel, hmod, hexist\n");
    printf("  skiplist:  skset, skget, skdel, skmod, skexist\n");
    printf("  echo:      echo\n");
    printf("\n示例:\n");
    printf("  %s -t set -n 100000 -c 50 -q\n", prog);
    printf("  %s -t set,get,del -n 50000 -c 20\n", prog);
    printf("  %s -t hset,rset,skset -n 100000 -c 100 -q\n", prog);
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
    while ((opt = getopt(argc, argv, "H:p:c:n:t:q")) != -1) {
        switch (opt) {
        case 'H': g_host  = optarg;          break;
        case 'p': g_port  = atoi(optarg);    break;
        case 'c': g_conns = atoi(optarg);    break;
        case 'n': g_total = atoi(optarg);    break;
        case 't': cmd_arg = optarg;          break;
        case 'q': g_quiet = 1;              break;
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

    memset(cmd_selected, 0, sizeof(cmd_selected));
    if (parse_commands(cmd_arg) <= 0) {
        usage(argv[0]);
        return 1;
    }

    printf("目标服务器: %s:%d\n", g_host, g_port);
    printf("请求数量:   %d\n", g_total);
    printf("并发连接:   %d\n", g_conns);

    for (int i = 0; i < CMD_CMD_COUNT; i++) {
        if (cmd_selected[i])
            run_bench(cmd_names[i], (cmd_type_t)i);
    }

    return 0;
}
