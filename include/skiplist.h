#ifndef _SKIPLIST_H
#define _SKIPLIST_H
// skiptable
typedef struct Node {
  char *key;   // 替换int为字符串key
  char *value; // 替换int为字符串value
  struct Node **forward;
} Node;

typedef struct SkipList {
  int level;     // 当前跳表最大层数
  int max_level; // 跳表最大支持层数
  Node *header;  // 头节点
  int count;     // 已存储的 kv 对数量
} SkipList;
typedef SkipList kvs_skiplist_t;

// skiplist 5+2
int skiplist_creat(kvs_skiplist_t *inst);
void skiplist_destory(kvs_skiplist_t *inst);
int skiplist_set(kvs_skiplist_t *inst, char *key, char *value);
char *skiplist_get(kvs_skiplist_t *inst, char *key);
int skiplist_del(kvs_skiplist_t *inst, char *key);
int skiplist_mod(kvs_skiplist_t *inst, char *key, char *value);
int skiplist_exist(kvs_skiplist_t *inst, char *key);
#endif