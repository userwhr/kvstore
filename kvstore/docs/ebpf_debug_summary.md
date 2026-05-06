# eBPF XDP 调试总结

## 问题 1：eBPF 捕获数据末尾多出 0 字节（以太网填充）

### 现象

redis-cli 发送 17 字节的 RESP 命令（`*1\r\n$7\r\nCOMMAND\r\n`），但 XDP 捕获到 20 字节，末尾多出 3 个 `0x00`。

| 发送字节数 | XDP 捕获字节数 | 差异 |
|-----------|---------------|------|
| 17        | 20            | +3   |
| 27        | 27            | 0    |
| 36        | 36            | 0    |

### 根因

以太网帧有最小长度要求（64 字节，不含 4 字节 FCS）。当 IP 包 + 以太网头（14 字节）< 64 字节时，网卡驱动会在帧末尾填充 `0x00` 至最小长度。

XDP 程序运行在 L2 层（网络驱动层），`ctx->data_end` 指向帧的实际末尾，**包含以太网填充字节**。

```
以太网帧结构（小包）：
┌──────────────┬──────────┬──────────┬─────────────┬─────────────┐
│ 以太网头(14) │ IP头(20) │ TCP头(20)│ Payload(17) │ 填充(3)     │
└──────────────┴──────────┴──────────┴─────────────┴─────────────┘
                                               ↑ data_end 指向这里
```

计算过程：
- 以太网帧总长 = 14 + 20 + 20 + 17 = 71 字节
- 最小帧要求 = 64 字节（不含 FCS）
- 71 > 64，实际不需要填充

但某些网卡/驱动对小包有额外处理，导致 `data_end` 包含了填充字节。

### eBPF 代码问题

```c
// ebpf_inc_sync.c 第 221 行
const char *payload_end = (const char *)data_end;  // 包含以太网填充
```

copy 循环用 `payload_end` 做边界检查，导致读取了填充的 0 字节：

```c
#pragma unroll
for (unsigned int i = 0; i < 128; i++) {
    if (i >= copy_len)
        break;
    if (payload + i >= payload_end)  // 边界检查用了 data_end
        break;
    buf[i] = payload[i];
}
```

### 修复

用 `ip->tot_len` 计算真实的 payload 边界，排除以太网填充：

```c
// 修复前
const char *payload_end = (const char *)data_end;

// 修复后
const char *payload_end = payload + payload_len;
```

`payload_len` 通过 `ip->tot_len` 计算，是 IP 层报告的真实长度，不受以太网填充影响：

```c
int ip_total_len = bpf_ntohs(ip->tot_len);
int payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
```

---

## 问题 2：eBPF perf_buffer 跨进程阻塞，跨线程正常

### 现象

- 在主进程中调用 `bpf_object__open_file()` + `perf_buffer__new()` → 正常
- `fork()` 后在子进程中调用 → 卡死在 `perf_buffer__new()`

日志停在：
```
[Forwarder] Creating perf buffer in child (map_fd=15, pid=9328)...
```

### 根因

`fork()` 后子进程继承了父进程的所有文件描述符，包括 `perf_event_open()` 创建的 perf event FD。这些继承的 FD 与 libbpf 内部状态冲突，导致新的 `perf_event_open()` 系统调用阻塞。

这是 libbpf 的已知问题。`perf_buffer__new()` 内部调用链：
```
perf_buffer__new()
  → perf_event_open()  // 阻塞在这里
  → mmap()
```

### 解决方案

使用 `pthread_create()` 替代 `fork()`：

**为什么线程可以：**
- 线程与主进程共享内存空间和文件描述符表
- 不会产生 FD 继承问题
- `perf_buffer__new()` 在线程中正常创建 perf buffer

**代码改动：**

```c
// 改动前：fork 子进程
pid_t pid = fork();
if (pid == 0) {
    // 子进程
    ebpf_load_xdp("ens33");
    ebpf_forwarder_run(g_slave_fd);
    _exit(0);
}

// 改动后：pthread 线程
void *forwarder_thread(void *arg) {
    ebpf_load_xdp("ens33");
    ebpf_forwarder_run(g_slave_fd);
    return NULL;
}

pthread_t tid;
pthread_create(&tid, NULL, forwarder_thread, NULL);
pthread_detach(tid);
```

### 架构对比

```
fork 方案（不工作）：
┌─────────────┐     fork()     ┌─────────────────┐
│ 主进程       │ ──────────→   │ 子进程           │
│ (加载 XDP)   │               │ (创建 perf_buf)  │
│              │               │ 卡死！           │
└─────────────┘               └─────────────────┘

pthread 方案（正常工作）：
┌───────────────────────────────────┐
│ 主进程                             │
│  ┌─────────────┐  ┌─────────────┐ │
│  │ 主线程       │  │ forwarder   │ │
│  │ (reactor)    │  │ 线程        │ │
│  │              │  │ (perf_buf)  │ │
│  └─────────────┘  └─────────────┘ │
└───────────────────────────────────┘
```

---

## 问题 3：Slave 端 reactor 只能收到一次 forwarder 消息

### 现象

Master 的 eBPF forwarder 转发了多条写命令，但 Slave 端的 reactor 只处理了第一条。

### 根因

Slave 收到 forwarder 转发的命令后，通过 `kvs_filter_protocol` 生成响应，再通过 `send_cb` 回发给 Master。但 Master 端的 eBPF forwarder **只写不读**：

1. Slave 发响应 → Master fd 的 TCP 接收缓冲区填满
2. TCP 流控生效 → Slave 的 `send()` 阻塞或返回 `EAGAIN`
3. `send_cb` 中 `send() <= 0` → **直接 `close(fd)`** → 连接断开

### 修复

Slave 端收到 `g_slave_fd` 的命令时，不回发响应：

```c
// reactor.c recv_cb 中
if (g_config.enable_slave && fd == g_slave_fd) {
    // 只消费数据，不回发响应
    memmove(conn->rbuffer, conn->rbuffer + oneRespLen,
            conn->rlength - oneRespLen);
    conn->rlength -= oneRespLen;
    continue;  // 跳过发送队列
}
```

---

## 问题 4：SYNC 完成后写操作仍被阻止

### 现象

SYNC 完成后，客户端发送写命令返回 `-SYNC Syncing in progress`。

### 根因

`ebpf_forwarder_run()` 是阻塞函数，`g_syncing = 0` 在 forwarder 退出后才执行，导致写操作一直被阻止。

### 修复

在 forwarder 启动前就释放同步锁：

```c
// forwarder 启动前，先释放同步锁
g_syncing = 0;
printf("[SYNC] eBPF forwarder starting, write operations resumed\n");

// 然后运行 forwarder
ebpf_forwarder_run(g_slave_fd);
```

---

## 问题 5：头文件缺少 struct perf_buffer 前置声明

### 现象

```
warning: 'struct perf_buffer' declared inside parameter list
error: passing argument 2 of 'ebpf_load_xdp' from incompatible pointer type
```

### 根因

头文件使用了 `struct perf_buffer *` 作为参数类型，但没有前置声明。编译器在参数列表内创建了一个匿名结构体，与 libbpf 定义的 `struct perf_buffer` 是不同类型。

### 修复

在头文件中添加前置声明：

```c
#include <stddef.h>

struct perf_buffer;  // 前置声明，来自 libbpf
```
