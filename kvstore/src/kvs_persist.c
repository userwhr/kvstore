#include "config.h"
#include "kvstore.h"
#include "resp.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h> // 新增：用于文件mode定义
#include <unistd.h>

// 文件恢复指令枚举（和你的网络指令一一对应）
enum {
  FILE_CMD_START = 0, // 指令起始位

  // 数组结构指令
  FILE_SET = FILE_CMD_START,
  FILE_DEL,
  FILE_MOD,

  // 红黑树结构指令
  FILE_RSET,
  FILE_RDEL,
  FILE_RMOD,

  // 哈希表结构指令
  FILE_HSET,
  FILE_HDEL,
  FILE_HMOD,

  // 跳表结构指令
  FILE_SKSET,
  FILE_SKDEL,
  FILE_SKMOD,

  FILE_CMD_MAX // 指令总数（用于循环匹配）
};
// 【必须配套】指令名称数组（和上面枚举一一对应）
const char *file_command[] = {"set",  "del",   "mod",   "rset",
                              "rdel", "rmod",  "hset",  "hdel",
                              "hmod", "skset", "skdel", "skmod"};
/**
 * @brief 初始化：仅打开文件 + 保存基础信息（只open一次）
 * @param filename 文件名
 * @param fc 待初始化的结构体
 * @return 0成功 -1失败
 */
int fcInit(const char *filename, file_content_t *fc) {
  // 1. 先判空！！！
  if (fc == NULL || filename == NULL) {
    return -1;
  }

  // 2. 初始化结构体
  memset(fc, 0, sizeof(file_content_t));

  // 3. 保存文件名（分配内存）
  fc->filename = kvs_malloc(strlen(filename) + 1);
  if (fc->filename == NULL) {
    perror("malloc filename failed");
    return -1;
  }
  strcpy(fc->filename, filename);

  // 4. 打开文件
  fc->file_fd = open(filename, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fc->file_fd == -1) {
    perror("open failed");
    free(fc->filename);
    fc->filename = NULL;
    return -1;
  }

  // ===================== 新增：获取文件大小 =====================
  struct stat file_stat;
  // fstat：通过文件描述符获取文件元信息（大小、权限、时间等）
  if (fstat(fc->file_fd, &file_stat) == -1) {
    perror("fstat get file size failed");
    // 失败必须清理资源！关闭fd、释放文件名内存
    close(fc->file_fd);
    free(fc->filename);
    fc->filename = NULL;
    fc->file_fd = -1;
    return -1;
  }

  // 赋值文件真实大小到结构体
  fc->file_size = (size_t)file_stat.st_size;
  // ============================================================

  // printf("✅ fcInit：文件打开成功，fd=%d，文件大小=%zu 字节\n", fc->file_fd,
  //        fc->file_size);
  return 0;
}

/**
 * @brief 映射文件：直接复用fc中已打开的fd，不重新open！！！
 * @param fc 已完成fcInit的结构体
 * @return 0成功 -1失败
 */
int map_file_to_struct(file_content_t *fc) {
  // 1. 合法性校验
  if (fc == NULL || fc->file_fd == -1) {
    fprintf(stderr, "map_file_to_struct：未初始化或fd无效\n");
    return -1;
  }

  // 2. 直接复用已有fd，获取文件大小（无open！）
  struct stat st;
  if (fstat(fc->file_fd, &st) == -1) {
    perror("fstat failed");
    return -1;
  }
  fc->file_size = st.st_size;

  // 3. 文件为空，直接返回（无需mmap）
  if (fc->file_size == 0) {
    printf("Void file no need mmap\n");
    return 0;
  }

  // 4. 直接复用fd做mmap映射（核心：无open，纯复用）
  fc->file_data = mmap(NULL, fc->file_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fc->file_fd, // ✅ 复用init中打开的fd
                       0);

  if (fc->file_data == MAP_FAILED) {
    perror("mmap failed");
    return -1;
  }

  return 0;
}
/**
 * @brief mmap文件恢复指令执行（仅执行SET/DEL/MOD，无回包、无持久化）
 * @param tokens 解析后的指令参数（tokens[0]=命令, tokens[1]=key,
 * tokens[2]=value）
 * @return 0 执行完成
 */
