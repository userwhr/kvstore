#include "array.h"
#include "config.h"
#include "hash.h"
#include "iouringTodesk.h"
#include "kvstore.h"
#include "rbtree.h"
#include "skiplist.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  char *new_str = (char *)malloc(len);
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

// 数组遍历（无修改，稳定）
KvInf_t *traverse_array(void) {
  if (!g_config.enable_array || !global_array)
    return NULL;
  KvInf_t *inf = (KvInf_t *)malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "array";
  inf->count = global_array->total;
  if (inf->count == 0) {
    inf->key = NULL;
    inf->value = NULL;
    return inf;
  }
  inf->key = (char **)malloc(sizeof(char *) * inf->count);
  inf->value = (char **)malloc(sizeof(char *) * inf->count);
  if (!inf->key || !inf->value) {
    free(inf->key);
    free(inf->value);
    free(inf);
    return NULL;
  }
  for (int i = 0; i < inf->count; i++) {
    inf->key[i] = safe_strdup(global_array->table[i].key);
    inf->value[i] = safe_strdup(global_array->table[i].value);
    if (!inf->key[i] || !inf->value[i]) {
      for (int j = 0; j < i; j++) {
        free(inf->key[j]);
        free(inf->value[j]);
      }
      free(inf->key);
      free(inf->value);
      free(inf);
      return NULL;
    }
  }
  return inf;
}

// 红黑树遍历回调（修复：扩容失败直接return，杜绝越界）
typedef struct {
  char **keys;
  char **values;
  int index;
  int capacity;
} RbtreeCollector;

static void rbtree_collect_callback(const void *key, const void *value,
                                    KvsRbtreeColor color, void *user_data) {
  RbtreeCollector *collector = (RbtreeCollector *)user_data;
  if (!collector || !key || !value)
    return;

  // 扩容：失败直接退出，不继续执行
  if (collector->index >= collector->capacity) {
    int new_cap = collector->capacity * 2;
    char **new_keys = (char **)malloc(sizeof(char *) * new_cap);
    char **new_values = (char **)malloc(sizeof(char *) * new_cap);
    if (!new_keys || !new_values) {
      free(new_keys);
      free(new_values);
      return;
    }
    memcpy(new_keys, collector->keys, sizeof(char *) * collector->capacity);
    memcpy(new_values, collector->values, sizeof(char *) * collector->capacity);
    free(collector->keys);
    free(collector->values);
    collector->keys = new_keys;
    collector->values = new_values;
    collector->capacity = new_cap;
  }

  collector->keys[collector->index] = safe_strdup((const char *)key);
  collector->values[collector->index] = safe_strdup((const char *)value);
  collector->index++;
}

// 红黑树遍历（修复：初始化结构体+内存安全）
KvInf_t *traverse_rbtree(void) {
  // printf("[调试] rbtree遍历: enable=%d, global_rbtree=%p\n",
  //        g_config.enable_rbtree, global_rbtree);

  if (!g_config.enable_rbtree || !global_rbtree)
    return NULL;

  KvInf_t *inf = (KvInf_t *)malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "rbtree";
  inf->key = NULL;
  inf->value = NULL;
  inf->count = 0;

  // 🔥 修复：栈上结构体清零（杜绝随机值）
  RbtreeCollector collector = {0};
  collector.capacity = 16;
  collector.index = 0;
  collector.keys = (char **)malloc(sizeof(char *) * collector.capacity);
  collector.values = (char **)malloc(sizeof(char *) * collector.capacity);

  if (!collector.keys || !collector.values) {
    free(collector.keys);
    free(collector.values);
    free(inf);
    return NULL;
  }

  rbtree_traverse(global_rbtree, rbtree_collect_callback, &collector);
  inf->count = collector.index;
  inf->key = collector.keys;
  inf->value = collector.values;

  // 安全收缩内存
  if (collector.index > 0 && collector.capacity > collector.index) {
    char **new_keys = (char **)malloc(sizeof(char *) * collector.index);
    char **new_values = (char **)malloc(sizeof(char *) * collector.index);
    if (new_keys && new_values) {
      memcpy(new_keys, collector.keys, sizeof(char *) * collector.index);
      memcpy(new_values, collector.values, sizeof(char *) * collector.index);
      free(collector.keys);
      free(collector.values);
      inf->key = new_keys;
      inf->value = new_values;
    }
  }

  return inf;
}

