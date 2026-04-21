#include "array.h"
#include "config.h"
#include "hash.h"
#include "kvstore.h"
#include "rbtree.h"
#include "skiplist.h"
#include <string.h>

extern kvs_array_t *global_array;
extern kvs_rbtree_t *global_rbtree;
extern kvs_hash_t *global_hash;
extern kvs_skiplist_t *global_skiplist;

typedef struct KvInf {
  char *type_name;
  char **key;
  char **value;
  int count;
} KvInf_t;

// 安全字符串拷贝（内部使用）
static char *safe_strdup(const char *str) {
  if (!str)
    return NULL;
  size_t len = strlen(str) + 1;
  char *new_str = (char *)kvs_malloc(len);
  if (new_str) {
    strcpy(new_str, str);
  }
  return new_str;
}

// 函数声明
KvInf_t *traverse_array(void);
KvInf_t *traverse_rbtree(void);
KvInf_t *traverse_hash(void);
KvInf_t *traverse_skiplist(void);
void free_kvinf(KvInf_t *inf);

// 函数定义
KvInf_t *traverse_array(void) {
  if (!g_config.enable_array || !global_array)
    return NULL;
  KvInf_t *inf = (KvInf_t *)kvs_malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "array";
  inf->count = global_array->total;
  if (inf->count == 0) {
    inf->key = NULL;
    inf->value = NULL;
    return inf;
  }
  inf->key = (char **)kvs_malloc(sizeof(char *) * inf->count);
  inf->value = (char **)kvs_malloc(sizeof(char *) * inf->count);
  if (!inf->key || !inf->value) {
    kvs_free(inf->key);
    kvs_free(inf->value);
    kvs_free(inf);
    return NULL;
  }
  for (int i = 0; i < inf->count; i++) {
    inf->key[i] = safe_strdup(global_array->table[i].key);
    inf->value[i] = safe_strdup(global_array->table[i].value);
    if (!inf->key[i] || !inf->value[i]) {
      // 清理已分配的内存
      for (int j = 0; j < i; j++) {
        kvs_free(inf->key[j]);
        kvs_free(inf->value[j]);
      }
      kvs_free(inf->key);
      kvs_free(inf->value);
      kvs_free(inf);
      return NULL;
    }
  }
  return inf;
}

// 红黑树遍历回调
typedef struct {
  char **keys;
  char **values;
  int index;
  int capacity;
} RbtreeCollector;

static void rbtree_collect_callback(const void *key, const void *value,
                                    KvsRbtreeColor color, void *user_data) {
  RbtreeCollector *collector = (RbtreeCollector *)user_data;
  if (collector->index >= collector->capacity) {
    // 扩容
    int new_cap = collector->capacity * 2;
    char **new_keys = (char **)kvs_malloc(sizeof(char *) * new_cap);
    char **new_values = (char **)kvs_malloc(sizeof(char *) * new_cap);
    if (!new_keys || !new_values) {
      kvs_free(new_keys);
      kvs_free(new_values);
      return;
    }
    memcpy(new_keys, collector->keys, sizeof(char *) * collector->capacity);
    memcpy(new_values, collector->values, sizeof(char *) * collector->capacity);
    kvs_free(collector->keys);
    kvs_free(collector->values);
    collector->keys = new_keys;
    collector->values = new_values;
    collector->capacity = new_cap;
  }
  collector->keys[collector->index] = safe_strdup((const char *)key);
  collector->values[collector->index] = safe_strdup((const char *)value);
  collector->index++;
}

KvInf_t *traverse_rbtree(void) {
  if (!g_config.enable_rbtree || !global_rbtree)
    return NULL;
  KvInf_t *inf = (KvInf_t *)kvs_malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "rbtree";
  // 先统计节点数量？这里我们使用动态收集
  RbtreeCollector collector;
  collector.capacity = 16;
  collector.index = 0;
  collector.keys = (char **)kvs_malloc(sizeof(char *) * collector.capacity);
  collector.values = (char **)kvs_malloc(sizeof(char *) * collector.capacity);
  if (!collector.keys || !collector.values) {
    kvs_free(collector.keys);
    kvs_free(collector.values);
    kvs_free(inf);
    return NULL;
  }
  kvs_rbtree_traverse(global_rbtree, rbtree_collect_callback, &collector);
  inf->count = collector.index;
  inf->key = collector.keys;
  inf->value = collector.values;
  // 如果容量大于实际数量，可以收缩
  if (collector.capacity > collector.index) {
    char **new_keys = (char **)kvs_malloc(sizeof(char *) * collector.index);
    char **new_values = (char **)kvs_malloc(sizeof(char *) * collector.index);
    if (new_keys && new_values) {
      memcpy(new_keys, collector.keys, sizeof(char *) * collector.index);
      memcpy(new_values, collector.values, sizeof(char *) * collector.index);
      kvs_free(collector.keys);
      kvs_free(collector.values);
      inf->key = new_keys;
      inf->value = new_values;
    }
  }
  return inf;
}

