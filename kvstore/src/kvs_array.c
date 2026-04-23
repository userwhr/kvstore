#include "kvstore.h"
#include <stdio.h>
#include <string.h>

// 内部扩容函数
static int kvs_array_expand(kvs_array_t *inst) {
  if (!inst || inst->capacity == 0)
    return -1;

  int new_capacity = inst->capacity * 2;
  kvs_array_item_t *new_table =
      kvs_malloc(new_capacity * sizeof(kvs_array_item_t));
  if (!new_table) {
    printf("扩容内存分配失败\n");
    return -2;
  }

  // 仅拷贝有效元素（紧凑存储，优化性能）
  memcpy(new_table, inst->table, inst->total * sizeof(kvs_array_item_t));

  // 释放旧数组，更新信息
  kvs_free(inst->table);
  inst->table = new_table;
  inst->capacity = new_capacity;

  return 0;
}

int kvs_array_creat(kvs_array_t *inst) {
  if (!inst || inst->table)
    return -1;

  inst->table = kvs_malloc(KVS_ARRAY_INIT_CAPACITY * sizeof(kvs_array_item_t));
  if (!inst->table)
    return -2;

  memset(inst->table, 0, KVS_ARRAY_INIT_CAPACITY * sizeof(kvs_array_item_t));
  inst->idx = 0;
  inst->total = 0;
  inst->capacity = KVS_ARRAY_INIT_CAPACITY;
  return 0;
}

void kvs_array_destory(kvs_array_t *inst) {
  if (!inst || !inst->table)
    return;

  // 遍历所有有效元素（紧凑存储，安全释放）
  for (int i = 0; i < inst->total; i++) {
    kvs_free(inst->table[i].key);
    kvs_free(inst->table[i].value);
  }
  kvs_free(inst->table);
  inst->table = NULL;
  inst->total = 0;
  inst->capacity = 0;
}

/*
@return <0 error =0 success >0 exist
*/
int kvs_array_set(kvs_array_t *inst, char *key, char *value) {
  if (inst == NULL || key == NULL || value == NULL)
    return -1;

  // 检查key是否已存在
  if (kvs_array_get(inst, key) != NULL)
    return 1;

  // 容量不足扩容
  if (inst->total >= inst->capacity) {
    int expand_ret = kvs_array_expand(inst);
    if (expand_ret != 0)
      return expand_ret;
  }

  // 分配内存
  char *kcopy = kvs_malloc(strlen(key) + 1);
  char *kvalue = kvs_malloc(strlen(value) + 1);
  if (!kcopy || !kvalue) {
    printf("内存分配失败\n");
    kvs_free(kcopy);
    kvs_free(kvalue);
    return -2;
  }
  strcpy(kcopy, key);
  strcpy(kvalue, value);

  // 🔥 修复核心：紧凑存储，直接插入到 total 位置（无空洞）
  inst->table[inst->total].key = kcopy;
  inst->table[inst->total].value = kvalue;
  inst->total++;

  return 0;
}

/*
@return null:找不到 !null:找到了
*/
char *kvs_array_get(kvs_array_t *inst, char *key) {
  if (inst == NULL || key == NULL)
    return NULL;

  // 遍历所有有效元素（紧凑连续）
  for (int i = 0; i < inst->total; i++) {
    if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0)
      return inst->table[i].value;
  }
  return NULL;
}

/*
@return -1 error =0 delete success
*/
int kvs_array_del(kvs_array_t *inst, char *key) {
  if (inst == NULL || key == NULL)
    return -1;

  for (int i = 0; i < inst->total; i++) {
    if (!inst->table[i].key)
      continue;

    if (strcmp(inst->table[i].key, key) == 0) {
      // 释放内存
      kvs_free(inst->table[i].key);
      kvs_free(inst->table[i].value);

      // 🔥 保持紧凑：后续元素前移
      int move_num = inst->total - i - 1;
      if (move_num > 0) {
        memmove(&inst->table[i], &inst->table[i + 1],
                move_num * sizeof(kvs_array_item_t));
      }

      // 清空最后一个位置
      inst->table[inst->total - 1].key = NULL;
      inst->table[inst->total - 1].value = NULL;
      inst->total--;

      // 维护索引
      if (inst->idx > inst->total)
        inst->idx = inst->total;

      return 0;
    }
  }
  return 1;
}

/*
@return: <0 error 0 success >0:no exist
*/
int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {
  if (inst == NULL || key == NULL || value == NULL)
    return -1;

  char *old_value = kvs_array_get(inst, key);
  if (!old_value)
    return 1;

  char *new_val = kvs_malloc(strlen(value) + 1);
  if (!new_val) {
    printf("value内存分配失败\n");
    return -2;
  }
  strcpy(new_val, value);

  // 找到并替换
  for (int i = 0; i < inst->total; i++) {
    if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
      kvs_free(inst->table[i].value);
      inst->table[i].value = new_val;
      return 0;
    }
  }

  kvs_free(new_val);
  return -1;
}

/*
@return -1:参数错误 0:存在 1:不存在
*/
int kvs_array_exist(kvs_array_t *inst, char *key) {
  if (!inst || !key)
    return -1;
  return kvs_array_get(inst, key) == NULL ? 1 : 0;
}