// 哈希表遍历（无修改，稳定）
KvInf_t *traverse_hash(void) {
  if (!g_config.enable_hash || !global_hash)
    return NULL;
  KvInf_t *inf = (KvInf_t *)malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "hash";
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
  inf->key = (char **)malloc(sizeof(char *) * total);
  inf->value = (char **)malloc(sizeof(char *) * total);
  if (!inf->key || !inf->value) {
    free(inf->key);
    free(inf->value);
    free(inf);
    return NULL;
  }
  int idx = 0;
  for (int i = 0; i < global_hash->max_slots; i++) {
    hashnode_t *node = global_hash->nodes[i];
    while (node) {
      inf->key[idx] = safe_strdup(node->key);
      inf->value[idx] = safe_strdup(node->value);
      if (!inf->key[idx] || !inf->value[idx]) {
        for (int j = 0; j < idx; j++) {
          free(inf->key[j]);
          free(inf->value[j]);
        }
        free(inf->key);
        free(inf->value);
        free(inf);
        return NULL;
      }
      idx++;
      node = node->next;
    }
  }
  return inf;
}

// 跳表遍历（无修改，稳定）
KvInf_t *traverse_skiplist(void) {
  if (!g_config.enable_skiplist || !global_skiplist)
    return NULL;
  KvInf_t *inf = (KvInf_t *)malloc(sizeof(KvInf_t));
  if (!inf)
    return NULL;
  inf->type_name = "skiplist";
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
  inf->key = (char **)malloc(sizeof(char *) * total);
  inf->value = (char **)malloc(sizeof(char *) * total);
  if (!inf->key || !inf->value) {
    free(inf->key);
    free(inf->value);
    free(inf);
    return NULL;
  }
  current = global_skiplist->header->forward[0];
  int idx = 0;
  while (current) {
    inf->key[idx] = safe_strdup(current->key);
    inf->value[idx] = safe_strdup(current->value);
    if (!inf->key[idx] || !inf->value[idx]) {
      for (int j = 0; j < idx; j++) {
        free(inf->key[j]);
        free(inf->value[j]);
      }
      free(inf->key);
      free(inf->value);
      free(inf);
      return NULL;
    }
    idx++;
    current = current->forward[0];
  }
  return inf;
}

// 释放内存（无修改，稳定）
void free_kvinf(KvInf_t *inf) {
  if (!inf)
    return;
  for (int i = 0; i < inf->count; i++) {
    free(inf->key[i]);
    free(inf->value[i]);
  }
  free(inf->key);
  free(inf->value);
  free(inf);
}

// ====================== RDB io_uring 异步写入 ======================
#define RDB_BUF_INIT_SIZE (1024 * 1024) // 初始 1MB

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} RdbBuf;

static void rdb_buf_init(RdbBuf *buf) {
  buf->cap = RDB_BUF_INIT_SIZE;
  buf->data = (char *)malloc(buf->cap);
  buf->len = 0;
}

static int rdb_buf_ensure(RdbBuf *buf, size_t need) {
  if (buf->len + need > buf->cap) {
    while (buf->len + need > buf->cap)
      buf->cap *= 2;
    char *new_data = (char *)realloc(buf->data, buf->cap);
    if (!new_data)
      return -1;
    buf->data = new_data;
  }
  return 0;
}

static void rdb_buf_write(RdbBuf *buf, const void *src, size_t n) {
  memcpy(buf->data + buf->len, src, n);
  buf->len += n;
}

// 写入一条二进制键值对：[4字节klen][4字节vlen][key bytes][value bytes]
static void rdb_buf_kv(RdbBuf *buf, const char *key, const char *value) {
  uint32_t klen = (uint32_t)strlen(key);
  uint32_t vlen = (uint32_t)strlen(value);
  rdb_buf_ensure(buf, 8 + klen + vlen);
  rdb_buf_write(buf, &klen, 4);
  rdb_buf_write(buf, &vlen, 4);
  rdb_buf_write(buf, key, klen);
  rdb_buf_write(buf, value, vlen);
}

