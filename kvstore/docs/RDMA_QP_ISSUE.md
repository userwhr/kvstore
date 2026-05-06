# RDMA QP 不匹配问题分析与修复

## 问题现象

使用 SoftRoCE (rxe) 进行 RDMA 通信时，出现 `event 8`（`RDMA_CM_EVENT_REJECTED`）连接被拒绝的错误。

## 根因分析

### QP 创建时机错误

RDMA 连接建立的核心流程中，**QP（Queue Pair）必须在 `rdma_accept()` / `rdma_connect()` 之前创建**。这是因为：

1. `rdma_connect()` 和 `rdma_accept()` 会触发双方交换 QP 信息（如 QP number）
2. 如果 QP 尚未创建，CM 层无法完成 QP 参数的协商
3. 对端收到连接请求后，发现 QP 信息不匹配，就会回复 `REJECTED`（event 8）

### 旧代码的问题

旧版 [`src/rdma_replication.c`](src/rdma_replication.c) 的流程：

```
Master:  bind -> listen -> CONNECT_REQUEST -> accept -> 分配PD/CQ/MR -> 建立连接
                                                              ^
                                                      QP 未创建！
```

旧代码在 `rdma_accept()` **之后**才分配 PD、CQ 等资源，**根本没有调用 `rdma_create_qp()`**。这意味着：

- Master 调用 `rdma_accept()` 时，QP 还不存在
- CM 层无法完成 QP 握手
- Slave 收到 `REJECTED` 事件

### testrdma 的正确架构

参照 [`testrdma/test_server.c`](testrdma/test_server.c) 和 [`testrdma/test_client.c`](testrdma/test_client.c) 的正确流程：

```
Master (test_server):
  bind -> listen -> CONNECT_REQUEST -> 分配PD/CQ -> 创建QP -> accept(带conn_param) -> ESTABLISHED
                                                              ^
                                                     QP 已就绪！

Slave (test_client):
  resolve_addr -> resolve_route -> 分配PD/CQ -> 创建QP -> connect(带conn_param) -> ESTABLISHED
                                                          ^
                                                 QP 已就绪！
```

关键区别在于：
- **旧代码**：`accept` → 分配资源（QP 未创建）
- **正确代码**：分配资源 → `create_qp` → `accept`/`connect`（QP 已就绪）

## 其他修复点

### 1. 缺少连接参数配置

旧代码的 `conn_param` 全为 0，而 SoftRoCE 需要正确设置：

| 参数 | 值 | 说明 |
|------|-----|------|
| `responder_resources` | 1 | 响应端资源数 |
| `initiator_depth` | 1 | 发起端资源数 |
| `retry_count` | 7 | 重试次数 |
| `rnr_retry_count` | 7 | RNR 重试次数 |
| `flow_control` | 1 | 启用流控 |

### 2. Slave 端缺少路由解析

旧代码 Slave 端在 `rdma_resolve_addr` 后没有调用 `rdma_resolve_route`，导致路由未建立。

### 3. 内存数据同步问题

SYNC 时 `dump_all_to_disk()` 写入新数据到文件后，旧代码直接使用 `kvs_log->file_data`（旧的 mmap 映射），导致发送的是旧数据。修复后改为重新打开文件读取最新内容发送。

### 4. 文件格式不一致

`dump_all_to_disk()` 写入格式为 `klen vlen key value`（key 和 value 之间有空格），但 `parse_kv_line()` 期望的格式是 `klen vlen keyvalue`（key 和 value 连续）。修复后将 `%s %s` 改为 `%s%s`。

## 总结

| 问题 | 旧代码 | 修复后 |
|------|--------|--------|
| QP 创建时机 | accept/connect 之后 | accept/connect 之前 |
| 连接参数 | 全为 0 | 设置 retry/rnr/flow_control |
| 路由解析 | 缺少 | 添加 rdma_resolve_route |
| 数据源 | 旧 mmap 映射 | 重新读取文件 |
| 文件格式 | `key value` 带空格 | `keyvalue` 连续 |
