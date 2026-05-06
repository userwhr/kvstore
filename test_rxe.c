// test_rxe.c
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
  printf("Test 1: Getting device list...\n");
  int num_devices;
  struct ibv_device **dev_list = ibv_get_device_list(&num_devices);

  if (!dev_list || num_devices == 0) {
    printf("FAIL: No RDMA devices found\n");
    return 1;
  }
  printf("OK: Found %d devices\n", num_devices);

  printf("Test 2: Opening device...\n");
  struct ibv_context *ctx = ibv_open_device(dev_list[0]);
  if (!ctx) {
    printf("FAIL: ibv_open_device failed\n");
    ibv_free_device_list(dev_list);
    return 1;
  }
  printf("OK: Device opened: %s\n", ctx->device->name);

  printf("Test 3: Allocating Protection Domain...\n");
  struct ibv_pd *pd = ibv_alloc_pd(ctx);
  if (!pd) {
    printf("FAIL: ibv_alloc_pd failed: %s\n", strerror(errno));
    // 检查 errno
    printf("errno = %d\n", errno);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return 1;
  }
  printf("OK: PD allocated: %p\n", (void *)pd);

  printf("All tests passed!\n");

  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(dev_list);
  return 0;
}