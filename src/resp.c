#include "resp.h"
#include "kvstore.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
extern char *command[];
extern int COMMAND_COUNT;

/* ===== Token 内存池：连续块 + 栈式索引，无锁（reactor 单线程） ===== */
#define TOKEN_POOL_SIZE 4096
#define TOKEN_BUF_SIZE  128

static char *pool_base = NULL;                 /* 整块内存起始地址 */
static char *token_free_stack[TOKEN_POOL_SIZE]; /* 空闲槽位指针栈 */
static int   token_free_top = 0;

void tokenPoolInit(void) {
  pool_base = malloc((size_t)TOKEN_POOL_SIZE * TOKEN_BUF_SIZE);
  for (int i = 0; i < TOKEN_POOL_SIZE; i++)
    token_free_stack[i] = pool_base + (size_t)i * TOKEN_BUF_SIZE;
  token_free_top = TOKEN_POOL_SIZE;
}

static char *tokenPoolAlloc(int len) {
  if (len >= TOKEN_BUF_SIZE || token_free_top <= 0)
    return NULL;                          /* 超大 token 或池耗尽，走 malloc */
  return token_free_stack[--token_free_top];
}

void tokenPoolFreeAll(char *cmd, char **params, int paramCnt) {
  if (cmd) {
    if (cmd >= pool_base && cmd < pool_base + (size_t)TOKEN_POOL_SIZE * TOKEN_BUF_SIZE)
      token_free_stack[token_free_top++] = cmd;
    else
      free(cmd);
  }
  for (int i = 0; i < paramCnt; i++) {
    if (params[i]) {
      if (params[i] >= pool_base && params[i] < pool_base + (size_t)TOKEN_POOL_SIZE * TOKEN_BUF_SIZE)
        token_free_stack[token_free_top++] = params[i];
      else
        free(params[i]);
    }
  }
}
// ====================== RESP 工具函数（小驼峰）======================
// 解析RESP中的数字（*和$后的长度）
static int parseInt(char **p) {
  int num = 0;
  while (isdigit(**p)) {
    num = num * 10 + (**p - '0');
    (*p)++;
  }
  return num;
}

// 跳过 \r\n 或 \0\n（当\r被替换为\0时）
static void skipCrlf(char **p) {
  if (**p == '\r' || **p == '\0')
    (*p)++;
  if (**p == '\n')
    (*p)++;
}

// ====================== 命令匹配函数（小驼峰）======================
// 输入命令字符串，返回命令索引、存储类型
int matchCommand(const char *cmd, StoreType *type) {
  // 计算命令长度（直到\r\n或结束符）
  int cmd_len = 0;
  while (cmd[cmd_len] != '\0' && cmd[cmd_len] != '\r' && cmd[cmd_len] != '\n') {
    cmd_len++;
  }

  for (int i = 0; i < COMMAND_COUNT; i++) {
    if (strncasecmp(cmd, command[i], cmd_len) == 0 &&
        command[i][cmd_len] == '\0') {
      // 判定存储类型
      if (i < 5)
        *type = STORE_ARRAY;
      else if (i < 10)
        *type = STORE_RBTREE;
      else if (i < 15)
        *type = STORE_HASH;
      else
        *type = STORE_SKIPLIST;
      return i;
    }
  }
  return -1; // 命令不存在
}

// 修复后的解析函数：只读rbuffer，不修改！复制数据+加\0，msg完全正常
int parseRespKv(char *resp, char **outCmd, char **params, int *paramCnt,
                StoreType *type) {
  char *p = resp;
  *paramCnt = 0;

  if (*p != '*')
    return -1;
  p++;
  int totalArgs = parseInt(&p);
  skipCrlf(&p);

  for (int i = 0; i < totalArgs; i++) {
    if (*p != '$')
      return -2;
    p++;
    int len = parseInt(&p);
    skipCrlf(&p);

    char *buf = tokenPoolAlloc(len);
    if (buf == NULL)
      buf = malloc(len + 1);   /* 超大 token 回退到 malloc */
    memcpy(buf, p, len);
    buf[len] = '\0';

    if (i == 0) {
      *outCmd = buf; // 命令：带\0的副本
    } else {
      params[*paramCnt] = buf; // 参数：带\0的副本
      (*paramCnt)++;
    }

    p += len;
    skipCrlf(&p);
  }

  int cmdIdx = matchCommand(*outCmd, type);
  if (cmdIdx == -1)
    return -3;

  return cmdIdx;
}
// ====================== 核心：计算单条RESP命令的总长度（解决粘包切割）
// ======================
// 功能：解析一段缓冲区，返回【第一条完整RESP命令】占用的总字节数
// 返回值：>0 表示有效长度，=0 表示数据不完整/协议非法
int getOneRespLen(char *buf, int bufLen) {
  if (buf == NULL || bufLen <= 0 || *buf != '*') {
    return 0; // 必须以 RESP 数组 * 开头
  }

  char *p = buf;
  char *end = buf + bufLen;  // 边界指针

  // 1. 解析数组长度 *N\r\n
  p++;
  if (p >= end) return 0;
  int argCnt = parseInt(&p);
  if (argCnt <= 0)
    return 0;
  if (p + 2 > end) return 0;  // 需要至少 \r\n
  skipCrlf(&p);

  // 2. 循环解析所有参数（$len\r\nvalue\r\n）
  for (int i = 0; i < argCnt; i++) {
    if (p >= end || *p != '$')
      return 0; // 必须是批量字符串
    p++;
    if (p >= end) return 0;
    // 解析字符串长度
    int strLen = parseInt(&p);
    if (strLen < 0)
      return 0;
    if (p + 2 > end) return 0;  // 需要至少 \r\n
    skipCrlf(&p);
    // 跳过字符串内容
    if (p + strLen > end) return 0;  // 数据不完整
    p += strLen;
    if (p + 2 > end) return 0;  // 需要至少 \r\n
    skipCrlf(&p);
  }

  // 3. 返回总长度（指针最终位置 - 起始位置 = 本条RESP总字节数）
  return p - buf;
}
