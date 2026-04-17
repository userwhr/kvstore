#include "iouringTodesk.h"
#include "mempool.h"

/**
 * @brief 初始化 io_uring（全局调用一次）
 */
int uringInit(void) {
  if (gInited)
    return 0;
  int ret = io_uring_queue_init(QUEUE_DEPTH, &gRing, 0);
  if (ret < 0) {
    fprintf(stderr, "uring init failed: %s\n", strerror(-ret));
    return ret;
  }
  gInited = 1;
  return 0;
}

/**
 * @brief 【外部核心调用】异步写文件（fd 作为第一个参数）
 * @param fd: 目标文件描述符（必传）
 * @param buf: 数据缓冲区
 * @param len: 数据长度
 * @param offset: 文件偏移量
 * @param cb: 完成回调
 * @param userData: 回调自定义数据
 */
int uringAsyncWrite(int fd, void *buf, size_t len, off_t offset, asyncIoCb_t cb,
                    void *userData) {
  if (!gInited)
    return -ENOLINK;

  // 分配请求对象
  struct ioReq *req = kvs_malloc(sizeof(*req));
  if (!req)
    return -ENOMEM;

  req->fd = fd;
  req->buf = buf;
  req->len = len;
  req->cb = cb;
  req->userData = userData;

  // 获取 SQE
  struct io_uring_sqe *sqe = io_uring_get_sqe(&gRing);
  if (!sqe) {
    kvs_free(req);
    return -EBUSY;
  }

  // 准备异步写请求
  io_uring_prep_write(sqe, fd, buf, len, offset);
  io_uring_sqe_set_data(sqe, req);
  sqe->flags |= IOSQE_ASYNC;

  // 提交
  int ret = io_uring_submit(&gRing);
  return ret < 0 ? ret : 0;
}

/**
 * @brief 异步落盘（fsync）同样带 fd 参数
 */
int uringAsyncFsync(int fd, asyncIoCb_t cb, void *userData) {
  if (!gInited)
    return -ENOLINK;

  struct ioReq *req = kvs_malloc(sizeof(*req));
  if (!req)
    return -ENOMEM;
  req->fd = fd;
  req->cb = cb;
  req->userData = userData;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&gRing);
  if (!sqe) {
    kvs_free(req);
    return -EBUSY;
  }

  io_uring_prep_fsync(sqe, fd, 0);
  io_uring_sqe_set_data(sqe, req);

  int ret = io_uring_submit(&gRing);
  return ret < 0 ? ret : 0;
}

/**
 * @brief 处理所有完成事件（循环处理）
 */
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
    if (req->cb) {
      req->cb(cqe->res, req->userData);
    }

    kvs_free(req);
    io_uring_cqe_seen(&gRing, cqe);
  }
}

/**
 * @brief 释放资源
 */
void uringCleanup(void) {
  if (gInited) {
    io_uring_queue_exit(&gRing);
    gInited = 0;
  }
}

// ====================== 回调函数示例（小驼峰） ======================
// 修复后的回调函数
void writeCallback(int res, void *data) {
  // data 就是我们分配的堆内存 async_buf
  char *asyncBuf = (char *)data;

  if (res < 0) {
    printf("[❌ 异步写入失败] 错误: %s\n", strerror(-res));
  }
  // ✅ 关键：释放异步IO的堆内存（必须做！）
  kvs_free(asyncBuf);
}