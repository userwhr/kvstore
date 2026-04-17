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
typedef void (*asyncIoCb_t)(int result, void *userData);

// 内部请求上下文
struct ioReq {
  int fd;
  void *buf;
  size_t len;
  asyncIoCb_t cb;
  void *userData;
};

// ====================== 全局 io_uring 实例 ======================
static struct io_uring gRing;
static int gInited = 0;

// ====================== 核心 API ======================
int uringInit(void) {
  if (gInited)
    return 0;
  int ret = io_uring_queue_init(QUEUE_DEPTH, &gRing, 0);
  if (ret < 0) {
    fprintf(stderr, "uring 初始化失败: %s\n", strerror(-ret));
    return ret;
  }
  gInited = 1;
  return 0;
}

// 异步写入文件
int uringAsyncWrite(int fd, void *buf, size_t len, off_t offset, asyncIoCb_t cb,
                    void *userData) {
  if (!gInited)
    return -ENOLINK;

  struct ioReq *req = malloc(sizeof(*req));
  if (!req)
    return -ENOMEM;

  req->fd = fd;
  req->buf = buf;
  req->len = len;
  req->cb = cb;
  req->userData = userData;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&gRing);
  if (!sqe) {
    free(req);
    return -EBUSY;
  }

  io_uring_prep_write(sqe, fd, buf, len, offset);
  io_uring_sqe_set_data(sqe, req);

  int ret = io_uring_submit(&gRing);
  return ret < 0 ? ret : 0;
}

// 处理完成事件（执行回调）
void uringProcessCqes(void) {
  if (!gInited)
    return;

  while (1) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_peek_cqe(&gRing, &cqe);
    if (ret == -EAGAIN)
      break;
    if (ret < 0)
      return;

    struct ioReq *req = (struct ioReq *)io_uring_cqe_get_data(cqe);
    if (req->cb)
      req->cb(cqe->res, req->userData);

    free(req);
    io_uring_cqe_seen(&gRing, cqe);
  }
}

// 释放资源
void uringCleanup(void) {
  if (gInited) {
    io_uring_queue_exit(&gRing);
    gInited = 0;
  }
}

// ====================== 测试回调函数 ======================
void writeCallback(int res, void *data) {
  const char *fileName = (const char *)data;
  if (res >= 0) {
    printf("[✅ 写入成功] 文件：%s，写入字节数：%d\n", fileName, res);
  } else {
    printf("[❌ 写入失败] 文件：%s，错误：%s\n", fileName, strerror(-res));
  }
}

// ====================== 主函数：测试写入字符串 ======================
int main() {
  // 1. 初始化 io_uring
  if (uringInit() != 0) {
    return -1;
  }
  printf("=== io_uring 初始化成功 ===\n");

  // 2. 打开文件（创建+清空+写入权限）
  const char *testFile = "iouring_test.txt";
  int fd = open(testFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    perror("打开文件失败");
    uringCleanup();
    return -1;
  }

  // 3. 要写入的测试字符串
  const char *testStr = "Hello, io_uring! 这是异步写入的测试字符串～\n";
  size_t strLen = strlen(testStr);
  printf("待写入字符串：%s", testStr);

  // 4. 提交异步写入请求
  uringAsyncWrite(fd, (void *)testStr, strLen, 0, writeCallback,
                  (void *)testFile);

  // 5. 处理 IO 完成事件（执行回调）
  uringProcessCqes();

  // 6. 关闭文件 + 释放资源
  close(fd);
  uringCleanup();

  printf("\n=== 程序执行完成，文件已写入！ ===\n");
  return 0;
}