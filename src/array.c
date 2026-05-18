#include "array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FNV-1a 哈希函数
static unsigned int _array_hash(const char *key, int size) {
  if (!key) return 0;
  unsigned int h = 2166136261u;
  const unsigned char *p = (const unsigned char *)key;
  while (*p) {
    h ^= *p++;
    h *= 16777619u;
  }
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  return h % size;
}

// 索引操作：查找，返回节点指针
static array_index_node_t *_index_find(array_index_node_t **index, int size,
                                       const char *key) {
  unsigned int h = _array_hash(key, size);
  array_index_node_t *node = index[h];
  while (node) {
    if (strcmp(node->key, key) == 0)
      return node;
    node = node->next;
  }
  return NULL;
}

// 索引操作：添加
static void _index_add(array_index_node_t **index, int size,
                        char *key, int idx) {
  unsigned int h = _array_hash(key, size);
  array_index_node_t *node = malloc(sizeof(array_index_node_t));
  if (!node) return;
  node->key = key;   // 指向数组中的 key，不复制
  node->idx = idx;
  node->next = index[h];
  index[h] = node;
}

// 索引操作：删除
static void _index_remove(array_index_node_t **index, int size,
                           const char *key) {
  unsigned int h = _array_hash(key, size);
  array_index_node_t *prev = NULL;
  array_index_node_t *cur = index[h];
  while (cur) {
    if (strcmp(cur->key, key) == 0) {
      if (prev)
        prev->next = cur->next;
      else
        index[h] = cur->next;
      free(cur);
      return;
    }
    prev = cur;
    cur = cur->next;
  }
}

// 索引操作：更新某个 key 的下标（删除移位后调用）
static void _index_update(array_index_node_t **index, int size,
                           const char *key, int new_idx) {
  array_index_node_t *node = _index_find(index, size, key);
  if (node)
    node->idx = new_idx;
}

// 内部扩容函数
static int array_expand(kvs_array_t *inst) {
  if (!inst || inst->capacity == 0)
    return -1;

  int new_capacity = inst->capacity * 2;
  kvs_array_item_t *new_table =
      malloc(new_capacity * sizeof(kvs_array_item_t));
  if (!new_table) {
    printf("扩容内存分配失败\n");
    return -2;
  }

  memcpy(new_table, inst->table, inst->total * sizeof(kvs_array_item_t));
  free(inst->table);
  inst->table = new_table;
  inst->capacity = new_capacity;
  return 0;
}

int array_creat(kvs_array_t *inst) {
  if (!inst || inst->table)
    return -1;

  inst->table = malloc(KVS_ARRAY_INIT_CAPACITY * sizeof(kvs_array_item_t));
  if (!inst->table)
    return -2;

  memset(inst->table, 0, KVS_ARRAY_INIT_CAPACITY * sizeof(kvs_array_item_t));
  inst->idx = 0;
  inst->total = 0;
  inst->capacity = KVS_ARRAY_INIT_CAPACITY;

  // 初始化哈希索引
  inst->index_size = ARRAY_INDEX_SIZE;
  inst->index = malloc(sizeof(array_index_node_t *) * inst->index_size);
  if (!inst->index) {
    free(inst->table);
    inst->table = NULL;
    return -2;
  }
  memset(inst->index, 0, sizeof(array_index_node_t *) * inst->index_size);

  return 0;
}

void array_destory(kvs_array_t *inst) {
  if (!inst || !inst->table)
    return;

  for (int i = 0; i < inst->total; i++) {
    free(inst->table[i].key);
    free(inst->table[i].value);
  }
  free(inst->table);
  inst->table = NULL;
  inst->total = 0;
  inst->capacity = 0;

  // 释放索引
  if (inst->index) {
    for (int i = 0; i < inst->index_size; i++) {
      array_index_node_t *node = inst->index[i];
      while (node) {
        array_index_node_t *tmp = node;
        node = node->next;
        free(tmp);
      }
    }
    free(inst->index);
    inst->index = NULL;
  }
}

/*
@return <0 error =0 success >0 exist
*/
int array_set(kvs_array_t *inst, char *key, char *value) {
  if (inst == NULL || key == NULL || value == NULL)
    return -1;

  // O(1) 索引查重
  if (_index_find(inst->index, inst->index_size, key))
    return 1;

  // 容量不足扩容
  if (inst->total >= inst->capacity) {
    int expand_ret = array_expand(inst);
    if (expand_ret != 0)
      return expand_ret;
  }

  // 分配内存
  char *kcopy = malloc(strlen(key) + 1);
  char *kvalue = malloc(strlen(value) + 1);
  if (!kcopy || !kvalue) {
    free(kcopy);
    free(kvalue);
    return -2;
  }
  strcpy(kcopy, key);
  strcpy(kvalue, value);

  inst->table[inst->total].key = kcopy;
  inst->table[inst->total].value = kvalue;

  // 加入索引
  _index_add(inst->index, inst->index_size, kcopy, inst->total);

  inst->total++;
  return 0;
}

/*
@return null:找不到 !null:找到了
*/
char *array_get(kvs_array_t *inst, char *key) {
  if (inst == NULL || key == NULL)
    return NULL;

  // O(1) 索引查找
  array_index_node_t *node = _index_find(inst->index, inst->index_size, key);
  if (node && node->idx >= 0 && node->idx < inst->total)
    return inst->table[node->idx].value;

  return NULL;
}

/*
@return -1 error =0 delete success
*/
int array_del(kvs_array_t *inst, char *key) {
  if (inst == NULL || key == NULL)
    return -1;

  // O(1) 定位
  array_index_node_t *node = _index_find(inst->index, inst->index_size, key);
  if (!node)
    return 1;

  int i = node->idx;
  if (i < 0 || i >= inst->total)
    return -1;

  // 从索引中移除
  _index_remove(inst->index, inst->index_size, key);

  // 释放内存
  free(inst->table[i].key);
  free(inst->table[i].value);

  // 前移元素
  int move_num = inst->total - i - 1;
  if (move_num > 0) {
    memmove(&inst->table[i], &inst->table[i + 1],
            move_num * sizeof(kvs_array_item_t));

    // 更新被移位元素的索引下标
    for (int j = i; j < inst->total - 1; j++) {
      _index_update(inst->index, inst->index_size, inst->table[j].key, j);
    }
  }

  inst->table[inst->total - 1].key = NULL;
  inst->table[inst->total - 1].value = NULL;
  inst->total--;

  if (inst->idx > inst->total)
    inst->idx = inst->total;

  return 0;
}

/*
@return: <0 error 0 success >0:no exist
*/
int array_mod(kvs_array_t *inst, char *key, char *value) {
  if (inst == NULL || key == NULL || value == NULL)
    return -1;

  // O(1) 定位
  array_index_node_t *node = _index_find(inst->index, inst->index_size, key);
  if (!node)
    return 1;

  int i = node->idx;
  if (i < 0 || i >= inst->total)
    return -1;

  char *new_val = malloc(strlen(value) + 1);
  if (!new_val)
    return -2;
  strcpy(new_val, value);

  free(inst->table[i].value);
  inst->table[i].value = new_val;
  return 0;
}

/*
@return -1:参数错误 0:存在 1:不存在
*/
int array_exist(kvs_array_t *inst, char *key) {
  if (!inst || !key)
    return -1;
  return _index_find(inst->index, inst->index_size, key) ? 0 : 1;
}