// RDB 异步写入上下文
typedef struct {
  char *buf;
  int fd;
} RdbWriteCtx;

extern volatile int g_rdb_write_done;

// RDB 写入完成回调
static void rdb_write_callback(int res, void *data) {
  RdbWriteCtx *ctx = (RdbWriteCtx *)data;
  if (res < 0) {
    fprintf(stderr, "[RDB] io_uring write failed: %s\n", strerror(-res));
  } else {
    printf("[RDB] Async write complete (%d bytes)\n", res);
  }
  close(ctx->fd);
  free(ctx->buf);
  free(ctx);
  g_rdb_write_done = 1;
}

// 落盘函数（io_uring 异步写入）
int dump_all_to_disk(void) {
  RdbBuf buf;
  rdb_buf_init(&buf);
  if (!buf.data) {
    perror("[RDB] malloc buffer failed");
    return -1;
  }

  // 数组
  if (g_config.enable_array) {
    KvInf_t *inf = traverse_array();
    if (inf) {
      char header[64];
      int hlen = sprintf(header, "array %d\n", inf->count);
      rdb_buf_ensure(&buf, hlen);
      rdb_buf_write(&buf, header, hlen);
      for (int i = 0; i < inf->count; i++)
        rdb_buf_kv(&buf, inf->key[i], inf->value[i]);
      free_kvinf(inf);
    }
  }

  // 红黑树
  if (g_config.enable_rbtree) {
    KvInf_t *inf = traverse_rbtree();
    if (inf) {
      char header[64];
      int hlen = sprintf(header, "rbtree %d\n", inf->count);
      rdb_buf_ensure(&buf, hlen);
      rdb_buf_write(&buf, header, hlen);
      for (int i = 0; i < inf->count; i++)
        rdb_buf_kv(&buf, inf->key[i], inf->value[i]);
      free_kvinf(inf);
    }
  }

  // 哈希
  if (g_config.enable_hash) {
    KvInf_t *inf = traverse_hash();
    if (inf) {
      char header[64];
      int hlen = sprintf(header, "hash %d\n", inf->count);
      rdb_buf_ensure(&buf, hlen);
      rdb_buf_write(&buf, header, hlen);
      for (int i = 0; i < inf->count; i++)
        rdb_buf_kv(&buf, inf->key[i], inf->value[i]);
      free_kvinf(inf);
    }
  }

  // 跳表
  if (g_config.enable_skiplist) {
    KvInf_t *inf = traverse_skiplist();
    if (inf) {
      char header[64];
      int hlen = sprintf(header, "skiplist %d\n", inf->count);
      rdb_buf_ensure(&buf, hlen);
      rdb_buf_write(&buf, header, hlen);
      for (int i = 0; i < inf->count; i++)
        rdb_buf_kv(&buf, inf->key[i], inf->value[i]);
      free_kvinf(inf);
    }
  }

  // 空数据直接返回
  if (buf.len == 0) {
    free(buf.data);
    return 0;
  }

  // 打开文件（截断模式）
  int fd = open("./log/full_log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("[RDB] open failed");
    free(buf.data);
    return -1;
  }

  // 通过 io_uring 异步写入
  static off_t rdb_offset = 0;
  rdb_offset = 0;

  RdbWriteCtx *ctx = (RdbWriteCtx *)malloc(sizeof(RdbWriteCtx));
  ctx->buf = buf.data;
  ctx->fd = fd;

  int ret = uringAsyncWrite(fd, buf.data, buf.len, rdb_offset,
                            rdb_write_callback, ctx);
  if (ret < 0) {
    fprintf(stderr, "[RDB] uringAsyncWrite failed: %d\n", ret);
    free(buf.data);
    free(ctx);
    close(fd);
    return -1;
  }

  printf("[RDB] Submitted %zu bytes to io_uring\n", buf.len);
  return 0;
}