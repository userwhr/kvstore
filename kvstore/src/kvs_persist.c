#include "kvstore.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h> // 新增：用于文件mode定义
#include <unistd.h>
const char *file_command[] = {"set", "rset", "hset", "skset"};
enum {
  FILE_CMD = 0,
  FILE_SET = FILE_CMD,
  FILE_RSET = 1,
  FILE_HSET = 2,
  FILE_SKSET = 3,
};
// 🔥 只扩容匿名映射（无文件、无Bus error）
static int resize_anon_mmap(file_content_t *fc) {
  size_t new_cap = fc->anon_cap * MMAP_GROW_FACTOR;
  // 匿名映射扩容（MAP_ANON | MAP_PRIVATE，无文件）
  void *new_anon = mmap(NULL, new_cap, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
  if (new_anon == MAP_FAILED) {
    perror("anon mmap扩容失败");
    return -1;
  }
  // 拷贝旧数据
  if (fc->anon_data) {
    memcpy(new_anon, fc->anon_data, fc->used_len);
    munmap(fc->anon_data, fc->anon_cap);
  }
  fc->anon_data = new_anon;
  fc->anon_cap = new_cap;
  printf("📈 匿名区扩容：%zu → %zu（无文件、无Bus error）\n",
         fc->anon_cap / MMAP_GROW_FACTOR, fc->anon_cap);
  return 0;
}

// 初始化：文件只读映射（不越界）+ 匿名可写映射（安全）
int map_file_to_struct(const char *filename, file_content_t *fc) {
  memset(fc, 0, sizeof(file_content_t));
  int fd = open(filename, O_RDWR | O_CREAT, 0644);
  if (fd == -1) {
    perror("open failed");
    return -1;
  }
  fc->file_fd = fd;

  // 1. 获取文件真实大小（只读）
  struct stat st;
  if (fstat(fd, &st) == -1) {
    perror("fstat failed");
    close(fd);
    return -1;
  }
  fc->file_size = st.st_size;
  fc->used_len = fc->file_size;

  // 2. 文件只读映射（只映射文件真实大小，绝不越界）
  if (fc->file_size > 0) {
    fc->file_data = mmap(NULL, fc->file_size,
                         PROT_READ, // 只读！防越界写
                         MAP_SHARED, fd, 0);
    if (fc->file_data == MAP_FAILED) {
      perror("file mmap failed");
      close(fd);
      return -1;
    }
  }
  int anon_cap = DEFAULT_CAPACITY;
  while (anon_cap < fc->file_size) {
    anon_cap *= MMAP_GROW_FACTOR;
  }
  // 3. 初始化匿名可写映射（安全区）
  fc->anon_cap = anon_cap;
  fc->anon_data = mmap(NULL, fc->anon_cap, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
  if (fc->anon_data == MAP_FAILED) {
    perror("anon mmap failed");
    close(fd);
    return -1;
  }

  // 4. 把文件数据拷贝到匿名区（后续只操作匿名区）
  if (fc->file_size > 0) {
    memcpy(fc->anon_data, fc->file_data, fc->file_size);
  }

  fc->filename = kvs_malloc(strlen(filename) + 1);
  strcpy(fc->filename, filename);

  printf("✅ 初始化：文件大小=%zu | 匿名区容量=%zu | 有效数据=%zu\n",
         fc->file_size, fc->anon_cap, fc->used_len);
  return 0;
}
// 追加：只写匿名区，自动扩容匿名区（无文件、无Bus error）
int append_mmap_cmd(file_content_t *fc, const char *cmd_type, const char *key,
                    const char *value) {
  if (!fc || !cmd_type || !key || !value)
    return -1;

  char new_line[LINE_MAX];
  int new_len = snprintf(new_line, sizeof(new_line), "%s %s %s\r\n", cmd_type,
                         key, value);

  // 匿名区空间不足 → 只扩容匿名区（无文件）
  while (fc->used_len + new_len > fc->anon_cap) {
    if (resize_anon_mmap(fc) == -1)
      return -1;
  }

  // 🔥 只写匿名区（绝对安全，无Bus error）
  memcpy((char *)fc->anon_data + fc->used_len, new_line, new_len);
  fc->used_len += new_len;

  printf("✅ 追加成功：%s", new_line);
  return 0;
}

// 删除：只操作匿名区
int delete_mmap_cmd(file_content_t *fc, const char *del_type, const char *key) {
  if (!fc || !del_type || !key || fc->used_len == 0)
    return -1;

  const char *prefix = NULL;
  if (!strcmp(del_type, "del"))
    prefix = "set";
  else if (!strcmp(del_type, "rdel"))
    prefix = "rset";
  else if (!strcmp(del_type, "hdel"))
    prefix = "hset";
  else if (!strcmp(del_type, "skdel"))
    prefix = "skset";
  else
    return -1;

  char *curr = fc->anon_data;
  char *end = (char *)fc->anon_data + fc->used_len;
  char *line_end;

  while ((line_end = memmem(curr, end - curr, "\r\n", 2)) && curr < end) {
    size_t line_len = line_end - curr;
    if (line_len == 0) {
      curr = line_end + 2;
      continue;
    }

    char line[LINE_MAX];
    memcpy(line, curr, line_len);
    line[line_len] = '\0';
    char *p = strtok(line, " "), *k = strtok(NULL, " ");

    if (p && k && !strcmp(p, prefix) && !strcmp(k, key)) {
      size_t del_len = line_len + 2;
      memmove(curr, line_end + 2, end - (line_end + 2));
      fc->used_len -= del_len;
      // printf("✅ 删除成功\n");
      return 0;
    }
    curr = line_end + 2;
  }
  fprintf(stderr, "❌ 未找到指令\n");
  return 1;
}

// 修改：只操作匿名区
int modify_mmap_cmd(file_content_t *fc, const char *mod_type, const char *key,
                    const char *new_value) {
  if (!fc || !mod_type || !key || !new_value || fc->used_len == 0)
    return -1;

  const char *prefix = NULL;
  if (!strcmp(mod_type, "mod"))
    prefix = "set";
  else if (!strcmp(mod_type, "rmod"))
    prefix = "rset";
  else if (!strcmp(mod_type, "hmod"))
    prefix = "hset";
  else if (!strcmp(mod_type, "skmod"))
    prefix = "skset";
  else
    return -1;

  char new_line[LINE_MAX];
  int new_len = snprintf(new_line, sizeof(new_line), "%s %s %s\r\n", prefix,
                         key, new_value);

  char *curr = fc->anon_data;
  char *end = (char *)fc->anon_data + fc->used_len;
  char *line_end;

  while ((line_end = memmem(curr, end - curr, "\r\n", 2)) && curr < end) {
    size_t old_line_len = line_end - curr;
    if (old_line_len == 0) {
      curr = line_end + 2;
      continue;
    }

    char old_line[LINE_MAX];
    memcpy(old_line, curr, old_line_len);
    old_line[old_line_len] = '\0';
    char *p = strtok(old_line, " "), *k = strtok(NULL, " ");

    if (p && k && !strcmp(p, prefix) && !strcmp(k, key)) {
      size_t old_total_len = old_line_len + 2;
      int expand = new_len - old_total_len;

      // 新指令更长 → 只扩容匿名区
      while (expand > 0 && fc->used_len + expand > fc->anon_cap) {
        if (resize_anon_mmap(fc) == -1)
          return -1;
      }

      if (expand > 0) {
        memmove(curr + new_len, line_end + 2, end - (line_end + 2));
        fc->used_len += expand;
      }

      memcpy(curr, new_line, new_len);
      if (new_len < old_total_len) {
        memset(curr + new_len, 0, old_total_len - new_len);
        fc->used_len -= (old_total_len - new_len);
      }
      // printf("✅ 修改成功\n");
      return 0;
    }
    curr = line_end + 2;
  }
  fprintf(stderr, "❌ 未找到指令\n");
  return 1;
}

// 落盘：只把匿名区有效数据写回文件（不扩文件、无ftruncate）
int sync_mmap_to_file(file_content_t *fc) {
  if (!fc || !fc->anon_data)
    return -1;

  // 1. 把文件指针移到开头
  lseek(fc->file_fd, 0, SEEK_SET);
  // 2. 只写有效数据（不扩文件，文件大小=used_len）
  ssize_t written = write(fc->file_fd, fc->anon_data, fc->used_len);
  if (written != (ssize_t)fc->used_len) {
    perror("write failed");
    return -1;
  }
  // 3. 截断文件到有效长度（可选，不影响）
  ftruncate(fc->file_fd, fc->used_len); // 这里用也没事，只缩不扩
  fsync(fc->file_fd);

  printf("✅ 落盘成功：文件大小=%zu（未扩容）\n", fc->used_len);
  return 0;
}

void unmap_file(file_content_t *fc) {
  if (!fc)
    return;
  if (fc->file_data)
    munmap(fc->file_data, fc->file_size);
  if (fc->anon_data)
    munmap(fc->anon_data, fc->anon_cap);
  if (fc->file_fd != -1)
    close(fc->file_fd);
  if (fc->filename)
    kvs_free(fc->filename);
  memset(fc, 0, sizeof(file_content_t));
  printf("✅ 释放完成\n");
}

/**
 *
 *
 *
 *
 *
 *
 *
 *
 */
// 以下为恢复文件指令的函数
int set_protocal(char *tokens[]) {
  int cmd = FILE_CMD;
  // 匹配指令类型
  for (cmd = FILE_CMD; cmd < FILE_SKSET; cmd++) {
    if (strcmp(tokens[0], file_command[cmd]) == 0) {
      break;
    }
  }

  int ret = 1;
  // 执行对应数据结构的set操作（修复宏定义）
  switch (cmd) {
#if ENABLE_ARRAY
  case FILE_SET:
    ret = kvs_array_set(global_array, tokens[1], tokens[2]);
    return 0;
#endif
#if ENABLE_RBTREE
  case FILE_RSET:
    printf("rset %s %s", tokens[1], tokens[2]);
    ret = kvs_rbtree_set(global_rbtree, tokens[1], tokens[2]);
    return 0;
#endif
#if ENABLE_HASH
  case FILE_HSET:
    ret = kvs_hash_set(global_hash, tokens[1], tokens[2]);
    return 0;
#endif
#if ENABLE_SKIPLIST
  case FILE_SKSET:
    ret = kvs_skiplist_set(global_skiplist, tokens[1], tokens[2]);
    return 0;
#endif
  default:
    fprintf(stderr, "不支持的恢复指令：%s\n", tokens[0]);
    return 0;
  }
  return ret;
}
static void free_kv_recover_result(KvRecoverResult *result) {
  if (result == NULL) {
    return;
  }
  if (result->cmds != NULL) {
    for (int i = 0; i < result->cmd_count; i++) {
      if (result->cmds[i] != NULL) {
        kvs_free(result->cmds[i]);
      }
    }
    kvs_free(result->cmds);
  }
  kvs_free(result);
}
/**
 * @brief 从日志文件获得KV指令,返回包含指令数和指令内容的结构体指针
 * @return 成功：返回KvRecoverResult结构体指针（需调用者手动释放)；
 *         失败/空文件：返回NULL（空文件可通过perror排查,或补充日志)
 */
// 恢复：只读文件真实大小，无null，不读mmap多余空间
KvRecoverResult *kvs_recover_data(const char *file) {
  KvRecoverResult *result = kvs_malloc(sizeof(KvRecoverResult));
  result->cmd_count = 0;
  result->cmds = NULL;

  int fd = open(file, O_RDONLY);
  if (fd == -1) {
    printf("空文件\n");
    return result;
  }

  // 🔥 只读文件真实大小（无null）
  struct stat st;
  fstat(fd, &st);
  size_t file_size = st.st_size;
  if (file_size == 0) {
    close(fd);
    return result;
  }

  // 只读取有效数据，无预分配null
  char *buf = kvs_malloc(file_size + 1);
  read(fd, buf, file_size);
  close(fd);
  buf[file_size] = '\0';

  // 按行分割指令
  result->cmds = kvs_malloc(MAX_RECOVER_CMD * sizeof(char *));
  char *start = buf;
  char *sep;
  while ((sep = strstr(start, "\r\n"))) {
    int len = sep - start;
    result->cmds[result->cmd_count] = kvs_malloc(len + 1);
    strncpy(result->cmds[result->cmd_count], start, len);
    result->cmds[result->cmd_count][len] = '\0';
    result->cmd_count++;
    start = sep + 2;
  }
  kvs_free(buf);
  return result;
}
// 5. 核心：执行恢复的所有指令
int execute_recovered_cmds(const char *file) {
  // 步骤1：恢复日志中的指令
  printf("进入execute_recovered_cmds\n");
  KvRecoverResult *recover_result = kvs_recover_data(file);
  if (recover_result == NULL) {
    fprintf(stderr, "指令恢复失败或日志文件为空\n");
    return -1;
  }
  printf("成功恢复 %d 条指令,开始执行...\n", recover_result->cmd_count);

  // 步骤2：遍历每条指令执行
  const int MAX_TOKENS = 10; // 单条指令最多分割为10个token（按需调整)
  printf("recover_result->cmd_count=%d", recover_result->cmd_count);
  for (int i = 0; i < recover_result->cmd_count; i++) {
    char *curr_cmd = recover_result->cmds[i];
    if (curr_cmd == NULL) { // 新增：空指令跳过
      // fprintf(stderr, "指令 %d 为空,跳过\n", i + 1);
      continue;
    }
    // printf("\n---------- 执行指令 %d ----------\n", i + 1);
    // printf("原始指令：%s\n", curr_cmd);

    char *tokens[MAX_TOKENS] = {}; // 空初始化器（符合VLA规则)

    // 手动初始化：tokens所有指针置NULL,response所有字节置0
    memset(tokens, 0, sizeof(tokens)); // 指针数组置NULL（0等价于NULL)

    // 2.2 复制指令（避免strtok修改原指令内容)
    char cmd_copy[strlen(curr_cmd) + 1];
    strcpy(cmd_copy, curr_cmd);

    // 2.3 分割指令为token
    int token_count = kvs_split_token(cmd_copy, tokens);
    if (token_count < 0) {
      fprintf(stderr, "指令 %d 分割失败\n", i + 1);
      // 新增：清理tokens避免泄漏
      for (int j = 0; j < MAX_TOKENS; j++) {
        if (tokens[j] != NULL) {
          kvs_free(tokens[j]);
        }
      }
      continue;
    }
    // printf("分割后token数量：%d\n", token_count);
    // for (int j = 0; j < token_count; j++) {
    //   printf("token[%d]：%s\n", j, tokens[j]);
    // }

    // 2.4 执行指令（调用你的kvs_filter_protocol)
    int ret = set_protocal(tokens);
    if (ret == 0) {
      // printf("指令执行成功\n"); // 修正：response带\r\n,不用加\n
    } else {
      fprintf(stderr, "指令执行失败\n");
    }
    // 2.5 清理当前指令的token内存（避免泄漏)
    for (int j = 0; j < token_count; j++) {
      if (tokens[j] != NULL) {
        kvs_free(tokens[j]);
        tokens[j] = NULL; // 新增：置NULL避免重复kvs_free
      }
    }
  }

  // 步骤3：释放恢复结果的内存
  free_kv_recover_result(recover_result);
  printf("\n所有指令执行完成！\n");
  return 0;
}