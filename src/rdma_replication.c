/*
 * ============================================================================
 * RDMA 主从同步实现（参照 testrdma 架构重写）
 * ============================================================================
 *
 * 【架构说明】
 * 参照 testrdma/test_server.c 和 testrdma/test_client.c 的架构，
 * 关键区别在于 QP 必须在 rdma_accept/rdma_connect 之前创建。
 *
 * Master 端流程（类似 test_server.c）:
 *   1. 创建 event_channel + listen_id
 *   2. rdma_bind_addr + rdma_listen
 *   3. 等待 CONNECT_REQUEST 事件
 *   4. 分配 PD、CQ，创建 QP（关键：在 accept 之前）
 *   5. 注册 MR
 *   6. rdma_accept（带 conn_param）
 *   7. 等待 ESTABLISHED 事件
 *   8. POST RECV -> 等待客户端数据 -> SEND 响应
 *
 * Slave 端流程（类似 test_client.c）:
 *   1. 创建 event_channel + cm_id
 *   2. rdma_resolve_addr -> 等待 ADDR_RESOLVED
 *   3. rdma_resolve_route -> 等待 ROUTE_RESOLVED
 *   4. 分配 PD、CQ，创建 QP（关键：在 connect 之前）
 *   5. 注册 MR
 *   6. rdma_connect（带 conn_param）
 *   7. 等待 ESTABLISHED 事件
 *   8. SEND 数据 -> POST RECV 接收响应
 *
 * 同步锁机制:
 *   - g_syncing = 1: Master 阻止所有写操作
 *   - g_syncing = 0: 正常模式
 * ============================================================================
 */

#include "rdma_replication.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ====================== 常量定义 ======================

/* RDMA 传输缓冲区大小：1MB */
#define RDMA_BUFFER_SIZE (1024 * 1024)

/* CQ 容量 */
#define CQ_SIZE 16

// ====================== 全局变量 ======================

/*
 * 全局同步标志
 * 0: 正常模式，允许写操作
 * 1: SYNC 进行中，阻止所有写操作
 */
volatile int g_syncing = 0;

// ====================== 内部数据结构 ======================

/*
 * RDMA 上下文结构体
 * 封装了一次 RDMA 连接所需的所有资源
 */
typedef struct rdma_context {
  struct ibv_pd *pd;
  struct ibv_comp_channel *comp_channel;
  struct ibv_cq *cq;
  struct ibv_mr *mr;
  void *buf;
  size_t buf_size;
} rdma_context_t;

// ====================== 连接参数构建 ======================

/*
 * 构建 RDMA 连接参数（rxe 优化）
 * 参照 testrdma/common.h 中的 build_conn_param
 */
static void build_conn_param(struct rdma_conn_param *param) {
  memset(param, 0, sizeof(*param));
  param->responder_resources = 1;
  param->initiator_depth = 1;
  param->retry_count = 7;
  param->rnr_retry_count = 7;
  param->flow_control = 1;
}

// ====================== 内部工具函数 ======================

/*
 * 初始化 RDMA 上下文（只分配缓冲区）
 */
static int rdma_context_init(rdma_context_t *ctx, size_t buf_size) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->buf_size = buf_size;
  ctx->buf = malloc(buf_size);
  if (!ctx->buf) {
    perror("malloc RDMA buffer failed");
    return -1;
  }
  memset(ctx->buf, 0, buf_size);
  return 0;
}

/*
 * 销毁 RDMA 上下文，释放所有资源
 */
static void rdma_context_destroy(rdma_context_t *ctx) {
  if (ctx->mr)
    ibv_dereg_mr(ctx->mr);
  if (ctx->cq)
    ibv_destroy_cq(ctx->cq);
  if (ctx->comp_channel)
    ibv_destroy_comp_channel(ctx->comp_channel);
  if (ctx->pd)
    ibv_dealloc_pd(ctx->pd);
  if (ctx->buf)
    free(ctx->buf);
  memset(ctx, 0, sizeof(rdma_context_t));
}

/*
 * 分配 RDMA 资源（PD, CQ, Comp Channel, MR）
 * 在 QP 创建之前调用
 */
static int rdma_alloc_resources(rdma_context_t *ctx,
                                struct ibv_context *verbs) {
  /* 分配保护域 */
  ctx->pd = ibv_alloc_pd(verbs);
  if (!ctx->pd) {
    perror("ibv_alloc_pd failed");
    return -1;
  }

  /* 创建完成通道 */
  ctx->comp_channel = ibv_create_comp_channel(verbs);
  if (!ctx->comp_channel) {
    perror("ibv_create_comp_channel failed");
    return -1;
  }

  /* 创建完成队列 */
  ctx->cq = ibv_create_cq(verbs, CQ_SIZE, NULL, ctx->comp_channel, 0);
  if (!ctx->cq) {
    perror("ibv_create_cq failed");
    return -1;
  }

  /* 请求 CQ 通知 */
  if (ibv_req_notify_cq(ctx->cq, 0)) {
    perror("ibv_req_notify_cq failed");
    return -1;
  }

  /* 注册内存区域 */
  ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->buf_size,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE);
  if (!ctx->mr) {
    perror("ibv_reg_mr failed");
    return -1;
  }

  return 0;
}

