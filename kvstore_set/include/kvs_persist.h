#ifndef _KVS_PERSIST_H
#define _KVS_PERSIST_H
#include "array.h"
#include "hash.h"
#include "rbtree.h"
#include "skiplist.h"
#if ENABLE_ARRAY
extern kvs_array_t *global_array;
#endif
#if ENABLE_RBTREE
extern kvs_rbtree_t *global_rbtree;
#endif
#if ENABLE_HASH
extern kvs_hash_t *global_hash;
#endif
#if ENABLE_SKIPLIST
extern kvs_skiplist_t *global_skiplist;
#endif

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
// 双区分离：文件只读区 + 匿名可写区
typedef struct file_content {
  char *filename;
  int file_fd;
  // 文件只读映射（只映射文件真实大小，绝不越界）
  void *file_data;
  size_t file_size; // 文件真实大小（只读）
  // 匿名可写映射（动态扩容，无Bus error）
  void *anon_data;
  size_t anon_cap; // 匿名区容量（只扩内存）
  size_t used_len; // 有效数据总长度
} file_content_t;

typedef struct KvRecoverResult {
  int cmd_count;
  char **cmds;
} KvRecoverResult;
int execute_recovery_cmds(file_content_t *fc);
int init_file(const char *filename, file_content_t *fc);
int modify_mmap_cmd(file_content_t *fc, const char *mod_type, const char *key,
                    const char *new_value);
int delete_mmap_cmd(file_content_t *fc, const char *token1, const char *token2);
void free_file_content(file_content_t *fc);
int execute_recovered_cmds(const char *file);
int sync_mmap_to_file(file_content_t *fc);
// 【修正版】映射文件到结构体（可写、可落盘）
int map_file_to_struct(const char *filename, file_content_t *fc);
int append_mmap_cmd(file_content_t *fc, const char *cmd_type, const char *key,
                    const char *value);
#endif