static int mmapProtocal(char *tokens[]) {
  // 安全校验
  if (tokens == NULL || tokens[0] == NULL) {
    fprintf(stderr, "mmap恢复：无效指令参数\n");
    return -1;
  }

  // 匹配指令类型（完全复用你的协议逻辑）
  int cmd = FILE_CMD_START;
  for (cmd = FILE_CMD_START; cmd < FILE_CMD_MAX; cmd++) {
    if (strcmp(tokens[0], file_command[cmd]) == 0) {
      break;
    }
  }

  // 仅执行内存操作，无持久化、无响应回发
  switch (cmd) {
  // 数组：SET/DEL/MOD
  case FILE_SET:
    kvs_array_set(global_array, tokens[1], tokens[2]);
    break;
  case FILE_DEL:
    kvs_array_del(global_array, tokens[1]);
    break;
  case FILE_MOD:
    kvs_array_mod(global_array, tokens[1], tokens[2]);
    break;

  // 红黑树：RSET/RDEL/RMOD
  case FILE_RSET:
    kvs_rbtree_set(global_rbtree, tokens[1], tokens[2]);
    break;
  case FILE_RDEL:
    kvs_rbtree_del(global_rbtree, tokens[1]);
    break;
  case FILE_RMOD:
    kvs_rbtree_mod(global_rbtree, tokens[1], tokens[2]);
    break;

  // 哈希表：HSET/HDEL/HMOD
  case FILE_HSET:
    kvs_hash_set(global_hash, tokens[1], tokens[2]);
    break;
  case FILE_HDEL:
    kvs_hash_del(global_hash, tokens[1]);
    break;
  case FILE_HMOD:
    kvs_hash_mod(global_hash, tokens[1], tokens[2]);
    break;

  // 跳表：SKSET/SKDEL/SKMOD
  case FILE_SKSET:
    kvs_skiplist_set(global_skiplist, tokens[1], tokens[2]);
    break;
  case FILE_SKDEL:
    kvs_skiplist_del(global_skiplist, tokens[1]);
    break;
  case FILE_SKMOD:
    kvs_skiplist_mod(global_skiplist, tokens[1], tokens[2]);
    break;

  default:
    fprintf(stderr, "mmap恢复：不支持的指令 -> %s\n", tokens[0]);
    break;
  }

  return 0;
}
// 🔥 修复后：支持带空格、任意字符的Key/Value
int parse_kv_line(char *line, char *key, char *value, int key_size,
                  int val_size) {
  int klen, vlen;
  // 1. 先读取  key长度 和 value长度
  if (sscanf(line, "%d %d", &klen, &vlen) != 2) {
    fprintf(stderr, "错误：无法解析长度\n");
    return -1;
  }

  // 安全校验：防止缓冲区溢出
  if (klen <= 0 || vlen <= 0 || klen >= key_size || vlen >= val_size) {
    fprintf(stderr, "错误：长度非法\n");
    return -1;
  }

  // 2. 定位到数据起始位置（跳过 "klen vlen "）
  char *data = line;
  while (*data != ' ')
    data++; // 跳过第一个数字
  data++;
  while (*data != ' ')
    data++; // 跳过第二个数字
  data++;   // 指向真正的key+value数据

  // 3. 精准拷贝key和value（带空格也完美保留）
  memcpy(key, data, klen);
  key[klen] = '\0';
  memcpy(value, data + klen, vlen);
  value[vlen] = '\0';

  return 0;
}
// 配套释放函数（必须调用，防止内存/文件句柄泄漏）
void free_file_content(file_content_t *fc) {
  if (fc == NULL)
    return;

  // 1. 解除mmap映射
  if (fc->file_data != NULL && fc->file_size > 0) {
    munmap(fc->file_data, fc->file_size);
    fc->file_data = NULL;
  }

  // 2. 关闭文件描述符
  if (fc->file_fd != -1) {
    close(fc->file_fd);
    fc->file_fd = -1;
  }

  // 3. 释放文件名内存
  if (fc->filename != NULL) {
    free(fc->filename);
    fc->filename = NULL;
  }

  // 4. 清空结构体
  memset(fc, 0, sizeof(file_content_t));
  printf("✅ 资源释放完成\n");
}

/**
 * @brief 从mmap映射的文件中读取所有resp指令(set/del/mod)并执行
 * @param fc 已mmap映射完成的文件结构体
 * @return 0成功，-1失败
 */
