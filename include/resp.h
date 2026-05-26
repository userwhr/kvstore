#include "kvstore.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _RESP_
#define _RESP_
typedef enum {
  STORE_ARRAY,
  STORE_RBTREE,
  STORE_HASH,
  STORE_SKIPLIST
} StoreType;
// 给字符串添加4字节的头,并且修改字符串src=4+src
char *add_length_prefix(char *src);
// 解析头
int parse_head(const char *str, uint32_t *out_len);
int checkLastTwoCharsIsCRLF(char *str);
int removeLastTwoChars(char *str);
int parseRespKvOnePass(const char *buf, int bufLen, char **outCmd,
                       char **params, int *paramCnt, StoreType *type);
void tokenPoolInit(void);
void tokenPoolFreeAll(char *cmd, char **params, int paramCnt);
#endif