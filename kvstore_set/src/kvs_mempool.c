#include "mempool.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 单线程核心结构（极简·有记录）====================
typedef struct mem_item {
  struct mem_item *next;
} mem_item_t;

// 记录每次 expand 分配的大块内存区间
typedef struct mem_chunk {
  char *start;            // 大块起始地址
  char *end;              // 大块结束地址（不包含）
  struct mem_chunk *next; // 下一个大块
} mem_chunk_t;

typedef struct {
  mem_item_t *free_list; // 空闲对象链表
  size_t block_size;     // 该槽位每个对象的大小
  mem_chunk_t *chunks;   // 该槽位下所有已分配的大块区间
} mem_slot_t;

static mem_slot_t g_slots[SLOT_NUM];
static int g_inited = 0;

#define EXPAND_COUNT 128

static inline size_t align_size(size_t size) {
  return (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
}

static inline int get_slot_idx(size_t align_sz) {
  return (align_sz / MEM_ALIGN) - 1;
}

// 扩充指定 slot 的内存池
static void expand_slot(mem_slot_t *slot) {
  size_t total_size = slot->block_size * EXPAND_COUNT;
  char *chunk = (char *)malloc(total_size);
  if (!chunk)
    return;

  // 记录这个大块的区间，以便释放时快速判断归属
  mem_chunk_t *c = (mem_chunk_t *)malloc(sizeof(mem_chunk_t));
  if (!c) {
    free(chunk);
    return;
  }
  c->start = chunk;
  c->end = chunk + total_size;
  c->next = slot->chunks;
  slot->chunks = c;

  // 将大块拆分成空闲对象，头插到 free_list
  for (int i = 0; i < EXPAND_COUNT; i++) {
    mem_item_t *item = (mem_item_t *)(chunk + i * slot->block_size);
    item->next = slot->free_list;
    slot->free_list = item;
  }
}

void kvs_init_slots(void) {
  if (g_inited)
    return;
  for (int i = 0; i < SLOT_NUM; i++) {
    g_slots[i].block_size = (i + 1) * MEM_ALIGN;
    g_slots[i].free_list = NULL;
    g_slots[i].chunks = NULL; // 初始化区间链表
  }
  g_inited = 1;
}

void *kvs_malloc(size_t size) {
  if (size == 0)
    return NULL;

  if (size <= MEM_POOL_MAX_SIZE) {
#if ENABLE_MEM_POOL
    if (g_inited) {
      size_t align_sz = align_size(size);
      int idx = get_slot_idx(align_sz);
      if (idx >= 0 && idx < SLOT_NUM) {
        mem_slot_t *slot = &g_slots[idx];
        if (!slot->free_list) {
          expand_slot(slot);
        }
        if (slot->free_list) {
          mem_item_t *item = slot->free_list;
          slot->free_list = item->next;
          memset(item, 0, size); // 清零后返回
          return item;
        }
      }
    }
#endif
  }

  // 超大或内存池禁用时直接使用 malloc
  void *ptr = malloc(size);
  if (ptr)
    memset(ptr, 0, size);
  return ptr;
}

// 修正后的释放函数 —— 能够正确将池内内存放回 free_list
void kvs_free(void *ptr) {
  if (!ptr)
    return;

  // 遍历所有槽位，检查指针属于哪个大块区间
  for (int i = 0; i < SLOT_NUM; i++) {
    mem_slot_t *slot = &g_slots[i];
    mem_chunk_t *c = slot->chunks;
    while (c) {
      if ((char *)ptr >= c->start && (char *)ptr < c->end) {
        // 验证偏移量是否是 block_size 的整数倍（确保是池内对象）
        size_t offset = (char *)ptr - c->start;
        if (offset % slot->block_size == 0) {
          // 正确归属，放回空闲链表
          mem_item_t *item = (mem_item_t *)ptr;
          item->next = slot->free_list;
          slot->free_list = item;
          return;
        }
        // 如果偏移不对齐，说明不是池内对象，跳出内层循环继续检查其他槽位
        break;
      }
      c = c->next;
    }
  }

  // 不归属于任何池内存块，则用系统 free 释放
  free(ptr);
}