#ifndef _ARRAY_
#define _ARRAY_
// array
#define KVS_ARRAY_INIT_CAPACITY 512 // 初始容量（替代原1024定长）
#define ARRAY_INDEX_SIZE 1024       // 哈希索引桶数
#define ENABLE_KEY_POINTER 1

typedef struct kvs_array_item {
  char *key;
  char *value;
} kvs_array_item_t;

// 哈希索引节点（内部使用，key指向数组中的同一个指针，不复制）
typedef struct array_index_node {
  char *key;                    // 指向 table[].key，不拥有内存
  int idx;                      // 在 table 中的下标
  struct array_index_node *next;
} array_index_node_t;

typedef struct kvs_array_s {
  kvs_array_item_t *table;
  int idx;
  int total;    // 已存储的kv对数量
  int capacity; // 数组当前容量（动态扩容核心）
  array_index_node_t **index;   // 哈希索引桶
  int index_size;               // 索引桶数
} kvs_array_t;

// 5+2
int array_creat(kvs_array_t *inst);
void array_destory(kvs_array_t *inst);
int array_set(kvs_array_t *inst, char *key, char *value);
char *array_get(kvs_array_t *inst, char *key);
int array_del(kvs_array_t *inst, char *key);
int array_mod(kvs_array_t *inst, char *key, char *value);
int array_exist(kvs_array_t *inst, char *key);

// end array
#endif