#include "resp.h"
#include "kvstore.h"
#include <ctype.h>
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
extern char *command[];
extern int COMMAND_COUNT;
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

    // ====================== 核心修复 ======================
    // 🔥 不修改原始resp！分配新内存，拷贝数据，手动加\0
    char *buf = malloc(len + 1);
    memcpy(buf, p, len);
    buf[len] = '\0'; // 只给副本加\0，原始rbuffer毫发无损
    // ======================================================

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
int getOneRespLen(char *buf) {
  if (buf == NULL || *buf != '*') {
    return 0; // 必须以 RESP 数组 * 开头
  }

  char *p = buf;
  // 1. 解析数组长度 *N\r\n
  p++;
  int argCnt = parseInt(&p);
  if (argCnt <= 0)
    return 0;
  skipCrlf(&p);

  // 2. 循环解析所有参数（$len\r\nvalue\r\n）
  for (int i = 0; i < argCnt; i++) {
    if (*p != '$')
      return 0; // 必须是批量字符串
    p++;
    // 解析字符串长度
    int strLen = parseInt(&p);
    if (strLen < 0)
      return 0;
    skipCrlf(&p);
    // 跳过字符串内容
    p += strLen;
    skipCrlf(&p);
  }

  // 3. 返回总长度（指针最终位置 - 起始位置 = 本条RESP总字节数）
  return p - buf;
}
