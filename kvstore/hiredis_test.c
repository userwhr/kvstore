#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// ===================== 配置（按需修改） =====================
#define SERVER_IP      "192.168.140.134"
#define SERVER_PORT    2000
#define PIPELINE_BATCH 100      // 每批pipeline条数
#define PER_OP_COUNT   100    // 每种命令发送条数
#define TOTAL_COUNT    (PER_OP_COUNT * 4)
#define RECV_BUF_SIZE  65536
#define OK_REPLY       "+OK\r\n"
#define OK_REPLY_LEN   5

static double get_cost(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) +
           (end->tv_usec - start->tv_usec) / 1000000.0;
}

// 从socket读取count条回复，每条与 "+OK\r\n" 比较
// 返回成功数，失败数存入 *fail
static int recv_and_check(int fd, int count, int *fail) {
    char recvbuf[RECV_BUF_SIZE];
    int recvlen = 0;
    int need = count * OK_REPLY_LEN;

    while (recvlen < need) {
        int n = recv(fd, recvbuf + recvlen, sizeof(recvbuf) - recvlen, 0);
        if (n <= 0) {
            // 连接关闭，剩余的全算失败
            int received = recvlen / OK_REPLY_LEN;
            *fail += (count - received);
            return 0;
        }
        recvlen += n;
    }

    // 逐条用 strcmp 比较 "+OK\r\n"
    int success = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(recvbuf + i * OK_REPLY_LEN, OK_REPLY, OK_REPLY_LEN) == 0) {
            success++;
        } else {
            (*fail)++;
        }
    }
    return success;
}

int main() {
    printf("Pipeline测试: %s:%d | 4种命令 x %d条 = %d条 | 每批%d条\n",
           SERVER_IP, SERVER_PORT, PER_OP_COUNT, TOTAL_COUNT, PIPELINE_BATCH);
    printf("===============================================================\n");

    const char *ops[] = {"set", "rset", "hset", "skset"};
    int total_success = 0, total_fail = 0;

    struct timeval t_start, t_end;
    gettimeofday(&t_start, NULL);

    for (int op_idx = 0; op_idx < 4; op_idx++) {
        // 每种命令创建独立连接
        redisContext *ctx = redisConnect(SERVER_IP, SERVER_PORT);
        if (ctx == NULL || ctx->err) {
            printf("  [%s] 连接失败: %s\n", ops[op_idx],
                   ctx ? ctx->errstr : "内存分配失败");
            if (ctx) redisFree(ctx);
            total_fail += PER_OP_COUNT;
            continue;
        }

        int fd = ctx->fd;  // 拿到底层socket fd
        int success = 0, fail = 0;
        struct timeval op_start, op_end;
        gettimeofday(&op_start, NULL);

        for (int i = 1; i <= PER_OP_COUNT; i++) {
            // 用hiredis发送RESP命令到pipeline缓冲区
            redisAppendCommand(ctx, "%s %d %d", ops[op_idx], i, i);

            // 每100条或最后一条时，flush并读取回复
            if (i % PIPELINE_BATCH == 0 || i == PER_OP_COUNT) {
                int batch_count = (i % PIPELINE_BATCH == 0) ? PIPELINE_BATCH
                                                             : ((i - 1) % PIPELINE_BATCH + 1);
                success += recv_and_check(fd, batch_count, &fail);
            }

            if (i % 5000 == 0) {
                printf("  [%s] 进度: %d/%d\n", ops[op_idx], i, PER_OP_COUNT);
            }
        }

        gettimeofday(&op_end, NULL);
        double op_cost = get_cost(&op_start, &op_end);
        printf("  [%s] 完成: 成功=%d 失败=%d 耗时=%.3fs QPS=%.0f\n",
               ops[op_idx], success, fail, op_cost, PER_OP_COUNT / op_cost);

        total_success += success;
        total_fail += fail;
        redisFree(ctx);
    }

    gettimeofday(&t_end, NULL);
    double total_cost = get_cost(&t_start, &t_end);

    printf("\n===============================================================\n");
    printf("Pipeline测试完成\n");
    printf("---------------------------------------------------------------\n");
    printf("总请求数:   %d\n", TOTAL_COUNT);
    printf("Pipeline:   %d条/批\n", PIPELINE_BATCH);
    printf("成功:       %d\n", total_success);
    printf("失败:       %d\n", total_fail);
    printf("总耗时:     %.3f 秒\n", total_cost);
    printf("QPS:        %.0f 请求/秒\n", TOTAL_COUNT / total_cost);
    printf("===============================================================\n");

    return 0;
}