/*
 * 等待指定数量的 RDMA 完成事件
 * 参照 testrdma 使用轮询方式（更简单可靠）
 */
static int rdma_wait_completions(rdma_context_t *ctx, int num_completions) {
  struct ibv_wc wc;
  int completed = 0;

  while (completed < num_completions) {
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int ret;

    /* 阻塞等待 CQ 事件 */
    ret = ibv_get_cq_event(ctx->comp_channel, &ev_cq, &ev_ctx);
    if (ret) {
      fprintf(stderr, "ibv_get_cq_event failed: %s\n", strerror(ret));
      return -1;
    }

    /* 确认事件 */
    ibv_ack_cq_events(ev_cq, 1);

    /* 请求下一次通知 */
    ret = ibv_req_notify_cq(ctx->cq, 0);
    if (ret) {
      fprintf(stderr, "ibv_req_notify_cq failed: %s\n", strerror(ret));
      return -1;
    }

    /* 轮询 CQ，取出所有已完成的 WC */
    while (1) {
      ret = ibv_poll_cq(ctx->cq, 1, &wc);
      if (ret < 0) {
        fprintf(stderr, "ibv_poll_cq failed\n");
        return -1;
      }
      if (ret == 0)
        break;

      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion error: %s (status=%d)\n",
                ibv_wc_status_str(wc.status), wc.status);
        return -1;
      }
      completed++;
    }
  }
  return 0;
}

/*
 * 等待一个 RDMA CM 事件并检查类型
 */
static int rdma_wait_cm_event(struct rdma_event_channel *channel,
                              struct rdma_cm_event **event,
                              enum rdma_cm_event_type expected) {
  int ret = rdma_get_cm_event(channel, event);
  if (ret) {
    fprintf(stderr, "rdma_get_cm_event failed: %s\n", strerror(errno));
    return -1;
  }
  if ((*event)->event != expected) {
    fprintf(stderr, "Unexpected CM event: %d (expected %d), status=%d\n",
            (*event)->event, expected, (*event)->status);
    rdma_ack_cm_event(*event);
    *event = NULL;
    return -1;
  }
  return 0;
}

/*
 * 手动关联 verbs 设备（SoftRoCE 兼容）
 * 优先选择 rxe0 设备
 */
static struct ibv_context *rdma_attach_device(void) {
  int num_devices = 0;
  struct ibv_device **dev_list = ibv_get_device_list(&num_devices);

  if (!dev_list || num_devices == 0) {
    fprintf(stderr, "ERROR: No RDMA devices found!\n");
    return NULL;
  }

  // printf("[RDMA] Found %d device(s):\n", num_devices);
  struct ibv_device *target = NULL;
  for (int i = 0; i < num_devices; i++) {
    const char *name = ibv_get_device_name(dev_list[i]);
    // printf("[RDMA]   %d: %s\n", i, name);
    if (strcmp(name, "rxe0") == 0) {
      target = dev_list[i];
    }
  }

  if (!target) {
    target = dev_list[0];
    // printf("[RDMA] rxe0 not found, using %s\n", ibv_get_device_name(target));
  }

  struct ibv_context *verbs = ibv_open_device(target);
  if (!verbs) {
    fprintf(stderr, "ERROR: Failed to open device %s\n",
            ibv_get_device_name(target));
  }

  ibv_free_device_list(dev_list);
  return verbs;
}

// ====================== Master 端实现 ======================

/*
 * Master 端：通过 RDMA 发送文件内容给 Slave
 *
 * 参照 testrdma/test_server.c 的架构：
 *   1. 打开文件，获取大小
 *   2. 创建 event_channel + listen_id
 *   3. rdma_bind_addr + rdma_listen
 *   4. 等待 CONNECT_REQUEST
 *   5. 分配 PD/CQ/CompChannel/MR（在 accept 之前）
 *   6. 创建 QP（关键：在 accept 之前）
 *   7. rdma_accept（带 conn_param）
 *   8. 等待 ESTABLISHED
 *   9. POST RECV 等待 Slave 发送数据
 *   10. 收到数据后，SEND 文件内容给 Slave
 */
