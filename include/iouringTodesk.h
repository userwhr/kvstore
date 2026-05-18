#ifndef _IOURING_
#define _IOURING_
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ====================== 配置 ======================
#define QUEUE_DEPTH 128

// ====================== 回调类型定义 ======================
// 小驼峰：asyncIoCb_t
typedef void (*asyncIoCb_t)(int result, void *userData);

// 内部请求上下文（小驼峰：ioReq）
struct ioReq {
  int fd;
  void *buf;
  size_t len;
  asyncIoCb_t cb;
  void *userData; // 小驼峰
};

// ====================== 全局 io_uring 实例 ======================
static struct io_uring gRing; // 小驼峰
static int gInited = 0;       // 小驼峰

// ====================== 对外可调用 API（小驼峰命名） ======================

int uringAsyncWrite(int fd, void *buf, size_t len, off_t offset, asyncIoCb_t cb,
                    void *userData);
int uringInit(void);
void writeCallback(int res, void *data);
void uringProcessCqes(void);
#endif