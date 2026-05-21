#ifndef _RBTREE_H
#define _RBTREE_H

#include <stdbool.h>
#include <stddef.h>

/************************ 通用配置与类型定义 ************************/
// 颜色枚举（替代宏定义，类型更安全）
typedef enum {
  KVS_RBTREE_COLOR_RED = 0,
  KVS_RBTREE_COLOR_BLACK = 1
} KvsRbtreeColor;

// 错误码枚举（替代魔法数字，语义清晰）
typedef enum {
  KVS_RBTREE_SUCCESS = 0,            // 操作成功
  KVS_RBTREE_ERR_INVALID_PARAM = -1, // 无效参数
  KVS_RBTREE_ERR_MEM_ALLOC = -2,     // 内存分配失败
  KVS_RBTREE_ERR_KEY_NOT_EXIST = 1,  // 键不存在
  KVS_RBTREE_ERR_KEY_EXIST = 2       // 键已存在（可选）
} KvsRbtreeErrCode;

// 键比较函数指针（支持自定义键类型，解耦宏开关）
typedef int (*KvsRbtreeKeyCmpFunc)(const void *key1, const void *key2);

// 遍历回调函数（解耦打印逻辑，增强扩展性）
typedef void (*KvsRbtreeTraverseFunc)(const void *key, const void *value,
                                      KvsRbtreeColor color, void *user_data);

/************************ 红黑树节点/树结构定义 ************************/
// 前置声明（隐藏内部实现，提升封装性）
typedef struct _KvsRbtreeNode KvsRbtreeNode;
typedef struct _KvsRbtree KvsRbtree;

// 红黑树节点结构体（重命名提升语义）
struct _KvsRbtreeNode {
  KvsRbtreeColor color;  // 节点颜色
  KvsRbtreeNode *left;   // 左孩子
  KvsRbtreeNode *right;  // 右孩子
  KvsRbtreeNode *parent; // 父节点
  void *key;             // 键（支持任意类型）
  void *value;           // 值（支持任意类型）
};

// 红黑树核心结构体（新增比较函数、空节点，增强扩展性）
struct _KvsRbtree {
  KvsRbtreeNode *root;         // 根节点
  KvsRbtreeNode *nil;          // 哨兵节点（替代NULL，简化边界处理）
  KvsRbtreeKeyCmpFunc key_cmp; // 键比较函数
  int count;                   // 已存储的 kv 对数量
};

// 对外暴露的红黑树类型别名（保持兼容）
typedef struct _KvsRbtree kvs_rbtree_t;

/************************ 内存管理接口（可自定义实现） ************************/
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

/************************ 红黑树核心操作接口 ************************/
/**
 * @brief 创建红黑树（默认字符键比较）
 * @param inst 红黑树实例指针（需提前分配内存）
 * @return 错误码：KVS_RBTREE_SUCCESS/失败码
 */
KvsRbtreeErrCode rbtree_create(kvs_rbtree_t *inst);

/**
 * @brief 销毁红黑树（释放所有节点+内存）
 * @param inst 红黑树实例指针
 */
void rbtree_destroy(kvs_rbtree_t *inst);

/**
 * @brief 设置键值对（存在则覆盖）
 * @param inst 红黑树实例
 * @param key 键（字符串）
 * @param value 值（字符串）
 * @return 错误码：KVS_RBTREE_SUCCESS/失败码
 */
KvsRbtreeErrCode rbtree_set(kvs_rbtree_t *inst, const char *key,
                                const char *value);

/**
 * @brief 获取键对应的值（返回值需调用kvs_free释放）
 * @param inst 红黑树实例
 * @param key 键（字符串）
 * @return 成功返回值的拷贝，失败返回NULL
 */
char *rbtree_get(kvs_rbtree_t *inst, const char *key);

/**
 * @brief 删除指定键值对
 * @param inst 红黑树实例
 * @param key 键（字符串）
 * @return 错误码：KVS_RBTREE_SUCCESS/KEY_NOT_EXIST/无效参数
 */
KvsRbtreeErrCode rbtree_del(kvs_rbtree_t *inst, const char *key);

/**
 * @brief 修改指定键的值
 * @param inst 红黑树实例
 * @param key 键（字符串）
 * @param new_value 新值（字符串）
 * @return 错误码：KVS_RBTREE_SUCCESS/KEY_NOT_EXIST/内存失败/无效参数
 */
KvsRbtreeErrCode rbtree_mod(kvs_rbtree_t *inst, const char *key,
                                const char *new_value);

/**
 * @brief 检查键是否存在
 * @param inst 红黑树实例
 * @param key 键（字符串）
 * @return 0-存在，1-不存在，-1-参数错误
 */
int rbtree_exist(kvs_rbtree_t *inst, const char *key);

/**
 * @brief 中序遍历红黑树（回调方式）
 * @param inst 红黑树实例
 * @param callback 遍历回调函数
 * @param user_data 传递给回调的用户数据
 */
void rbtree_traverse(kvs_rbtree_t *inst, KvsRbtreeTraverseFunc callback,
                         void *user_data);

#endif // _KVS_RBTREE_H