int rdma_master_send_file(const char *file_path, int rdma_port) {
  struct rdma_event_channel *cm_channel = NULL;
  struct rdma_cm_id *listen_id = NULL;
  struct rdma_cm_id *child_id = NULL;
  struct rdma_cm_event *event = NULL;
  rdma_context_t ctx;
  int ret = -1;
  FILE *fp = NULL;
  size_t file_size = 0;

  memset(&ctx, 0, sizeof(rdma_context_t));

  /* ========== 第1步：打开文件，获取大小 ========== */
  fp = fopen(file_path, "rb");
  if (!fp) {
    perror("fopen file for RDMA send failed");
    return -1;
  }
  fseek(fp, 0, SEEK_END);
  file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  // printf("[RDMA Master] File size: %zu bytes\n", file_size);

  /* ========== 第2步：创建 event channel 和 CM ID ========== */
  cm_channel = rdma_create_event_channel();
  if (!cm_channel) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto cleanup;
  }

  ret = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed: %s\n", strerror(errno));
    goto cleanup;
  }

  /* ========== 第3步：绑定地址 + 监听 ========== */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(rdma_port);
  addr.sin_addr.s_addr = INADDR_ANY;

  ret = rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
  if (ret) {
    fprintf(stderr, "rdma_bind_addr failed: %s\n", strerror(errno));
    goto cleanup;
  }

  /* SoftRoCE 修复：手动绑定 verbs 设备 */
  if (!listen_id->verbs) {
    // printf("[RDMA Master] Manually attaching RDMA device...\n");
    listen_id->verbs = rdma_attach_device();
    if (!listen_id->verbs) {
      goto cleanup;
    }
    // printf("[RDMA Master] Attached to device: %s\n",
    //        listen_id->verbs->device->name);
  }

  ret = rdma_listen(listen_id, 1);
  if (ret) {
    fprintf(stderr, "rdma_listen failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Master] Listening on port %d...\n", rdma_port);
  fflush(stdout);

  /* ========== 第4步：等待 Slave 连接请求 ========== */
  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_CONNECT_REQUEST);
  if (ret)
    goto cleanup;

  child_id = event->id;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Master] Connection request received\n");
  fflush(stdout);

  /* 确保 child_id 有 verbs */
  if (!child_id->verbs) {
    if (listen_id->verbs) {
      child_id->verbs = listen_id->verbs;
    } else {
      child_id->verbs = rdma_attach_device();
    }
  }
  if (!child_id->verbs) {
    fprintf(stderr, "FATAL: Cannot get verbs for child_id\n");
    goto cleanup;
  }
  // printf("[RDMA Master] Child verbs device: %s\n",
  //        child_id->verbs->device->name);

  /* ========== 第5步：初始化缓冲区 + 分配 RDMA 资源 ========== */
  ret = rdma_context_init(&ctx, RDMA_BUFFER_SIZE);
  if (ret)
    goto cleanup;

  ret = rdma_alloc_resources(&ctx, child_id->verbs);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] RDMA resources allocated\n");
  fflush(stdout);

  /* ========== 第6步：创建 QP（关键：在 accept 之前） ========== */
  struct ibv_qp_init_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.send_cq = ctx.cq;
  qp_attr.recv_cq = ctx.cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = CQ_SIZE;
  qp_attr.cap.max_recv_wr = CQ_SIZE;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ret = rdma_create_qp(child_id, ctx.pd, &qp_attr);
  if (ret) {
    fprintf(stderr, "rdma_create_qp failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Master] QP created (QP num=%d)\n", child_id->qp->qp_num);
  fflush(stdout);

  /* ========== 第7步：接受连接（带 conn_param） ========== */
  struct rdma_conn_param conn_param;
  build_conn_param(&conn_param);

  // printf("[RDMA Master] Accepting connection...\n");
  fflush(stdout);

  ret = rdma_accept(child_id, &conn_param);
  if (ret) {
    fprintf(stderr, "rdma_accept failed: %s (errno=%d)\n", strerror(errno),
            errno);
    goto cleanup;
  }

  /* ========== 第8步：等待连接建立 ========== */
  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_ESTABLISHED);
  if (ret)
    goto cleanup;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Master] Connection established!\n");
  fflush(stdout);

  /* ========== 第9步：先 POST RECV，等待 Slave 发送请求 ========== */
  struct ibv_sge recv_sge;
  struct ibv_recv_wr recv_wr, *bad_recv_wr;

  memset(&recv_sge, 0, sizeof(recv_sge));
  recv_sge.addr = (uintptr_t)ctx.buf;
  recv_sge.length = ctx.buf_size;
  recv_sge.lkey = ctx.mr->lkey;

  memset(&recv_wr, 0, sizeof(recv_wr));
  recv_wr.wr_id = 0;
  recv_wr.sg_list = &recv_sge;
  recv_wr.num_sge = 1;

  ret = ibv_post_recv(child_id->qp, &recv_wr, &bad_recv_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_recv failed: %s\n", strerror(ret));
    goto cleanup;
  }
  // printf("[RDMA Master] Waiting for Slave request...\n");
  fflush(stdout);

  /* 等待接收完成 */
  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] Received request from Slave: \"%s\"\n",
  //        (char *)ctx.buf);

  /* ========== 第10步：发送文件内容给 Slave ========== */
  /* 先发送文件大小（8 字节） */
  struct ibv_sge sge;
  struct ibv_send_wr send_wr, *bad_wr;

  memcpy(ctx.buf, &file_size, sizeof(file_size));

  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)ctx.buf;
  sge.length = sizeof(file_size);
  sge.lkey = ctx.mr->lkey;

  memset(&send_wr, 0, sizeof(send_wr));
  send_wr.wr_id = 1;
  send_wr.sg_list = &sge;
  send_wr.num_sge = 1;
  send_wr.opcode = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;

  ret = ibv_post_send(child_id->qp, &send_wr, &bad_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_send (file size) failed: %s\n", strerror(ret));
    goto cleanup;
  }

  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] File size sent: %zu bytes\n", file_size);

  /* 分块发送文件内容 */
  size_t total_sent = 0;
  int chunk_id = 2;
  while (total_sent < file_size) {
    size_t chunk_size = file_size - total_sent;
    if (chunk_size > RDMA_BUFFER_SIZE)
      chunk_size = RDMA_BUFFER_SIZE;

    size_t read_ret = fread(ctx.buf, 1, chunk_size, fp);
    if (read_ret != chunk_size && ferror(fp)) {
      fprintf(stderr, "fread failed\n");
      goto cleanup;
    }

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)ctx.buf;
    sge.length = chunk_size;
    sge.lkey = ctx.mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = chunk_id++;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(child_id->qp, &send_wr, &bad_wr);
    if (ret) {
      fprintf(stderr, "ibv_post_send (chunk) failed: %s\n", strerror(ret));
      goto cleanup;
    }

    ret = rdma_wait_completions(&ctx, 1);
    if (ret)
      goto cleanup;

    total_sent += chunk_size;
    // printf("[RDMA Master] Sent chunk: %zu/%zu bytes (%.1f%%)\r", total_sent,
    //        file_size, (double)total_sent / file_size * 100);
    fflush(stdout);
  }
  // printf("\n[RDMA Master] File transfer complete\n");

  ret = 0;

  /* ========== 清理资源 ========== */
