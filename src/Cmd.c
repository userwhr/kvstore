#include <string.h>
#include <strings.h>
#include <stdio.h>

// 指令集（和宏下标严格对应）
const char *command[] = {"set",   "get",     "del",   "mod",   "exist",
                         "rset",  "rget",    "rdel",  "rmod",  "rexist",
                         "hset",  "hget",    "hdel",  "hmod",  "hexist",
                         "skset", "skget",   "skdel", "skmod", "skexist",
                         "save",  "COMMAND", "SYNC", "echo", "flushall", "dbcount", "ping", "config"};
int COMMAND_COUNT = (sizeof(command) / sizeof(command[0]));

/* ===== 命令哈希表：O(1) 查找 ===== */
#define CMD_HASH_SIZE 64  /* 2 的幂，大于 COMMAND_COUNT 的两倍 */

typedef struct {
  const char *name;
  int index;
  int used;
} cmd_hash_entry_t;

static cmd_hash_entry_t cmd_hash_table[CMD_HASH_SIZE];
static int cmd_hash_initialized = 0;

/* FNV-1a 哈希函数 */
static unsigned int cmd_hash(const char *str) {
  unsigned int hash = 2166136261u;
  while (*str) {
    hash ^= (unsigned char)*str;
    hash *= 16777619u;
    str++;
  }
  return hash & (CMD_HASH_SIZE - 1);
}

/* 初始化命令哈希表 */
void cmdHashTableInit(void) {
  if (cmd_hash_initialized) return;
  memset(cmd_hash_table, 0, sizeof(cmd_hash_table));
  for (int i = 0; i < COMMAND_COUNT; i++) {
    /* 转小写后计算哈希，保证大小写不敏感 */
    char lower[32];
    int len = strlen(command[i]);
    if (len >= (int)sizeof(lower)) continue;
    for (int j = 0; j < len; j++)
      lower[j] = (command[i][j] >= 'A' && command[i][j] <= 'Z') ? command[i][j] + 32 : command[i][j];
    lower[len] = '\0';

    unsigned int idx = cmd_hash(lower);
    /* 开放寻址法解决冲突 */
    while (cmd_hash_table[idx].used) {
      idx = (idx + 1) & (CMD_HASH_SIZE - 1);
    }
    cmd_hash_table[idx].name = command[i];
    cmd_hash_table[idx].index = i;
    cmd_hash_table[idx].used = 1;
  }
  cmd_hash_initialized = 1;
}

/* O(1) 命令查找，返回命令索引，-1 表示不存在 */
int cmdHashLookup(const char *cmd, int cmd_len) {
  char buf[32];
  if (cmd_len >= (int)sizeof(buf)) return -1;
  /* 转小写，保证大小写不敏感 */
  for (int i = 0; i < cmd_len; i++)
    buf[i] = (cmd[i] >= 'A' && cmd[i] <= 'Z') ? cmd[i] + 32 : cmd[i];
  buf[cmd_len] = '\0';

  unsigned int idx = cmd_hash(buf);
  while (cmd_hash_table[idx].used) {
    if (strcasecmp(cmd_hash_table[idx].name, buf) == 0) {
      return cmd_hash_table[idx].index;
    }
    idx = (idx + 1) & (CMD_HASH_SIZE - 1);
  }
  return -1;
}