int AOF_commands_from_mmap(file_content_t *fc) {
  // 空文件直接返回
  if (fc->file_size == 0 || fc->file_data == NULL) {
    printf("NO CMD IN AOF\n");
    return 0;
  }

  // 数据源：mmap映射的文件指令区 + 游标（替代原网络接收缓冲区）
  char *mmap_data = (char *)fc->file_data;
  size_t total_size = fc->file_size;
  size_t processed = 0; // 已处理的指令字节数

  // ====================== 完全复用你的原解析+执行循环逻辑
  // ======================
  while (processed < total_size) {
    char *curr_data = mmap_data + processed; // 当前未处理的指令起始位置
    int oneRespLen = getOneRespLen(curr_data);

    // 无效指令/解析失败，退出
    if (oneRespLen <= 0 || (processed + oneRespLen) > total_size) {
      fprintf(stderr, "❌ 指令解析失败，跳过剩余数据\n");
      break;
    }

    // -------------- 复用：指令解析 --------------
    char *cmd = NULL;
    char *params[10] = {0};
    int paramCnt = 0;
    StoreType type;
    int parseRet = parseRespKv(curr_data, &cmd, &params[0], &paramCnt, &type);

    if (parseRet == -1)
      break;
    if (parseRet < 0) {
      fprintf(stderr, "❌ 指令格式错误，终止执行\n");
      return -1;
    }

    // -------------- 复用：构造参数数组 --------------
    char *tokens[20] = {0};
    tokens[0] = cmd;
    for (int i = 0; i < paramCnt; i++)
      tokens[i + 1] = params[i];

    // 核心执行
    mmapProtocal(tokens);

    // -------------- 复用：释放解析内存 --------------
    kvs_free(cmd);
    for (int i = 0; i < paramCnt; i++)
      kvs_free(params[i]);

    // -------------- 游标前移：消耗已处理指令 --------------
    processed += oneRespLen;
  }
  return 0;
}

int snapshot_restore_from_mmap(file_content_t *fc) {
  if (!fc || fc->file_data == MAP_FAILED || fc->file_size == 0) {
    printf("⚠️  空文件或映射失败，无需恢复\n");
    return 0;
  }

  char *data = (char *)fc->file_data;
  size_t size = fc->file_size;
  size_t pos = 0;

  char current_type[32] = {0};
  int expected_count = 0;
  int loaded_count = 0;

  while (pos < size) {
    char *line_start = data + pos;
    size_t remain = size - pos;
    char *line_end = memchr(line_start, '\n', remain);
    size_t line_len = line_end ? (line_end - line_start) : remain;

    char line[1024] = {0}; // 初始化缓冲区
    if (line_len >= sizeof(line)) {
      fprintf(stderr, "行过长，无法处理\n");
      return -1;
    }
    memcpy(line, line_start, line_len);
    line[line_len] = '\0';

    if (line_len > 0 && line[line_len - 1] == '\r') {
      line[line_len - 1] = '\0';
      line_len--;
    }

    if (line_end) {
      pos += line_len + 1;
    } else {
      pos += line_len;
    }

    if (line_len == 0)
      continue;

    // ===================== 核心修复 =====================
    // 1. 解析字符串+数字
    char type_buf[32] = {0};
    int count;
    if (sscanf(line, "%31s %d", type_buf, &count) == 2) {

      // 2. 只允许合法类型！过滤掉 1 1 这种键值对！
      if (strcmp(type_buf, "array") == 0 || strcmp(type_buf, "rbtree") == 0 ||
          strcmp(type_buf, "hash") == 0 || strcmp(type_buf, "skiplist") == 0) {

        // 3. 先赋值，再打印！
        strncpy(current_type, type_buf, sizeof(current_type) - 1);
        expected_count = count;
        loaded_count = 0;
        // printf("✅ 识别类型：%s\n", current_type); // 正确打印
        continue;
      }
    }
    // ====================================================

    // 无类型标记 → 错误
    if (current_type[0] == '\0') {
      fprintf(stderr, "错误：在类型行之前遇到键值对\n");
      return -1;
    }

    // 解析键值对
    char key[512] = {0}, value[512] = {0};
    if (parse_kv_line(line, key, value, 512, 512) != 0) {
      fprintf(stderr, "解析失败：%s\n", line);
      return -1;
    }

    // ===================== 测试打印（必执行！） =====================
    // printf("🎯 处理键值：类型=%s | key=%s | value=%s\n", current_type, key,
    //        value);

    // 插入数据结构（无冗余校验）
    if (strcmp(current_type, "array") == 0) {
      kvs_array_set(global_array, key, value);
    } else if (strcmp(current_type, "rbtree") == 0) {
      kvs_rbtree_set(global_rbtree, key, value);
    } else if (strcmp(current_type, "hash") == 0) {
      kvs_hash_set(global_hash, key, value);
    } else if (strcmp(current_type, "skiplist") == 0) {
      kvs_skiplist_set(global_skiplist, key, value);
    }

    loaded_count++;
  }
  return 0;
}