cleanup:
  if (fp)
    fclose(fp);
  if (event)
    rdma_ack_cm_event(event);
  if (child_id) {
    rdma_disconnect(child_id);
  }
  rdma_context_destroy(&ctx);
  if (child_id) {
    rdma_destroy_id(child_id);
  }
  if (listen_id)
    rdma_destroy_id(listen_id);
  if (cm_channel)
    rdma_destroy_event_channel(cm_channel);
  return ret;
}

// ====================== Slave 端实现 ======================

/*
 * Slave 端：通过 RDMA 从 Master 接收文件数据到内存缓冲区
 *
 * 参照 testrdma/test_client.c 的架构：
 *   1. 创建 event_channel + cm_id
 *   2. rdma_resolve_addr -> 等待 ADDR_RESOLVED
 *   3. rdma_resolve_route -> 等待 ROUTE_RESOLVED
 *   4. 分配 PD/CQ/CompChannel/MR（在 connect 之前）
 *   5. 创建 QP（关键：在 connect 之前）
 *   6. rdma_connect（带 conn_param）
 *   7. 等待 ESTABLISHED
 *   8. SEND 请求给 Master
 *   9. 接收文件大小 + 分块接收文件内容
 */
int rdma_slave_recv_file(const char *server_ip, int rdma_port, char **out_buf,
                         size_t *out_size) {
  struct rdma_event_channel *cm_channel = NULL;
  struct rdma_cm_id *cm_id = NULL;
  struct rdma_cm_event *event = NULL;
  rdma_context_t ctx;
  int ret = -1;
  size_t file_size = 0;
  size_t total_recv = 0;

  memset(&ctx, 0, sizeof(rdma_context_t));

  /* ========== 第1步：创建 event channel 和 CM ID ========== */
  cm_channel = rdma_create_event_channel();
  if (!cm_channel) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    return -1;
  }

  ret = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Slave] CM ID created\n");

  /* ========== 第2步：解析服务器地址 ========== */
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(rdma_port);
  ret = inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
  if (ret <= 0) {
    fprintf(stderr, "inet_pton failed for %s\n", server_ip);
    goto cleanup;
  }

  ret = rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&server_addr, 2000);
  if (ret) {
    fprintf(stderr, "rdma_resolve_addr failed: %s\n", strerror(errno));
    goto cleanup;
  }

  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_ADDR_RESOLVED);
  if (ret)
    goto cleanup;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Slave] Address resolved to %s:%d\n", server_ip, rdma_port);

  /* ========== 第3步：解析路由 ========== */
  ret = rdma_resolve_route(cm_id, 2000);
  if (ret) {
    fprintf(stderr, "rdma_resolve_route failed: %s\n", strerror(errno));
    goto cleanup;
  }

  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_ROUTE_RESOLVED);
  if (ret)
    goto cleanup;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Slave] Route resolved\n");

  /* SoftRoCE 修复：手动绑定 verbs 设备 */
  if (!cm_id->verbs) {
    // printf("[RDMA Slave] Manually attaching RDMA device...\n");
    cm_id->verbs = rdma_attach_device();
    if (!cm_id->verbs) {
      goto cleanup;
    }
    // printf("[RDMA Slave] Attached to device: %s\n", cm_id->verbs->device->name);
  }

  /* ========== 第4步：初始化缓冲区 + 分配 RDMA 资源 ========== */
  ret = rdma_context_init(&ctx, RDMA_BUFFER_SIZE);
  if (ret)
    goto cleanup;

  ret = rdma_alloc_resources(&ctx, cm_id->verbs);
  if (ret)
    goto cleanup;
  // printf("[RDMA Slave] RDMA resources allocated\n");

  /* ========== 第5步：创建 QP（关键：在 connect 之前） ========== */
  struct ibv_qp_init_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.send_cq = ctx.cq;
  qp_attr.recv_cq = ctx.cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = CQ_SIZE;
  qp_attr.cap.max_recv_wr = CQ_SIZE;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ret = rdma_create_qp(cm_id, ctx.pd, &qp_attr);
  if (ret) {
    fprintf(stderr, "rdma_create_qp failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Slave] QP created (QP num=%d)\n", cm_id->qp->qp_num);

  /* ========== 第6步：连接 Master（带 conn_param） ========== */
  struct rdma_conn_param conn_param;
  build_conn_param(&conn_param);

  ret = rdma_connect(cm_id, &conn_param);
  if (ret) {
    fprintf(stderr, "rdma_connect failed: %s (errno=%d)\n", strerror(errno),
            errno);
    goto cleanup;
  }
  // printf("[RDMA Slave] Connecting...\n");

  /* ========== 第7步：等待连接建立 ========== */
  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_ESTABLISHED);
  if (ret)
    goto cleanup;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Slave] Connection established!\n");

  /* ========== 第8步：发送请求给 Master ========== */
  const char *request = "SYNC_REQUEST";
  memcpy(ctx.buf, request, strlen(request) + 1);

  struct ibv_sge send_sge;
  struct ibv_send_wr send_wr, *bad_wr;

  memset(&send_sge, 0, sizeof(send_sge));
  send_sge.addr = (uintptr_t)ctx.buf;
  send_sge.length = strlen(request) + 1;
  send_sge.lkey = ctx.mr->lkey;

  memset(&send_wr, 0, sizeof(send_wr));
  send_wr.wr_id = 0;
  send_wr.sg_list = &send_sge;
  send_wr.num_sge = 1;
  send_wr.opcode = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;

  ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_send (request) failed: %s\n", strerror(ret));
    goto cleanup;
  }

  /* 等待发送完成 */
  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;
  // printf("[RDMA Slave] Request sent to Master\n");

  /* ========== 第9步：接收文件大小（8 字节） ========== */
  struct ibv_sge recv_sge;
  struct ibv_recv_wr recv_wr, *bad_recv_wr;

  memset(&recv_sge, 0, sizeof(recv_sge));
  recv_sge.addr = (uintptr_t)ctx.buf;
  recv_sge.length = ctx.buf_size;
  recv_sge.lkey = ctx.mr->lkey;

  memset(&recv_wr, 0, sizeof(recv_wr));
  recv_wr.wr_id = 1;
  recv_wr.sg_list = &recv_sge;
  recv_wr.num_sge = 1;

  ret = ibv_post_recv(cm_id->qp, &recv_wr, &bad_recv_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_recv (file size) failed: %s\n", strerror(ret));
    goto cleanup;
  }

  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;

  memcpy(&file_size, ctx.buf, sizeof(file_size));

  if (file_size == 0) {
    fprintf(stderr, "Warning: Received file size is 0\n");
    ret = 0;
    goto cleanup;
  }

  // printf("[RDMA Slave] File size to receive: %zu bytes (%.2f MB)\n", file_size,
  //        (double)file_size / (1024 * 1024));

  /* ========== 第10步：分配最终数据缓冲区 ========== */
  char *data_buf = (char *)malloc(file_size);
  if (!data_buf) {
    fprintf(stderr, "malloc failed for data buffer (%zu bytes)\n", file_size);
    goto cleanup;
  }

  /* ========== 第11步：循环接收文件内容 ========== */
  total_recv = 0;
  int chunk_id = 2;
  while (total_recv < file_size) {
    size_t remaining = file_size - total_recv;
    size_t chunk_size =
        remaining > RDMA_BUFFER_SIZE ? RDMA_BUFFER_SIZE : remaining;

    memset(&recv_sge, 0, sizeof(recv_sge));
    recv_sge.addr = (uintptr_t)ctx.buf;
    recv_sge.length = ctx.buf_size;
    recv_sge.lkey = ctx.mr->lkey;

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = chunk_id++;
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;

    ret = ibv_post_recv(cm_id->qp, &recv_wr, &bad_recv_wr);
    if (ret) {
      fprintf(stderr, "ibv_post_recv (chunk) failed: %s\n", strerror(ret));
      goto cleanup;
    }

    ret = rdma_wait_completions(&ctx, 1);
    if (ret)
      goto cleanup;

    /* 将接收到的数据拷贝到最终缓冲区 */
    memcpy(data_buf + total_recv, ctx.buf, chunk_size);

    total_recv += chunk_size;

    if (chunk_id % 10 == 0 || total_recv >= file_size) {
      // printf("[RDMA Slave] Progress: %zu/%zu bytes (%.1f%%)\r", total_recv,
      //        file_size, (double)total_recv / file_size * 100);
      fflush(stdout);
    }
  }
  // printf("\n[RDMA Slave] Data receive complete (%zu bytes)\n", file_size);

  /* 设置输出参数 */
  *out_buf = data_buf;
  *out_size = file_size;
  data_buf = NULL; /* 防止被 cleanup 释放 */
  ret = 0;

  /* ========== 清理资源 ========== */
