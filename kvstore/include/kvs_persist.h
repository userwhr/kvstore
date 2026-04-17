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
  size_t file_size; // 文件真实大小（只读)
} file_content_t;

typedef struct KvRecoverResult {
  int cmd_count;
  char **cmds;
} KvRecoverResult;
int fcInit(const char *filename, file_content_t *fc);
int map_file_to_struct(file_content_t *fc);
void free_file_content(file_content_t *fc);
int replay_commands_from_mmap(file_content_t *fc);
#endif
