#ifndef CONFIG_H
#define CONFIG_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 配置结构体：替代你原来的所有宏
typedef struct {
  // 持久化
  int enable_full_persist;
  int enable_incremental_persist;

  // 路径（固定大小，避免内存泄漏）
  char log_path[256];
  char log_path_icr[256];
  char cmd_delim[16];

  // 数值配置
  int max_recover_cmd;
  int line_max;
  int default_capacity;
  int mmap_grow_factor;
  int buffer_length;
  int max_cmd_num;

  // 存储引擎
  int enable_rbtree;
  int enable_array;
  int enable_hash;
  int enable_skiplist;

  // 内存池
  int enable_slab_pool;

  // 端口
  int port;
} KvConfig;

// 全局配置变量（替代宏，程序中直接使用）
extern KvConfig g_config;

/**
 * @brief 加载并解析配置文件
 * @param config_path 配置文件路径（如 "./kvstore.conf"）
 * @return 0成功，-1失败
 */
int load_config(const char *config_path);

#endif // CONFIG_H