cleanup:
  if (data_buf)
    free(data_buf);
  if (event) {
    rdma_ack_cm_event(event);
    event = NULL;
  }
  if (cm_id && cm_id->qp) {
    rdma_disconnect(cm_id);
  }
  rdma_context_destroy(&ctx);
  if (cm_id) {
    rdma_destroy_id(cm_id);
  }
  if (cm_channel) {
    rdma_destroy_event_channel(cm_channel);
  }
  return ret;
}

// ====================== Master 端：直接发送内存数据 ======================

/*
 * Master 端：通过 RDMA 直接发送内存缓冲区数据给 Slave
 *
 * 与 rdma_master_send_file 的区别：
 *   - 不打开文件，直接从内存缓冲区 data 中读取数据发送
 *   - data_size 指定数据大小
 *   - 适用于 kvs_log 已经通过 mmap 加载到内存的场景
 *
 * 流程（同 testrdma/test_server.c 架构）:
 *   1. 创建 event_channel + listen_id
 *   2. rdma_bind_addr + rdma_listen
 *   3. 等待 CONNECT_REQUEST
 *   4. 分配 PD/CQ/CompChannel/MR
 *   5. 创建 QP（关键：在 accept 之前）
 *   6. rdma_accept（带 conn_param）
 *   7. 等待 ESTABLISHED
 *   8. POST RECV 等待 Slave 请求
 *   9. 收到请求后，SEND 内存数据给 Slave
 */
