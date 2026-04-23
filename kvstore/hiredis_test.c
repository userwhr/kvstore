#include <ctype.h>
#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// ===================== 固定配置（和Python完全一致） =====================
#define SERVER_IP "192.168.140.134"
#define SERVER_PORT 2000
#define TEST_COUNT 10 // 固定1000条测试
#define BUF_SIZE 4096

// 大Value字符串（对应Python的LARGE_VALUE）
#define LARGE_VALUE                                                            \
  "测试大Value内容：abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstu" \
  "vwxyz1234567890abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstuvw" \
  "xyz1234567890abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstuvwxy" \
  "z1234567890abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstuvwxyz1" \
  "234567890abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqrstuvwxyz123" \
  "4567890"

// 大Key（对应Python的BIG_KEY）
#define BIG_KEY                                                                \
  "bigkey_"                                                                    \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BIG_KEY_VALUE "value_for_big_key"
#define NEW_BIG_VALUE "modified_value_for_big_key"

// 全局连接句柄
redisContext *ctx;

// ===================== 工具函数 =====================
// 计算耗时（秒）
double get_cost(struct timeval *start, struct timeval *end) {
  return (end->tv_sec - start->tv_sec) +
         (end->tv_usec - start->tv_usec) / 1000000.0;
}

// 清空输入缓冲区
void flush_stdin() {
  char c;
  while ((c = getchar()) != '\n' && c != EOF)
    ;
}

// 校验服务端响应
int check_reply(redisReply *reply, const char *expect) {
  int res = 0;
  if (reply == NULL)
    return 0;

  if (reply->type == REDIS_REPLY_STATUS) {
    res = (strcmp(reply->str, expect) == 0);
  } else if (reply->type == REDIS_REPLY_STRING) {
    res = (strcmp(reply->str, expect) == 0);
  }
  freeReplyObject(reply);
  return res;
}

// ===================== 批量测试核心（对应Python run_batch）
// =====================
void run_batch(const char *op, int mul) {
  int success = 0, fail = 0;
  struct timeval start, end;
  gettimeofday(&start, NULL);

  printf("\n🚀 启动【%s】批量测试：key1~key%d | 倍数=%d\n", op, TEST_COUNT,
         mul);
  printf("===============================================================\n");

  char cmd[256], key[32], val[32];
  redisReply *reply;

  for (int i = 1; i <= TEST_COUNT; i++) {
    snprintf(key, sizeof(key), "key%d", i);
    snprintf(val, sizeof(val), "%d", i * mul);

    // 拼接命令
    if (strcmp(op, "set") == 0)
      snprintf(cmd, sizeof(cmd), "set %s %s", key, val);
    else if (strcmp(op, "get") == 0)
      snprintf(cmd, sizeof(cmd), "get %s", key);
    else if (strcmp(op, "mod") == 0)
      snprintf(cmd, sizeof(cmd), "mod %s %s", key, val);
    else if (strcmp(op, "exist") == 0)
      snprintf(cmd, sizeof(cmd), "exist %s", key);
    else if (strcmp(op, "del") == 0)
      snprintf(cmd, sizeof(cmd), "del %s", key);

    else if (strcmp(op, "rset") == 0)
      snprintf(cmd, sizeof(cmd), "rset %s %s", key, val);
    else if (strcmp(op, "rget") == 0)
      snprintf(cmd, sizeof(cmd), "rget %s", key);
    else if (strcmp(op, "rmod") == 0)
      snprintf(cmd, sizeof(cmd), "rmod %s %s", key, val);
    else if (strcmp(op, "rexist") == 0)
      snprintf(cmd, sizeof(cmd), "rexist %s", key);
    else if (strcmp(op, "rdel") == 0)
      snprintf(cmd, sizeof(cmd), "rdel %s", key);

    else if (strcmp(op, "hset") == 0)
      snprintf(cmd, sizeof(cmd), "hset %s %s", key, val);
    else if (strcmp(op, "hget") == 0)
      snprintf(cmd, sizeof(cmd), "hget %s", key);
    else if (strcmp(op, "hmod") == 0)
      snprintf(cmd, sizeof(cmd), "hmod %s %s", key, val);
    else if (strcmp(op, "hexist") == 0)
      snprintf(cmd, sizeof(cmd), "hexist %s", key);
    else if (strcmp(op, "hdel") == 0)
      snprintf(cmd, sizeof(cmd), "hdel %s", key);

    else if (strcmp(op, "skset") == 0)
      snprintf(cmd, sizeof(cmd), "skset %s %s", key, val);
    else if (strcmp(op, "skget") == 0)
      snprintf(cmd, sizeof(cmd), "skget %s", key);
    else if (strcmp(op, "skmod") == 0)
      snprintf(cmd, sizeof(cmd), "skmod %s %s", key, val);
    else if (strcmp(op, "skexist") == 0)
      snprintf(cmd, sizeof(cmd), "skexist %s", key);
    else if (strcmp(op, "skdel") == 0)
      snprintf(cmd, sizeof(cmd), "skdel %s", key);
    else {
      fail++;
      continue;
    }

    // 发送命令
    reply = redisCommand(ctx, cmd);
    // 校验预期结果
    int ok = 0;
    if (strstr(op, "set") || strstr(op, "mod") || strstr(op, "del")) {
      ok = check_reply(reply, "OK");
    } else if (strstr(op, "get")) {
      ok = check_reply(reply, val);
    } else if (strstr(op, "exist")) {
      ok = check_reply(reply, "exist");
    }

    ok ? success++ : fail++;

    // 进度打印
    if (i % 1000 == 0) {
      printf("📊 进度：%d/%d | 成功：%d | 失败：%d\n", i, TEST_COUNT, success,
             fail);
    }
  }

  // 统计结果
  gettimeofday(&end, NULL);
  double cost = get_cost(&start, &end);
  double qps = TEST_COUNT / cost;

  printf("\n===============================================================\n");
  printf("📊 测试完成\n");
  printf("操作：%s | 总数：%d\n", op, TEST_COUNT);
  printf("✅ 成功：%d | ❌ 失败：%d\n", success, fail);
  printf("⏱️ 总耗时：%.2fs | 🚀 速率：%.2f条/秒\n", cost, qps);
  printf("===============================================================\n");
}

