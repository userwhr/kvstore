#include "config.h"

// 全局配置实例
KvConfig g_config;

// 工具函数：去除字符串首尾空白字符
static char *trim_whitespace(char *str) {
  if (str == NULL)
    return NULL;

  // 去除首部空格
  while (isspace((unsigned char)*str))
    str++;

  // 去除尾部空格
  char *end = str + strlen(str) - 1;
  while (end > str && isspace((unsigned char)*end))
    end--;
  *(end + 1) = '\0';

  return str;
}

// 工具函数：去除字符串两端的引号（处理配置文件中的"xxx"）
static void trim_quotes(char *str) {
  if (str[0] == '"' || str[0] == '\'') {
    memmove(str, str + 1, strlen(str));
    size_t len = strlen(str);
    if (len > 0 && (str[len - 1] == '"' || str[len - 1] == '\'')) {
      str[len - 1] = '\0';
    }
  }
}

// 解析单行配置
static void parse_line(char *line, KvConfig *cfg) {
  line = trim_whitespace(line);
  if (line[0] == '#' || line[0] == '\0')
    return; // 跳过注释/空行

  // 分割 key 和 value
  char *equal = strchr(line, '=');
  if (equal == NULL)
    return;

  *equal = '\0';
  char *key = trim_whitespace(line);
  char *value = trim_whitespace(equal + 1);

  // ====================== 字符串配置项 ======================
  if (strcmp(key, "LOG_PATH") == 0) {
    trim_quotes(value);
    strncpy(cfg->log_path, value, sizeof(cfg->log_path) - 1);
  } else if (strcmp(key, "LOG_PATH_ICR") == 0) {
    trim_quotes(value);
    strncpy(cfg->log_path_icr, value, sizeof(cfg->log_path_icr) - 1);
  } else if (strcmp(key, "CMD_DELIM") == 0) {
    trim_quotes(value);
    strncpy(cfg->cmd_delim, value, sizeof(cfg->cmd_delim) - 1);
  }

  // ====================== 整型配置项 ======================
  else if (strcmp(key, "ENABLE_FULL_PERSIST") == 0)
    cfg->enable_full_persist = atoi(value);
  else if (strcmp(key, "ENABLE_INCREMENTAL_PERSIST") == 0)
    cfg->enable_incremental_persist = atoi(value);
  else if (strcmp(key, "MAX_RECOVER_CMD") == 0)
    cfg->max_recover_cmd = atoi(value);
  else if (strcmp(key, "LINE_MAX") == 0)
    cfg->line_max = atoi(value);
  else if (strcmp(key, "DEFAULT_CAPACITY") == 0)
    cfg->default_capacity = atoi(value);
  else if (strcmp(key, "MMAP_GROW_FACTOR") == 0)
    cfg->mmap_grow_factor = atoi(value);
  else if (strcmp(key, "BUFFER_LENGTH") == 0)
    cfg->buffer_length = atoi(value);
  else if (strcmp(key, "MAX_CMD_NUM") == 0)
    cfg->max_cmd_num = atoi(value);
  else if (strcmp(key, "ENABLE_RBTREE") == 0)
    cfg->enable_rbtree = atoi(value);
  else if (strcmp(key, "ENABLE_ARRAY") == 0)
    cfg->enable_array = atoi(value);
  else if (strcmp(key, "ENABLE_HASH") == 0)
    cfg->enable_hash = atoi(value);
  else if (strcmp(key, "ENABLE_SKIPLIST") == 0)
    cfg->enable_skiplist = atoi(value);
  else if (strcmp(key, "ENABLE_SLAB_POOL") == 0)
    cfg->enable_slab_pool = atoi(value);
  else if (strcmp(key, "PORT") == 0)
    cfg->port = atoi(value);
}

// 加载配置文件
int load_config(const char *config_path) {
  // 初始化默认值
  memset(&g_config, 0, sizeof(KvConfig));

  // 设置默认值
  strcpy(g_config.log_path, "./log/full_log.txt");
  strcpy(g_config.log_path_icr, "./log/icr_log.txt");
  strcpy(g_config.cmd_delim, "&&");
  g_config.line_max = 1048576;
  g_config.max_cmd_num = 10000;
  g_config.enable_rbtree = 1;
  g_config.enable_array = 1;
  g_config.enable_hash = 1;
  g_config.enable_skiplist = 1;
  g_config.enable_slab_pool = 1;
  g_config.port = 6379;

  FILE *fp = fopen(config_path, "r");
  if (fp == NULL) {
    perror("fopen config file failed");
    return -1;
  }

  char line[1024];
  while (fgets(line, sizeof(line), fp) != NULL) {
    parse_line(line, &g_config);
  }

  fclose(fp);
  return 0;
}