int rdma_master_send_mem(const void *data, size_t data_size, int rdma_port) {
  struct rdma_event_channel *cm_channel = NULL;
  struct rdma_cm_id *listen_id = NULL;
  struct rdma_cm_id *child_id = NULL;
  struct rdma_cm_event *event = NULL;
  rdma_context_t ctx;
  int ret = -1;

  memset(&ctx, 0, sizeof(rdma_context_t));

  if (!data || data_size == 0) {
    fprintf(stderr, "[RDMA Master] Invalid data pointer or size\n");
    return -1;
  }

  // printf("[RDMA Master] Memory data size: %zu bytes\n", data_size);

  /* ========== 第1步：创建 event channel 和 CM ID ========== */
  cm_channel = rdma_create_event_channel();
  if (!cm_channel) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    return -1;
  }

  ret = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed: %s\n", strerror(errno));
    goto cleanup;
  }

  /* ========== 第2步：绑定地址 + 监听 ========== */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(rdma_port);
  addr.sin_addr.s_addr = INADDR_ANY;

  ret = rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
  if (ret) {
    fprintf(stderr, "rdma_bind_addr failed: %s\n", strerror(errno));
    goto cleanup;
  }

  /* SoftRoCE 修复：手动绑定 verbs 设备 */
  if (!listen_id->verbs) {
    // printf("[RDMA Master] Manually attaching RDMA device...\n");
    listen_id->verbs = rdma_attach_device();
    if (!listen_id->verbs) {
      goto cleanup;
    }
    // printf("[RDMA Master] Attached to device: %s\n",
    //        listen_id->verbs->device->name);
  }

  ret = rdma_listen(listen_id, 1);
  if (ret) {
    fprintf(stderr, "rdma_listen failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Master] Listening on port %d...\n", rdma_port);
  fflush(stdout);

  /* ========== 第3步：等待 Slave 连接请求 ========== */
  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_CONNECT_REQUEST);
  if (ret)
    goto cleanup;

  child_id = event->id;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Master] Connection request received\n");
  fflush(stdout);

  /* 确保 child_id 有 verbs */
  if (!child_id->verbs) {
    if (listen_id->verbs) {
      child_id->verbs = listen_id->verbs;
    } else {
      child_id->verbs = rdma_attach_device();
    }
  }
  if (!child_id->verbs) {
    fprintf(stderr, "FATAL: Cannot get verbs for child_id\n");
    goto cleanup;
  }

  /* ========== 第4步：初始化缓冲区 + 分配 RDMA 资源 ========== */
  ret = rdma_context_init(&ctx, RDMA_BUFFER_SIZE);
  if (ret)
    goto cleanup;

  ret = rdma_alloc_resources(&ctx, child_id->verbs);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] RDMA resources allocated\n");
  fflush(stdout);

  /* ========== 第5步：创建 QP（关键：在 accept 之前） ========== */
  struct ibv_qp_init_attr qp_attr;
  memset(&qp_attr, 0, sizeof(qp_attr));
  qp_attr.send_cq = ctx.cq;
  qp_attr.recv_cq = ctx.cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = CQ_SIZE;
  qp_attr.cap.max_recv_wr = CQ_SIZE;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ret = rdma_create_qp(child_id, ctx.pd, &qp_attr);
  if (ret) {
    fprintf(stderr, "rdma_create_qp failed: %s\n", strerror(errno));
    goto cleanup;
  }
  // printf("[RDMA Master] QP created (QP num=%d)\n", child_id->qp->qp_num);
  fflush(stdout);

  /* ========== 第6步：接受连接（带 conn_param） ========== */
  struct rdma_conn_param conn_param;
  build_conn_param(&conn_param);

  // printf("[RDMA Master] Accepting connection...\n");
  fflush(stdout);

  ret = rdma_accept(child_id, &conn_param);
  if (ret) {
    fprintf(stderr, "rdma_accept failed: %s (errno=%d)\n", strerror(errno),
            errno);
    goto cleanup;
  }

  /* ========== 第7步：等待连接建立 ========== */
  ret = rdma_wait_cm_event(cm_channel, &event, RDMA_CM_EVENT_ESTABLISHED);
  if (ret)
    goto cleanup;
  rdma_ack_cm_event(event);
  event = NULL;
  // printf("[RDMA Master] Connection established!\n");
  fflush(stdout);

  /* ========== 第8步：先 POST RECV，等待 Slave 发送请求 ========== */
  struct ibv_sge recv_sge;
  struct ibv_recv_wr recv_wr, *bad_recv_wr;

  memset(&recv_sge, 0, sizeof(recv_sge));
  recv_sge.addr = (uintptr_t)ctx.buf;
  recv_sge.length = ctx.buf_size;
  recv_sge.lkey = ctx.mr->lkey;

  memset(&recv_wr, 0, sizeof(recv_wr));
  recv_wr.wr_id = 0;
  recv_wr.sg_list = &recv_sge;
  recv_wr.num_sge = 1;

  ret = ibv_post_recv(child_id->qp, &recv_wr, &bad_recv_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_recv failed: %s\n", strerror(ret));
    goto cleanup;
  }
  // printf("[RDMA Master] Waiting for Slave request...\n");
  fflush(stdout);

  /* 等待接收完成 */
  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] Received request from Slave: \"%s\"\n",
  //        (char *)ctx.buf);

  /* ========== 第9步：发送内存数据给 Slave ========== */
  /* 先发送数据大小（8 字节），与 Slave 端 rdma_slave_recv_file 协议一致 */
  struct ibv_sge sge;
  struct ibv_send_wr send_wr, *bad_wr;

  memcpy(ctx.buf, &data_size, sizeof(data_size));

  memset(&sge, 0, sizeof(sge));
  sge.addr = (uintptr_t)ctx.buf;
  sge.length = sizeof(data_size);
  sge.lkey = ctx.mr->lkey;

  memset(&send_wr, 0, sizeof(send_wr));
  send_wr.wr_id = 1;
  send_wr.sg_list = &sge;
  send_wr.num_sge = 1;
  send_wr.opcode = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;

  ret = ibv_post_send(child_id->qp, &send_wr, &bad_wr);
  if (ret) {
    fprintf(stderr, "ibv_post_send (data size) failed: %s\n", strerror(ret));
    goto cleanup;
  }

  ret = rdma_wait_completions(&ctx, 1);
  if (ret)
    goto cleanup;
  // printf("[RDMA Master] Data size sent: %zu bytes\n", data_size);

  /* 分块发送数据内容（从内存 data 中拷贝） */
  size_t total_sent = 0;
  int chunk_id = 2;
  while (total_sent < data_size) {
    size_t chunk_size = data_size - total_sent;
    if (chunk_size > RDMA_BUFFER_SIZE)
      chunk_size = RDMA_BUFFER_SIZE;

    /* 从内存数据中拷贝到 RDMA 缓冲区 */
    memcpy(ctx.buf, (const char *)data + total_sent, chunk_size);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)ctx.buf;
    sge.length = chunk_size;
    sge.lkey = ctx.mr->lkey;

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = chunk_id++;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    ret = ibv_post_send(child_id->qp, &send_wr, &bad_wr);
    if (ret) {
      fprintf(stderr, "ibv_post_send (chunk) failed: %s\n", strerror(ret));
      goto cleanup;
    }

    ret = rdma_wait_completions(&ctx, 1);
    if (ret)
      goto cleanup;

    total_sent += chunk_size;
    // printf("[RDMA Master] Sent chunk: %zu/%zu bytes (%.1f%%)\r", total_sent,
    //        data_size, (double)total_sent / data_size * 100);
    fflush(stdout);
  }
  // printf("\n[RDMA Master] Memory data transfer complete\n");

  ret = 0;

  /* ========== 清理资源 ========== */