// ===================== 大Key测试（对应Python test_big_key）
// =====================
void test_big_key() {
  printf("\n🔑 开始大Key完整测试（超长Key名称）\n");
  printf("===============================================================\n");

  redisReply *reply;
  // rset
  reply = redisCommand(ctx, "rset %s %s", BIG_KEY, BIG_KEY_VALUE);
  printf("📩 rset响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // rget
  reply = redisCommand(ctx, "rget %s", BIG_KEY);
  printf("📩 rget响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // rmod
  reply = redisCommand(ctx, "rmod %s %s", BIG_KEY, NEW_BIG_VALUE);
  printf("📩 rmod响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // rexist
  reply = redisCommand(ctx, "rexist %s", BIG_KEY);
  printf("📩 rexist响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // rdel
  reply = redisCommand(ctx, "rdel %s", BIG_KEY);
  printf("📩 rdel响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  printf("===============================================================\n");
}

// ===================== 大Value测试（对应Python test_big_value_mod_get）
// =====================
void test_big_value() {
  printf("\n💾 开始大Value的mod/get测试\n");
  printf("===============================================================\n");

  const char *test_key = "bigvalue_key";
  redisReply *reply;

  // set 大value
  reply = redisCommand(ctx, "set %s %s", test_key, LARGE_VALUE);
  printf("📩 set响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // get 大value
  reply = redisCommand(ctx, "get %s", test_key);
  printf("📩 get响应长度: %zu\n", reply ? strlen(reply->str) : 0);
  freeReplyObject(reply);

  // mod 大value
  char new_big_val[4096];
  snprintf(new_big_val, sizeof(new_big_val),
           "修改后的大Value内容：XYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZX"
           "YZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXY"
           "ZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZXYZ");
  reply = redisCommand(ctx, "mod %s %s", test_key, new_big_val);
  printf("📩 mod响应: %s\n", reply ? reply->str : "NULL");
  freeReplyObject(reply);

  // 删除（修复错误：正确执行del并释放reply）
  reply = redisCommand(ctx, "del %s", test_key);
  freeReplyObject(reply);

  printf("===============================================================\n");
}

// ===================== 主菜单（对应Python main） =====================
int main() {
  // 1. 连接服务器
  ctx = redisConnect(SERVER_IP, SERVER_PORT);
  if (ctx == NULL || ctx->err) {
    printf("❌ 连接失败: %s\n", ctx ? ctx->errstr : "内存分配失败");
    return 1;
  }
  printf("✅ 成功连接到 %s:%d\n", SERVER_IP, SERVER_PORT);
  printf("📌 指令说明：\n");
  printf("   - 输入 'exit' 退出\n");
  printf("   - 输入 'bigkey' 测试大Key\n");
  printf("   - 输入 'bigvalue' 测试大Value\n");
  printf("   - 基础指令：set/get/mod/exist/del\n");
  printf("   - r/h/sk前缀指令：rset/rget/skset...\n");

  char input[64];
  while (1) {
    printf("\n请输入指令：");
    fgets(input, sizeof(input), stdin);
    input[strcspn(input, "\n")] = 0; // 去除换行
    for (int i = 0; input[i]; i++)
      input[i] = tolower(input[i]);

    // 退出
    if (strcmp(input, "exit") == 0) {
      printf("👋 退出程序...\n");
      break;
    }

    // 大Key测试
    if (strcmp(input, "bigkey") == 0) {
      test_big_key();
      continue;
    }

    // 大Value测试
    if (strcmp(input, "bigvalue") == 0) {
      test_big_value();
      continue;
    }

    // 批量操作
    const char *batch_ops[] = {
        "set",  "get",    "mod",   "exist", "del",     "rset",  "rget",
        "rmod", "rexist", "rdel",  "hset",  "hget",    "hmod",  "hexist",
        "hdel", "skset",  "skget", "skmod", "skexist", "skdel", NULL};

    int is_batch = 0;
    for (int i = 0; batch_ops[i]; i++) {
      if (strcmp(input, batch_ops[i]) == 0) {
        is_batch = 1;
        break;
      }
    }

    if (is_batch) {
      int mul;
      printf("请输入倍数（如1、2、3）：");
      if (scanf("%d", &mul) != 1) {
        printf("❌ 倍数必须是整数！\n");
        flush_stdin();
        continue;
      }
      flush_stdin();
      run_batch(input, mul);
      continue;
    }

    // 手动命令
    printf("📤 手动发送：%s\n", input);
    redisReply *reply = redisCommand(ctx, input);
    if (reply) {
      printf("📩 响应：%s\n", reply->str ? reply->str : "NULL");
      freeReplyObject(reply);
    } else {
      printf("❌ 发送失败\n");
    }
  }

  // 释放连接
  redisFree(ctx);
  return 0;
}