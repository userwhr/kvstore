#include "config.h"
#include <strings.h>

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

// 工具函数：将配置值转为整数（支持 "1"/"0" 和 "yes"/"no"）
static int parse_bool_value(const char *value) {
  if (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0)
    return 1;
  if (strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0)
    return 0;
  return atoi(value);
}

// 解析单行配置（格式：KEY VALUE，空格分隔，支持行尾 # 注释）
static void parse_line(char *line, KvConfig *cfg) {
  line = trim_whitespace(line);
  if (line[0] == '#' || line[0] == '\0')
    return; // 跳过注释/空行

  // 分割 key 和 value（按第一个空格/制表符分割）
  char *sep = line;
  while (*sep && !isspace((unsigned char)*sep))
    sep++;
  if (*sep == '\0')
    return; // 没有 value，跳过

  *sep = '\0';
  char *key = trim_whitespace(line);
  char *value = trim_whitespace(sep + 1);

  // 去掉行尾 # 注释
  char *comment = strchr(value, '#');
  if (comment) {
    // 确保 # 前面是空格，避免误切（如值本身含 #）
    if (comment == value || isspace((unsigned char)*(comment - 1))) {
      *comment = '\0';
      value = trim_whitespace(value);
    }
  }

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
    cfg->enable_full_persist = parse_bool_value(value);
  else if (strcmp(key, "ENABLE_INCREMENTAL_PERSIST") == 0)
    cfg->enable_incremental_persist = parse_bool_value(value);
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
    cfg->enable_rbtree = parse_bool_value(value);
  else if (strcmp(key, "ENABLE_ARRAY") == 0)
    cfg->enable_array = parse_bool_value(value);
  else if (strcmp(key, "ENABLE_HASH") == 0)
    cfg->enable_hash = parse_bool_value(value);
  else if (strcmp(key, "ENABLE_SKIPLIST") == 0)
    cfg->enable_skiplist = parse_bool_value(value);
  else if (strcmp(key, "ENABLE_SLAVE") == 0)
    cfg->enable_slave = parse_bool_value(value);
  else if (strcmp(key, "SLAVE_OF_IP") == 0) {
    trim_quotes(value);
    strncpy(cfg->slave_of_ip, value, sizeof(cfg->slave_of_ip) - 1);
  } else if (strcmp(key, "SLAVE_OF_PORT") == 0)
    cfg->slave_of_port = atoi(value);
  else if (strcmp(key, "SLAVE_RDMA_PORT") == 0)
    cfg->slave_rdma_port = atoi(value);
  else if (strcmp(key, "PORT") == 0)
    cfg->port = atoi(value);
  else if (strcmp(key, "MAX_SLAVES") == 0)
    cfg->max_slaves = atoi(value);
  if (cfg->enable_slave == 1) {
    cfg->enable_incremental_persist = 0;
    cfg->enable_full_persist = 0;
  }
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
  g_config.port = 6379;
  g_config.max_slaves = 16;
  // Slave 默认配置
  strcpy(g_config.slave_of_ip, "127.0.0.1");
  g_config.slave_of_port = 6379;
  g_config.slave_rdma_port = 6380;

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