cleanup:
  if (event)
    rdma_ack_cm_event(event);
  if (child_id) {
    rdma_disconnect(child_id);
  }
  rdma_context_destroy(&ctx);
  if (child_id) {
    rdma_destroy_id(child_id);
  }
  if (listen_id)
    rdma_destroy_id(listen_id);
  if (cm_channel)
    rdma_destroy_event_channel(cm_channel);
  return ret;
}

// ====================== Master 端线程封装 ======================

/*
 * 线程参数结构体（文件路径版本）
 */
typedef struct {
  char file_path[256];
  int rdma_port;
} rdma_thread_arg_t;

/*
 * 线程参数结构体（内存数据版本）
 */
typedef struct {
  const void *data;
  size_t data_size;
  int rdma_port;
} rdma_mem_thread_arg_t;

/*
 * Master 端 RDMA 传输线程入口函数（文件路径版本）
 */
void *rdma_master_send_file_thread(void *arg) {
  rdma_thread_arg_t *targ = (rdma_thread_arg_t *)arg;
  // printf("[SYNC] RDMA transfer thread started, file=%s, port=%d\n",
  //        targ->file_path, targ->rdma_port);

  int ret = rdma_master_send_file(targ->file_path, targ->rdma_port);
  if (ret == 0) {
    // printf("[SYNC] RDMA transfer completed successfully\n");
  } else {
    fprintf(stderr, "[SYNC] RDMA transfer failed\n");
  }

  /* 传输完成，释放同步锁，恢复写操作 */
  g_syncing = 0;
  // printf("[SYNC] Sync complete, write operations resumed\n");

  free(arg);
  return NULL;
}

// ====================== Master 端：等待 Slave 连接并同步 ======================

/*
 * Master 端：等待 Slave 通过 TCP 连接并发送 SYNC 命令，
 * 然后通过 RDMA 传输内存数据给 Slave
 *
 * 流程：
 *   1. 如果 listen_fd < 0，自己创建 TCP listen socket 并绑定 tcp_port
 *   2. 在 listen_fd 上 accept 等待 Slave 连接
 *   3. 接收 Slave 的 SYNC 命令，回复 +OK
 *   4. 通过 RDMA 发送内存数据给 Slave
 *   5. 返回 Slave 的 TCP 连接 fd（用于后续 eBPF 增量同步）
 *
 * 注意：如果传入有效的 listen_fd，调用者负责关闭它；
 *       如果 listen_fd < 0，函数内部创建并关闭。
 */
