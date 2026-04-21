#include "config.h"
#include "kvstore.h"
#include <fcntl.h>
#include <signal.h>
file_content_t *kvs_log = NULL;
file_content_t *Icr_log = NULL;
int main(int argc, char *argv[]) {
  if (load_config("./kvstore.conf") != 0) {
    exit(EXIT_FAILURE);
  }
  kvs_init();
  uringInit();
  if (g_config.enable_full_persist == 1) {
    kvs_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
    printf("map_file_to_struct\n");
    fcInit(g_config.log_path, kvs_log);
    map_file_to_struct(kvs_log);
    printf("snapshot_restore_from_mmap start\n");
    int ret = snapshot_restore_from_mmap(kvs_log);
  }
  if (g_config.enable_incremental_persist == 1) {
    Icr_log = (file_content_t *)kvs_malloc(sizeof(file_content_t));
    fcInit(g_config.log_path_icr, Icr_log);
    map_file_to_struct(Icr_log);
    AOF_commands_from_mmap(Icr_log);
  }

#define ENABLE_REATOR 1 // 修正：启用reactor（原代码注释了）
#if ENABLE_REATOR
  printf("reactor star\n");
  react_entrence(g_config.port, kvs_filter_protocol);
#else // 修正：条件编译逻辑，避免冲突
  nytco_start(port, kvs_protocol);
#endif

  return 0;
}