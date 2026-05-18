#include "kvstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_LEVEL 16 // 最大层数
#define P 0.5        // 随机层数的概率因子

// 随机生成节点层数
static int random_level() {
  int level = 1;
  srand((unsigned)time(NULL));
  while (rand() < RAND_MAX * P && level < MAX_LEVEL) {
    level++;
  }
  return level;
}

// 创建新节点
static Node *create_node(const char *key, const char *value, int level) {
  Node *node = (Node *)malloc(sizeof(Node));
  if (!node)
    return NULL;

  // 字符串深拷贝，避免外部指针失效
  node->key = (char *)malloc(strlen(key) + 1);
  node->value = (char *)malloc(strlen(value) + 1);
  if (!node->key || !node->value) {
    free(node->key);
    free(node->value);
    free(node);
    return NULL;
  }
  strcpy(node->key, key);
  strcpy(node->value, value);

  // 分配forward数组
  node->forward = (Node **)malloc(sizeof(Node *) * level);
  if (!node->forward) {
    free(node->key);
    free(node->value);
    free(node);
    return NULL;
  }
  memset(node->forward, 0, sizeof(Node *) * level);
  return node;
}

// 1. 创建跳表（对应kvs_rbtree_creat）
int skiplist_creat(kvs_skiplist_t *inst) {
  if (!inst)
    return -1;

  // 初始化跳表结构体
  inst->max_level = MAX_LEVEL;
  inst->level = 1;

  // 创建头节点（key/value为空）
  inst->header = create_node("", "", MAX_LEVEL);
  if (!inst->header)
    return -1;

  return 0;
}

// 释放单个节点内存
static void free_node(Node *node) {
  if (node) {
    free(node->key);
    free(node->value);
    free(node->forward);
    free(node);
  }
}

// 2. 销毁跳表（对应kvs_rbtree_destory）
void skiplist_destory(kvs_skiplist_t *inst) {
  if (!inst)
    return;

  // 遍历跳表所有节点并释放
  Node *current = inst->header->forward[0];
  Node *next = NULL;
  while (current) {
    next = current->forward[0];
    free_node(current);
    current = next;
  }

  // 释放头节点
  free_node(inst->header);
  // 清空结构体
  memset(inst, 0, sizeof(kvs_skiplist_t));
}

// 3. 设置键值对（对应kvs_rbtree_set）
int skiplist_set(kvs_skiplist_t *inst, char *key, char *value) {
  if (!inst || !key || strlen(key) == 0 || !value)
    return -1;

  Node *update[MAX_LEVEL] = {0}; // 记录每层需要更新的节点
  Node *current = inst->header;

  // 从最高层向下查找，记录update节点
  for (int i = inst->level - 1; i >= 0; i--) {
    while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
      current = current->forward[i];
    }
    update[i] = current;
  }

  // 检查key是否已存在，存在则更新value
  current = current->forward[0];
  if (current && strcmp(current->key, key) == 0) {
    free(current->value); // 释放旧value
    current->value = (char *)malloc(strlen(value) + 1);
    if (!current->value)
      return -1;
    strcpy(current->value, value);
    return 0;
  }

  // 不存在则创建新节点
  int new_level = random_level();
  Node *new_node = create_node(key, value, new_level);
  if (!new_node)
    return -1;

  // 如果新节点层数超过当前跳表层数，补充update数组
  if (new_level > inst->level) {
    for (int i = inst->level; i < new_level; i++) {
      update[i] = inst->header;
    }
    inst->level = new_level;
  }

  // 插入新节点到各层
  for (int i = 0; i < new_level; i++) {
    new_node->forward[i] = update[i]->forward[i];
    update[i]->forward[i] = new_node;
  }

  return 0;
}

// 4. 获取键对应的值（对应kvs_rbtree_get）
char *skiplist_get(kvs_skiplist_t *inst, char *key) {
  if (!inst || !key || strlen(key) == 0)
    return NULL;

  Node *current = inst->header;
  // 从最高层向下查找
  for (int i = inst->level - 1; i >= 0; i--) {
    while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
      current = current->forward[i];
    }
  }

  current = current->forward[0];
  // 找到则返回value的拷贝（避免外部修改/释放导致问题）
  if (current && strcmp(current->key, key) == 0) {
    char *val = (char *)malloc(strlen(current->value) + 1);
    strcpy(val, current->value);
    return val;
  }

  return NULL;
}

// 5. 删除键值对（对应kvs_rbtree_del）
int skiplist_del(kvs_skiplist_t *inst, char *key) {
  if (!inst || !key || strlen(key) == 0)
    return -1;

  Node *update[MAX_LEVEL] = {0};
  Node *current = inst->header;

  // 从最高层向下查找，记录update节点
  for (int i = inst->level - 1; i >= 0; i--) {
    while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
      current = current->forward[i];
    }
    update[i] = current;
  }

  // 检查key是否存在
  current = current->forward[0];
  if (!current || strcmp(current->key, key) != 0) {
    return -1; // 键不存在
  }

  // 从各层移除当前节点
  for (int i = 0; i < inst->level; i++) {
    if (update[i]->forward[i] != current) {
      break;
    }
    update[i]->forward[i] = current->forward[i];
  }

  // 释放节点内存
  free_node(current);

  // 更新跳表的最大层数（如果需要）
  while (inst->level > 1 && inst->header->forward[inst->level - 1] == NULL) {
    inst->level--;
  }

  return 0;
}

// 6. 修改键对应的值（对应kvs_rbtree_mod）
// 返回值：
// -1：参数错误（inst/key/value为空，或key是空字符串）
//  1：要修改的key不存在（新增自定义错误码，区分两种失败场景）
//  0：修改成功（仅更新已有key的value，不新增）
int skiplist_mod(kvs_skiplist_t *inst, char *key, char *value) {
  // 1. 前置参数校验：参数非法返回-1
  if (!inst || !key || strlen(key) == 0 || !value)
    return -1;

  Node *current = inst->header;

  // 2. 从最高层向下查找key的位置（和set函数的查找逻辑一致）
  for (int i = inst->level - 1; i >= 0; i--) {
    while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
      current = current->forward[i];
    }
  }

  // 3. 定位到最底层的节点，检查key是否存在
  current = current->forward[0];
  if (!current || strcmp(current->key, key) != 0) {
    return 1; // key不存在，返回1（区分参数错误的-1）
  }

  // 4. key存在，执行value更新（和set函数的更新逻辑一致）
  free(current->value); // 释放旧value内存
  current->value = (char *)malloc(strlen(value) + 1);
  if (!current->value) {
    // 内存分配失败：这里建议返回新的错误码（比如-3），也可降级返回-1（参数/资源类错误）
    // 按你的核心需求，优先保证「key不存在返回-1」的语义，这里暂返回-1（或自定义-3）
    return -1;
  }
  strcpy(current->value, value); // 拷贝新value

  // 5. 修改成功，返回0
  return 0;
}
int skiplist_exist(kvs_skiplist_t *inst, char *key) {
  if (inst == NULL || key == NULL) {
    return -1;
  }
  char *ret = skiplist_get(inst, key);
  if (ret == NULL) {
    return 1;
  } else {
    return 0;
  }
}