int rdma_master_wait_slave_and_sync(const void *data, size_t data_size,
                                    int listen_fd, int rdma_port, int *slave_fd) {
  int internal_listen_fd = -1;
  int conn_fd = -1;
  int ret = -1;
  int should_close_listen = 0; /* 是否由本函数关闭 listen_fd */

  *slave_fd = -1;

  /* ========== 第1步：获取 listen socket ========== */
  if (listen_fd < 0) {
    /* 没有外部 listen fd，自己创建 */
    // printf("[Master] Creating TCP listen socket on port...\n");

    internal_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (internal_listen_fd < 0) {
      perror("socket failed");
      return -1;
    }

    int opt = 1;
    setsockopt(internal_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0); /* 随机端口 */
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(internal_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      perror("bind failed");
      goto cleanup;
    }

    if (listen(internal_listen_fd, 1) < 0) {
      perror("listen failed");
      goto cleanup;
    }

    /* 获取实际绑定的端口 */
    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    getsockname(internal_listen_fd, (struct sockaddr *)&bound_addr, &addr_len);
    // printf("[Master] Internal listen fd created on port %d\n",
    //        ntohs(bound_addr.sin_port));

    listen_fd = internal_listen_fd;
    should_close_listen = 1;
  } else {
    // printf("[Master] Using external listen fd=%d\n", listen_fd);
    should_close_listen = 0; /* 外部 fd，不由我们关闭 */
  }

  /* ========== 第2步：等待 Slave 连接 ========== */
  // printf("[Master] Waiting for Slave SYNC connection...\n");

  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
  if (conn_fd < 0) {
    perror("accept failed");
    goto cleanup;
  }

  char client_ip[64];
  inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
  // printf("[Master] Slave connected from %s:%d\n", client_ip,
  //        ntohs(client_addr.sin_port));

  /* ========== 第3步：接收 SYNC 命令 ========== */
  char buf[1024] = {0};
  int recv_ret = recv(conn_fd, buf, sizeof(buf) - 1, 0);
  if (recv_ret <= 0) {
    perror("recv SYNC failed");
    goto cleanup;
  }
  buf[recv_ret] = '\0';
  // printf("[Master] Received from Slave: %s\n", buf);

  /* 检查是否为 SYNC 命令 */
  if (strstr(buf, "SYNC") == NULL) {
    fprintf(stderr, "[Master] Unexpected command from Slave: %s\n", buf);
    goto cleanup;
  }

  /* ========== 第4步：回复 +OK ========== */
  const char *ok_resp = "+OK\r\n";
  int send_ret = send(conn_fd, ok_resp, strlen(ok_resp), 0);
  if (send_ret <= 0) {
    perror("send +OK failed");
    goto cleanup;
  }
  // printf("[Master] SYNC acknowledged, sent +OK\n");

  /* 如果 listen_fd 是内部创建的，关闭它 */
  if (should_close_listen && internal_listen_fd >= 0) {
    close(internal_listen_fd);
    internal_listen_fd = -1;
  }
  /* 注意：如果是外部传入的 listen_fd，不关闭，由 Reactor 负责 */

  /* ========== 第5步：通过 RDMA 发送内存数据给 Slave ========== */
  // printf("[Master] Starting RDMA data transfer on port %d...\n", rdma_port);

  ret = rdma_master_send_mem(data, data_size, rdma_port);
  if (ret != 0) {
    fprintf(stderr, "[Master] RDMA data transfer failed\n");
    goto cleanup;
  }
  // printf("[Master] RDMA data transfer complete (%zu bytes)\n", data_size);

  /* 返回 Slave 的 TCP 连接 fd（用于后续 eBPF 转发写命令） */
  *slave_fd = conn_fd;
  conn_fd = -1; /* 防止 cleanup 中关闭 */

  ret = 0;
  goto done;

cleanup:
  ret = -1;

done:
  if (conn_fd >= 0)
    close(conn_fd);
  if (internal_listen_fd >= 0)
    close(internal_listen_fd);
  /* 外部 listen_fd 不由我们关闭 */
  return ret;
}

/*
 * Master 端 RDMA 传输线程入口函数（内存数据版本）
 */
void *rdma_master_send_mem_thread(void *arg) {
  rdma_mem_thread_arg_t *targ = (rdma_mem_thread_arg_t *)arg;
  // printf("[SYNC] RDMA mem transfer thread started, size=%zu, port=%d\n",
  //        targ->data_size, targ->rdma_port);

  int ret = rdma_master_send_mem(targ->data, targ->data_size, targ->rdma_port);
  if (ret == 0) {
    // printf("[SYNC] RDMA mem transfer completed successfully\n");
  } else {
    fprintf(stderr, "[SYNC] RDMA mem transfer failed\n");
  }

  /* 传输完成，释放同步锁，恢复写操作 */
  g_syncing = 0;
  // printf("[SYNC] Sync complete, write operations resumed\n");

  free(arg);
  return NULL;
}
