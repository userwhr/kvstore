#ifndef _TRAVERSAL_H_
#define _TRAVERSAL_H_

#include "config.h"

typedef struct KvInf {
  char *type_name;
  char **key;
  char **value;
  int count;
} KvInf_t;

// 遍历函数声明
KvInf_t *traverse_array(void);
KvInf_t *traverse_rbtree(void);
KvInf_t *traverse_hash(void);
KvInf_t *traverse_skiplist(void);

// 释放KvInf_t内存
void free_kvinf(KvInf_t *inf);

// 遍历所有数据结构并落盘
int dump_all_to_disk(void);

#endif // _TRAVERSAL_H_