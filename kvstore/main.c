#include "kvstore.h"
#include <fcntl.h>
#include <signal.h>
file_content_t *kvs_log = NULL;
file_content_t *Icr_log = NULL;
int main(int argc, char *argv[]) {
  kvs_init();
  uringInit();
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
#if ENABLE_INCREMENTAL_PERSIST
  Icr_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
  fcInit(LOG_PATH_ICR, Icr_log);
  map_file_to_struct(Icr_log);
  replay_commands_from_mmap(Icr_log);
#endif

#define ENABLE_REATOR 1 // 修正：启用reactor（原代码注释了）
#if ENABLE_REATOR
  printf("reactor star\n");
  react_entrence(PORT, kvs_filter_protocol);
#else // 修正：条件编译逻辑，避免冲突
  nytco_start(port, kvs_protocol);
#endif

  return 0;
}