KvInf_t *traverse_hash(void) {
  if (!g_config.enable_hash || !global_hash)
    return NULL;
  KvInf_t *inf = (KvInf_t *)kvs_malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "hash";
  // 先计算总节点数
  int total = 0;
  for (int i = 0; i < global_hash->max_slots; i++) {
    hashnode_t *node = global_hash->nodes[i];
    while (node) {
      total++;
      node = node->next;
    }
  }
  inf->count = total;
  if (total == 0) {
    inf->key = NULL;
    inf->value = NULL;
    return inf;
  }
  inf->key = (char **)kvs_malloc(sizeof(char *) * total);
  inf->value = (char **)kvs_malloc(sizeof(char *) * total);
  if (!inf->key || !inf->value) {
    kvs_free(inf->key);
    kvs_free(inf->value);
    kvs_free(inf);
    return NULL;
  }
  int idx = 0;
  for (int i = 0; i < global_hash->max_slots; i++) {
    hashnode_t *node = global_hash->nodes[i];
    while (node) {
      inf->key[idx] = safe_strdup(node->key);
      inf->value[idx] = safe_strdup(node->value);
      if (!inf->key[idx] || !inf->value[idx]) {
        // 清理
        for (int j = 0; j < idx; j++) {
          kvs_free(inf->key[j]);
          kvs_free(inf->value[j]);
        }
        kvs_free(inf->key);
        kvs_free(inf->value);
        kvs_free(inf);
        return NULL;
      }
      idx++;
      node = node->next;
    }
  }
  return inf;
}

KvInf_t *traverse_skiplist(void) {
  if (!g_config.enable_skiplist || !global_skiplist)
    return NULL;
  KvInf_t *inf = (KvInf_t *)kvs_malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "skiplist";
  // 计算节点数
  int total = 0;
  Node *current = global_skiplist->header->forward[0];
  while (current) {
    total++;
    current = current->forward[0];
  }
  inf->count = total;
  if (total == 0) {
    inf->key = NULL;
    inf->value = NULL;
    return inf;
  }
  inf->key = (char **)kvs_malloc(sizeof(char *) * total);
  inf->value = (char **)kvs_malloc(sizeof(char *) * total);
  if (!inf->key || !inf->value) {
    kvs_free(inf->key);
    kvs_free(inf->value);
    kvs_free(inf);
    return NULL;
  }
  current = global_skiplist->header->forward[0];
  int idx = 0;
  while (current) {
    inf->key[idx] = safe_strdup(current->key);
    inf->value[idx] = safe_strdup(current->value);
    if (!inf->key[idx] || !inf->value[idx]) {
      for (int j = 0; j < idx; j++) {
        kvs_free(inf->key[j]);
        kvs_free(inf->value[j]);
      }
      kvs_free(inf->key);
      kvs_free(inf->value);
      kvs_free(inf);
      return NULL;
    }
    idx++;
    current = current->forward[0];
  }
  return inf;
}

void free_kvinf(KvInf_t *inf) {
  if (!inf)
    return;
  for (int i = 0; i < inf->count; i++) {
    kvs_free(inf->key[i]);
    kvs_free(inf->value[i]);
  }
  kvs_free(inf->key);
  kvs_free(inf->value);
  kvs_free(inf);
}

// 遍历所有数据结构并落盘
int dump_all_to_disk(void) {
  FILE *fp = fopen("./log/full_log.txt", "w");
  if (!fp) {
    // 尝试创建目录
    system("mkdir -p ./log");
    fp = fopen("./log/full_log.txt", "w");
    if (!fp) {
      perror("Failed to open log file");
      return -1;
    }
  }

  // 遍历数组
  if (g_config.enable_array) {
    KvInf_t *array_inf = traverse_array();
    if (array_inf) {
      fprintf(fp, "array %d\r\n", array_inf->count);
      for (int i = 0; i < array_inf->count; i++) {
        fprintf(fp, "%s %s\r\n", array_inf->key[i], array_inf->value[i]);
      }
      free_kvinf(array_inf);
    }
  }

  // 遍历红黑树
  if (g_config.enable_rbtree) {
    KvInf_t *rbtree_inf = traverse_rbtree();
    if (rbtree_inf) {
      fprintf(fp, "rbtree %d\r\n", rbtree_inf->count);
      for (int i = 0; i < rbtree_inf->count; i++) {
        fprintf(fp, "%s %s\r\n", rbtree_inf->key[i], rbtree_inf->value[i]);
      }
      free_kvinf(rbtree_inf);
    }
  }

  // 遍历哈希表
  if (g_config.enable_hash) {
    KvInf_t *hash_inf = traverse_hash();
    if (hash_inf) {
      fprintf(fp, "hash %d\r\n", hash_inf->count);
      for (int i = 0; i < hash_inf->count; i++) {
        fprintf(fp, "%s %s\r\n", hash_inf->key[i], hash_inf->value[i]);
      }
      free_kvinf(hash_inf);
    }
  }

  // 遍历跳表
  if (g_config.enable_skiplist) {
    KvInf_t *skiplist_inf = traverse_skiplist();
    if (skiplist_inf) {
      fprintf(fp, "skiplist %d\r\n", skiplist_inf->count);
      for (int i = 0; i < skiplist_inf->count; i++) {
        fprintf(fp, "%s %s\r\n", skiplist_inf->key[i], skiplist_inf->value[i]);
      }
      free_kvinf(skiplist_inf);
    }
  }
  fclose(fp);
  return 0;
}
