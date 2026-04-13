#ifndef KVS_MEM_POOL_H
#define KVS_MEM_POOL_H

#include <stddef.h>
#include <stdlib.h>

#define MEM_POOL_MAX_SIZE 128
#define MEM_ALIGN 8
#define SLOT_NUM (MEM_POOL_MAX_SIZE / MEM_ALIGN)
#define ENABLE_MEM_POOL 1

void kvs_init_slots(void);
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);

#endif