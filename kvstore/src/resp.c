#include "resp.h"
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
/**
 * @brief 在字符串末尾追加\r\n（回车+换行）
 * @param src 原始字符串（不可为NULL）
 * @return 成功返回新分配的字符串（需手动free），失败返回NULL
 * @note 1. 原始字符串不会被修改；2.
 * 返回的新字符串需调用者手动释放，避免内存泄漏
 */
char *append_rn(char *src) {
  // 1. 基础参数校验：防止空指针
  if (src == NULL) {
    fprintf(stderr, "错误：输入字符串为NULL\n");
    return NULL;
  }

  size_t src_len = strlen(src);
  size_t new_len = src_len + 2 + 1;

  char *new_str = (char *)kvs_malloc(new_len);
  if (new_str == NULL) {
    perror("malloc失败");
    return NULL;
  }
  // 4. 拷贝原字符串 + 追加\r\n + 补结束符
  strcpy(new_str, src);        // 拷贝原字符串
  new_str[src_len] = '\r';     // 追加\r
  new_str[src_len + 1] = '\n'; // 追加\n
  new_str[src_len + 2] = '\0'; // 补字符串结束符
  kvs_free(src);
  src = new_str;
  return new_str;
}
char *add_length_prefix(char *src) {
  // 1. 基础参数校验：防止空指针
  if (src == NULL) {
    fprintf(stderr, "错误：输入字符串为NULL\n");
    return NULL;
  }

  // 2. 计算长度：原字符串长度（不含\0） + 4字节前缀
  uint32_t src_len =
      (uint32_t)strlen(src); // 原字符串长度（最多支持4GB，满足绝大多数场景）
  size_t new_total_len =
      4 + src_len; // 新字符串总长度（无\0，因为前缀+内容是二进制结构）

  // 3. 分配新内存：存储4字节长度 + 原字符串内容
  char *new_str = (char *)kvs_malloc(new_total_len);
  if (new_str == NULL) {
    perror("malloc失败");
    return NULL;
  }

  // 4. 写入4字节长度前缀（小端模式：低字节存低地址，兼容x86架构）
  // 方式1：逐字节写入（最通用，不依赖系统大小端）
  new_str[0] = (src_len) & 0xFF;       // 第1字节：长度的低8位
  new_str[1] = (src_len >> 8) & 0xFF;  // 第2字节：长度的中8位
  new_str[2] = (src_len >> 16) & 0xFF; // 第3字节：长度的高8位
  new_str[3] = (src_len >> 24) & 0xFF; // 第4字节：长度的最高8位

  // 方式2：直接拷贝整型（依赖系统大小端，可选）
  // memcpy(new_str, &src_len, 4);

  // 5. 拷贝原字符串内容到长度前缀之后
  memcpy(new_str + 4, src, src_len);
  kvs_free(src);
  src = new_str;
  return new_str;
}
/**
 * @brief 解析4字节小端长度前缀
 * @param str 输入字符串（需至少4字节）
 * @param len_out 输出参数：解析出的长度值（成功时有效）
 * @return 0=成功，-1=输入为空指针，-2=输入长度不足4字节
 */

int removeLastTwoChars(char *str) {
  // 1. 校验输入合法性：空指针 或 字符串长度<2 时无法移除最后两位
  if (str == NULL || strlen(str) < 2) {
    return -1;
  }

  // 2. 核心操作：在倒数第二个字符的位置设置 \0，截断字符串
  // 原理：字符串以 \0 为结束标志，设置后后面的字符会被忽略
  size_t str_len = strlen(str);
  str[str_len - 2] = '\0';

  return 1; // 成功移除最后两位
}

// 符合返回1,不符合返回0,参数错误返回-1
int checkLastTwoCharsIsCRLF(char *str) {
  // 1. 处理空指针或字符串长度不足的情况
  if (str == NULL || strlen(str) < 2) {
    return -1; // 表示输入无效（长度不够，无法包含两个字符）
  }

  // 2. 计算字符串末尾位置，检查最后两个字符
  size_t str_len = strlen(str);
  // 最后一个字符是 \n，倒数第二个是 \r
  if (str[str_len - 1] == '\n' && str[str_len - 2] == '\r') {
    return 1; // 符合条件
  } else {
    return 0; // 不符合条件
  }
}

// 【改造版】成功返回0，失败返回-1；解析后的长度存在 out_len 里
int parse_head(const char *str, uint32_t *out_len) {
  // 1. 参数错误：str空 或 out_len空 → 函数执行失败，返回-1
  if (str == NULL || out_len == NULL) {
    fprintf(stderr, "parse_head错误：空指针\n");
    return -1;
  }

  // 2. 小端解析长度（正常解析，包括长度0）
  *out_len = 0;
  *out_len |= (uint8_t)str[0];
  *out_len |= (uint8_t)str[1] << 8;
  *out_len |= (uint8_t)str[2] << 16;
  *out_len |= (uint8_t)str[3] << 24;

  return 0; // 函数执行成功（不管长度是0还是其他值）
}