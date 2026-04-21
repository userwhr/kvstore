#ifndef _ARRAY_
#define _ARRAY_
// array
#define KVS_ARRAY_INIT_CAPACITY 1024 // 初始容量（替代原1024定长）
#define ENABLE_KEY_POINTER 1

typedef struct kvs_array_item {
  char *key;
  char *value;
} kvs_array_item_t;

typedef struct kvs_array_s {
  kvs_array_item_t *table;
  int idx;
  int total;    // 已存储的kv对数量
  int capacity; // 数组当前容量（动态扩容核心）
} kvs_array_t;

// 5+2
int kvs_array_creat(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value);
char *kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);

// end array
#endif