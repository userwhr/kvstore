#include "kvstore.h"
#include <fcntl.h>
#include <signal.h>
file_content_t *kvs_log = NULL;
int filefd = -1;
/**
 * @brief Ctrl+C信号处理函数（优雅退出）
 */
void handle_ctrl_c(int sig) {
  sync_mmap_to_file(kvs_log);
  // free_file_content(kvs_log);

  exit(0);
}

/**
 * @brief 注册Ctrl+C信号捕获
 */
void register_ctrl_c_handler() {
  // 注册SIGINT（Ctrl+C）信号的处理函数
  signal(SIGINT, handle_ctrl_c);
}
int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("参数错误：%s <port>\n", argv[0]);
    return -1;
  }

  int port = atoi(argv[1]);
  if (port <= 0 || port > 65535) {
    printf("端口非法\n");
    return -1;
  }
  printf("进入kvs_init\n");
  kvs_init();

#if ENABLE_FULL_PERSIST
  // 🔥 【正确顺序】1. 先初始化 mmap（空文件预分配，但 used_len=0）
  kvs_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
  printf("map_file_to_struct\n");
  map_file_to_struct(LOG_PATH, kvs_log);

  // 🔥 【正确顺序】2. 再执行恢复（只读 used_len 有效数据，无 null）
  int ret = execute_recovered_cmds(LOG_PATH);
  if (ret == -1) {
    printf("恢复失败\n");
  }
#endif

#define ENABLE_REATOR 1 // 修正：启用reactor（原代码注释了）
#if ENABLE_REATOR
  printf("reactor star\n");
  react_entrence(port, kvs_protocol);
#else // 修正：条件编译逻辑，避免冲突
  nytco_start(port, kvs_protocol);
#endif

  return 0;
}