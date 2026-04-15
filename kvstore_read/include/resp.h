#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// 给字符串添加4字节的头,并且修改字符串src=4+src
char *add_length_prefix(char *src);
// 解析头
int parse_head(const char *str, uint32_t *out_len);
int checkLastTwoCharsIsCRLF(char *str);
int removeLastTwoChars(char *str);