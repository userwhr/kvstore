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

// ====================== 单次解析：同时提取 token 并返回消耗字节数
// 返回值：>0 消耗字节数，0 数据不完整，-1 协议错误，-2 命令不存在
int parseRespKvOnePass(const char *buf, int bufLen, char **outCmd,
                       char **params, int *paramCnt, StoreType *type) {
  const char *p = buf;
  const char *end = buf + bufLen;
  *paramCnt = 0;

  if (p >= end || *p != '*')
    return -1;
  p++;
  if (p >= end) return 0;
  int totalArgs = parseInt((char **)&p);
  if (totalArgs <= 0)
    return -1;
  /* 跳过 \r\n */
  if (p + 2 > end) return 0;
  skipCrlf((char **)&p);

  for (int i = 0; i < totalArgs; i++) {
    if (p >= end || *p != '$')
      return -1;
    p++;
    if (p >= end) return 0;
    int len = parseInt((char **)&p);
    if (len < 0) return -1;
    if (p + 2 > end) return 0;
    skipCrlf((char **)&p);
    if (p + len > end) return 0;  /* 数据不完整 */
    /* 分配 token 并拷贝 */
    char *tbuf = tokenPoolAlloc(len);
    if (tbuf == NULL)
      tbuf = malloc(len + 1);
    memcpy(tbuf, p, len);
    tbuf[len] = '\0';
    if (i == 0)
      *outCmd = tbuf;
    else
      params[(*paramCnt)++] = tbuf;
    p += len;
    if (p + 2 > end) return 0;
    skipCrlf((char **)&p);
  }

  int cmdIdx = matchCommand(*outCmd, type);
  if (cmdIdx == -1)
    return -2;
  return p - buf;
}
