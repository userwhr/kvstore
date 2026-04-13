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
} SkipList;
typedef SkipList kvs_skiplist_t;

// skiplist 5+2
int kvs_skiplist_creat(kvs_skiplist_t *inst);
void kvs_skiplist_destory(kvs_skiplist_t *inst);
int kvs_skiplist_set(kvs_skiplist_t *inst, char *key, char *value);
char *kvs_skiplist_get(kvs_skiplist_t *inst, char *key);
int kvs_skiplist_del(kvs_skiplist_t *inst, char *key);
int kvs_skiplist_mod(kvs_skiplist_t *inst, char *key, char *value);
int kvs_skiplist_exist(kvs_skiplist_t *inst, char *key);
#endif