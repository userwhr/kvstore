#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct pack {
  uint32_t data_length;
  char *data;
} pack_t;

// 给字符串添加4字节的头,并且修改字符串src=4+src
char *add_length_prefix(char *src);
// 解析头
int parse_head(const char *str, uint32_t *out_len);
pack_t *parse_pack(const char *str);

void free_pack_data(pack_t *pack);
void malloc_pack_data(pack_t *pack, size_t datalen);
int checkLastTwoCharsIsCRLF(char *str);
int removeLastTwoChars(char *str);
void free_pack(pack_t *pack);