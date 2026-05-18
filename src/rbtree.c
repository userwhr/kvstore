#include "rbtree.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/************************ 私有函数声明（内部使用） ************************/
static KvsRbtreeNode *rbtree_min_node(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static KvsRbtreeNode *rbtree_max_node(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static KvsRbtreeNode *rbtree_successor(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static void rbtree_left_rotate(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static void rbtree_right_rotate(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static void rbtree_insert_fixup(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static void rbtree_insert_node(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static void rbtree_delete_fixup(kvs_rbtree_t *tree, KvsRbtreeNode *node);
static KvsRbtreeNode *rbtree_delete_node(kvs_rbtree_t *tree,
                                         KvsRbtreeNode *node);
static KvsRbtreeNode *rbtree_search_node(kvs_rbtree_t *tree, const void *key);
static void rbtree_traverse_node(kvs_rbtree_t *tree, KvsRbtreeNode *node,
                                 KvsRbtreeTraverseFunc callback,
                                 void *user_data);
static int rbtree_str_key_cmp(const void *key1, const void *key2);
static char *safe_strdup(const char *str);

/************************ 内存管理实现（可替换为自定义内存池）
 * ************************/

/************************ 私有工具函数 ************************/
// 字符串比较函数（默认）
static int rbtree_str_key_cmp(const void *key1, const void *key2) {
  assert(key1 && key2);
  return strcmp((const char *)key1, (const char *)key2);
}

// 安全字符串拷贝（封装malloc+strcpy，失败返回NULL）
static char *safe_strdup(const char *str) {
  if (!str)
    return NULL;
  size_t len = strlen(str) + 1;
  char *new_str = (char *)malloc(len);
  if (new_str) {
    strcpy(new_str, str);
  }
  return new_str;
}

/************************ 红黑树核心操作（私有） ************************/
// 查找最小节点
static KvsRbtreeNode *rbtree_min_node(kvs_rbtree_t *tree, KvsRbtreeNode *node) {
  assert(tree && node);
  while (node->left != tree->nil) {
    node = node->left;
  }
  return node;
}

// 查找最大节点
static KvsRbtreeNode *rbtree_max_node(kvs_rbtree_t *tree, KvsRbtreeNode *node) {
  assert(tree && node);
  while (node->right != tree->nil) {
    node = node->right;
  }
  return node;
}

// 查找后继节点
static KvsRbtreeNode *rbtree_successor(kvs_rbtree_t *tree,
                                       KvsRbtreeNode *node) {
  assert(tree && node);
  KvsRbtreeNode *parent = node->parent;

  // 有右子树：后继是右子树最小节点
  if (node->right != tree->nil) {
    return rbtree_min_node(tree, node->right);
  }

  // 无右子树：向上找第一个左孩子的父节点
  while (parent != tree->nil && node == parent->right) {
    node = parent;
    parent = parent->parent;
  }
  return parent;
}

// 左旋转
static void rbtree_left_rotate(kvs_rbtree_t *tree, KvsRbtreeNode *x) {
  assert(tree && x && x->right != tree->nil);
  KvsRbtreeNode *y = x->right;

  // 1. 将y的左子树挂到x的右子树
  x->right = y->left;
  if (y->left != tree->nil) {
    y->left->parent = x;
  }

  // 2. 处理y的父节点
  y->parent = x->parent;
  if (x->parent == tree->nil) { // x是根节点
    tree->root = y;
  } else if (x == x->parent->left) { // x是左孩子
    x->parent->left = y;
  } else { // x是右孩子
    x->parent->right = y;
  }

  // 3. x作为y的左孩子
  y->left = x;
  x->parent = y;
}

// 右旋转
static void rbtree_right_rotate(kvs_rbtree_t *tree, KvsRbtreeNode *y) {
  assert(tree && y && y->left != tree->nil);
  KvsRbtreeNode *x = y->left;

  // 1. 将x的右子树挂到y的左子树
  y->left = x->right;
  if (x->right != tree->nil) {
    x->right->parent = y;
  }

  // 2. 处理x的父节点
  x->parent = y->parent;
  if (y->parent == tree->nil) { // y是根节点
    tree->root = x;
  } else if (y == y->parent->right) { // y是右孩子
    y->parent->right = x;
  } else { // y是左孩子
    y->parent->left = x;
  }

  // 3. y作为x的右孩子
  x->right = y;
  y->parent = x;
}

// 插入后修复红黑树性质
static void rbtree_insert_fixup(kvs_rbtree_t *tree, KvsRbtreeNode *z) {
  assert(tree && z);
  KvsRbtreeNode *uncle = tree->nil;

  // 父节点为红色时需要修复
  while (z->parent->color == KVS_RBTREE_COLOR_RED) {
    KvsRbtreeNode *grandparent =
        z->parent->parent; // 祖父节点（必存在，根是黑）

    // 父节点是祖父的左孩子
    if (z->parent == grandparent->left) {
      uncle = grandparent->right; // 叔叔节点

      // 情况1：叔叔是红色 → 重染色
      if (uncle->color == KVS_RBTREE_COLOR_RED) {
        z->parent->color = KVS_RBTREE_COLOR_BLACK;
        uncle->color = KVS_RBTREE_COLOR_BLACK;
        grandparent->color = KVS_RBTREE_COLOR_RED;
        z = grandparent; // 继续向上修复
      }
      // 情况2：叔叔是黑色
      else {
        // 情况2a：z是右孩子 → 先左旋转为情况2b
        if (z == z->parent->right) {
          z = z->parent;
          rbtree_left_rotate(tree, z);
        }
        // 情况2b：z是左孩子 → 重染色+右旋
        z->parent->color = KVS_RBTREE_COLOR_BLACK;
        grandparent->color = KVS_RBTREE_COLOR_RED;
        rbtree_right_rotate(tree, grandparent);
      }
    }
    // 父节点是祖父的右孩子（镜像逻辑）
    else {
      uncle = grandparent->left;

      if (uncle->color == KVS_RBTREE_COLOR_RED) {
        z->parent->color = KVS_RBTREE_COLOR_BLACK;
        uncle->color = KVS_RBTREE_COLOR_BLACK;
        grandparent->color = KVS_RBTREE_COLOR_RED;
        z = grandparent;
      } else {
        if (z == z->parent->left) {
          z = z->parent;
          rbtree_right_rotate(tree, z);
        }
        z->parent->color = KVS_RBTREE_COLOR_BLACK;
        grandparent->color = KVS_RBTREE_COLOR_RED;
        rbtree_left_rotate(tree, grandparent);
      }
    }
  }
  // 根节点强制为黑色
  tree->root->color = KVS_RBTREE_COLOR_BLACK;
}

// 插入节点（内部）
static void rbtree_insert_node(kvs_rbtree_t *tree, KvsRbtreeNode *z) {
  assert(tree && z && tree->key_cmp);
  KvsRbtreeNode *parent = tree->nil;
  KvsRbtreeNode *current = tree->root;

  // 1. 查找插入位置
  while (current != tree->nil) {
    parent = current;
    int cmp = tree->key_cmp(z->key, current->key);
    if (cmp < 0) {
      current = current->left;
    } else if (cmp > 0) {
      current = current->right;
    } else {
      // 键已存在，直接返回（外部set接口会先删除旧节点）
      return;
    }
  }

  // 2. 设置父节点
  z->parent = parent;
  if (parent == tree->nil) { // 树为空，z是根节点
    tree->root = z;
  } else {
    int cmp = tree->key_cmp(z->key, parent->key);
    if (cmp < 0) {
      parent->left = z;
    } else {
      parent->right = z;
    }
  }

  // 3. 初始化节点属性
  z->left = tree->nil;
  z->right = tree->nil;
  z->color = KVS_RBTREE_COLOR_RED;

  // 4. 修复红黑树性质
  rbtree_insert_fixup(tree, z);
}

// 删除后修复红黑树性质
static void rbtree_delete_fixup(kvs_rbtree_t *tree, KvsRbtreeNode *x) {
  assert(tree && x);
  KvsRbtreeNode *sibling = tree->nil;

  while (x != tree->root && x->color == KVS_RBTREE_COLOR_BLACK) {
    // x是左孩子
    if (x == x->parent->left) {
      sibling = x->parent->right;

      // 情况1：兄弟是红色 → 重染色+左旋
      if (sibling->color == KVS_RBTREE_COLOR_RED) {
        sibling->color = KVS_RBTREE_COLOR_BLACK;
        x->parent->color = KVS_RBTREE_COLOR_RED;
        rbtree_left_rotate(tree, x->parent);
        sibling = x->parent->right;
      }

      // 情况2：兄弟的两个孩子都是黑色 → 重染色
      if (sibling->left->color == KVS_RBTREE_COLOR_BLACK &&
          sibling->right->color == KVS_RBTREE_COLOR_BLACK) {
        sibling->color = KVS_RBTREE_COLOR_RED;
        x = x->parent;
      }
      // 情况3：兄弟右孩子黑色，左孩子红色 → 重染色+右旋
      else if (sibling->right->color == KVS_RBTREE_COLOR_BLACK) {
        sibling->left->color = KVS_RBTREE_COLOR_BLACK;
        sibling->color = KVS_RBTREE_COLOR_RED;
        rbtree_right_rotate(tree, sibling);
        sibling = x->parent->right;
      }
      // 情况4：兄弟右孩子红色 → 重染色+左旋
      else {
        sibling->color = x->parent->color;
        x->parent->color = KVS_RBTREE_COLOR_BLACK;
        sibling->right->color = KVS_RBTREE_COLOR_BLACK;
        rbtree_left_rotate(tree, x->parent);
        x = tree->root; // 修复完成，退出循环
      }
    }
    // x是右孩子（镜像逻辑）
    else {
      sibling = x->parent->left;

      if (sibling->color == KVS_RBTREE_COLOR_RED) {
        sibling->color = KVS_RBTREE_COLOR_BLACK;
        x->parent->color = KVS_RBTREE_COLOR_RED;
        rbtree_right_rotate(tree, x->parent);
        sibling = x->parent->left;
      }

      if (sibling->left->color == KVS_RBTREE_COLOR_BLACK &&
          sibling->right->color == KVS_RBTREE_COLOR_BLACK) {
        sibling->color = KVS_RBTREE_COLOR_RED;
        x = x->parent;
      } else if (sibling->left->color == KVS_RBTREE_COLOR_BLACK) {
        sibling->right->color = KVS_RBTREE_COLOR_BLACK;
        sibling->color = KVS_RBTREE_COLOR_RED;
        rbtree_left_rotate(tree, sibling);
        sibling = x->parent->left;
      } else {
        sibling->color = x->parent->color;
        x->parent->color = KVS_RBTREE_COLOR_BLACK;
        sibling->left->color = KVS_RBTREE_COLOR_BLACK;
        rbtree_right_rotate(tree, x->parent);
        x = tree->root;
      }
    }
  }
  x->color = KVS_RBTREE_COLOR_BLACK;
}

// 删除节点（内部）
static KvsRbtreeNode *rbtree_delete_node(kvs_rbtree_t *tree, KvsRbtreeNode *z) {
  assert(tree && z && z != tree->nil);
  KvsRbtreeNode *del_node = tree->nil; // 实际要删除的节点
  KvsRbtreeNode *fix_node = tree->nil; // 需要修复的节点

  // 1. 确定实际删除的节点（最多一个子节点）
  if (z->left == tree->nil || z->right == tree->nil) {
    del_node = z;
  } else {
    del_node = rbtree_successor(tree, z); // 后继节点
  }

  // 2. 确定修复节点（删除节点的子节点）
  fix_node = (del_node->left != tree->nil) ? del_node->left : del_node->right;

  // 3. 处理父节点关联
  fix_node->parent = del_node->parent;
  if (del_node->parent == tree->nil) {
    tree->root = fix_node;
  } else if (del_node == del_node->parent->left) {
    del_node->parent->left = fix_node;
  } else {
    del_node->parent->right = fix_node;
  }

  // 4. 若删除的是后继节点，拷贝键值到原节点
  if (del_node != z) {
    // 释放原节点的键值，替换为后继节点的键值
    free(z->key);
    free(z->value);
    z->key = safe_strdup((const char *)del_node->key);
    z->value = safe_strdup((const char *)del_node->value);
    if (!z->key || !z->value) {
      fprintf(stderr, "copy key/value failed when delete successor\n");
    }
  }

  // 5. 若删除的是黑色节点，修复红黑树性质
  if (del_node->color == KVS_RBTREE_COLOR_BLACK) {
    rbtree_delete_fixup(tree, fix_node);
  }

  return del_node; // 返回实际删除的节点，供外部释放
}

// 查找节点（内部）
static KvsRbtreeNode *rbtree_search_node(kvs_rbtree_t *tree, const void *key) {
  if (!tree || !key || tree->key_cmp == NULL) {
    return NULL;
  }

  KvsRbtreeNode *current = tree->root;
  while (current != tree->nil) {
    int cmp = tree->key_cmp(key, current->key);
    if (cmp < 0) {
      current = current->left;
    } else if (cmp > 0) {
      current = current->right;
    } else {
      return current; // 找到节点
    }
  }
  return tree->nil; // 未找到
}

// 遍历节点（内部，中序）
static void rbtree_traverse_node(kvs_rbtree_t *tree, KvsRbtreeNode *node,
                                 KvsRbtreeTraverseFunc callback,
                                 void *user_data) {
  if (!tree || !callback || node == tree->nil) {
    return;
  }
  // 中序：左→根→右
  rbtree_traverse_node(tree, node->left, callback, user_data);
  callback(node->key, node->value, node->color, user_data);
  rbtree_traverse_node(tree, node->right, callback, user_data);
}

/************************ 红黑树对外接口（公有） ************************/
// 创建红黑树
KvsRbtreeErrCode rbtree_create(kvs_rbtree_t *inst) {
  if (!inst) {
    return KVS_RBTREE_ERR_INVALID_PARAM;
  }

  // 初始化哨兵节点（nil）
  inst->nil = (KvsRbtreeNode *)malloc(sizeof(KvsRbtreeNode));
  if (!inst->nil) {
    return KVS_RBTREE_ERR_MEM_ALLOC;
  }
  inst->nil->color = KVS_RBTREE_COLOR_BLACK;
  inst->nil->left = inst->nil;
  inst->nil->right = inst->nil;
  inst->nil->parent = inst->nil;

  // 初始化根节点和比较函数
  inst->root = inst->nil;
  inst->key_cmp = rbtree_str_key_cmp; // 默认字符键比较

  return KVS_RBTREE_SUCCESS;
}

// 销毁红黑树
void rbtree_destroy(kvs_rbtree_t *inst) {
  if (!inst) {
    return;
  }

  // 循环删除所有节点（中序遍历删除最小节点）
  while (inst->root != inst->nil) {
    KvsRbtreeNode *min_node = rbtree_min_node(inst, inst->root);
    KvsRbtreeNode *del_node = rbtree_delete_node(inst, min_node);
    if (del_node != inst->nil) {
      free(del_node->key);
      free(del_node->value);
      free(del_node);
    }
  }

  // 释放哨兵节点
  free(inst->nil);
  inst->root = NULL;
  inst->nil = NULL;
  inst->key_cmp = NULL;
}

// 设置键值对
KvsRbtreeErrCode rbtree_set(kvs_rbtree_t *inst, const char *key,
                                const char *value) {
  // 参数校验
  if (!inst || !key || strlen(key) == 0 || !value) {
    return KVS_RBTREE_ERR_INVALID_PARAM;
  }

  // 先删除已存在的节点（避免重复）
  KvsRbtreeErrCode ret = rbtree_del(inst, key);
  if (ret != KVS_RBTREE_SUCCESS && ret != KVS_RBTREE_ERR_KEY_NOT_EXIST) {
    return ret; // 除了"不存在"外的错误直接返回
  }

  // 创建新节点
  KvsRbtreeNode *new_node = (KvsRbtreeNode *)malloc(sizeof(KvsRbtreeNode));
  if (!new_node) {
    return KVS_RBTREE_ERR_MEM_ALLOC;
  }

  // 分配并拷贝键值
  new_node->key = safe_strdup(key);
  new_node->value = safe_strdup(value);
  if (!new_node->key || !new_node->value) {
    free(new_node->key);
    free(new_node->value);
    free(new_node);
    return KVS_RBTREE_ERR_MEM_ALLOC;
  }

  // 插入节点
  rbtree_insert_node(inst, new_node);
  return KVS_RBTREE_SUCCESS;
}

// 获取值
char *rbtree_get(kvs_rbtree_t *inst, const char *key) {
  if (!inst || !key || strlen(key) == 0) {
    return NULL;
  }

  KvsRbtreeNode *node = rbtree_search_node(inst, key);
  if (node == inst->nil || node == NULL) {
    return NULL;
  }

  // 返回值的拷贝，外部需自行释放
  return safe_strdup((const char *)node->value);
}

// 删除键值对
KvsRbtreeErrCode rbtree_del(kvs_rbtree_t *inst, const char *key) {
  if (!inst || !key || strlen(key) == 0) {
    return KVS_RBTREE_ERR_INVALID_PARAM;
  }

  KvsRbtreeNode *node = rbtree_search_node(inst, key);
  if (node == inst->nil || node == NULL) {
    return KVS_RBTREE_ERR_KEY_NOT_EXIST;
  }

  // 删除节点并释放内存
  KvsRbtreeNode *del_node = rbtree_delete_node(inst, node);
  if (del_node != inst->nil) {
    free(del_node->key);
    free(del_node->value);
    free(del_node);
  }
  return KVS_RBTREE_SUCCESS;
}

// 修改值
KvsRbtreeErrCode rbtree_mod(kvs_rbtree_t *inst, const char *key,
                                const char *new_value) {
  if (!inst || !key || strlen(key) == 0 || !new_value) {
    return KVS_RBTREE_ERR_INVALID_PARAM;
  }

  KvsRbtreeNode *node = rbtree_search_node(inst, key);
  if (node == inst->nil || node == NULL) {
    return KVS_RBTREE_ERR_KEY_NOT_EXIST;
  }

  // 释放旧值，拷贝新值
  free(node->value);
  node->value = safe_strdup(new_value);
  if (!node->value) {
    return KVS_RBTREE_ERR_MEM_ALLOC;
  }

  return KVS_RBTREE_SUCCESS;
}

// 检查键是否存在
int rbtree_exist(kvs_rbtree_t *inst, const char *key) {
  if (!inst || !key || strlen(key) == 0) {
    return -1; // 参数错误
  }

  KvsRbtreeNode *node = rbtree_search_node(inst, key);
  return (node == inst->nil || node == NULL) ? 1 : 0;
}

// 遍历红黑树
void rbtree_traverse(kvs_rbtree_t *inst, KvsRbtreeTraverseFunc callback,
                         void *user_data) {
  if (!inst || !callback) {
    return;
  }
  rbtree_traverse_node(inst, inst->root, callback, user_data);
}