# KVStore

一个用 C 语言编写的高性能键值存储引擎，兼容 Redis RESP 协议。

## 特性

- **多数据结构支持**：数组、红黑树、哈希表、跳表
- **高性能**：基于 Epoll 的 Reactor 模型 + io_uring 异步 I/O
- **Redis 兼容**：完整 RESP 协议支持，可使用 `redis-cli` 连接
- **持久化**：AOF（追加日志）和 RDB 快照
- **主从复制**：RDMA 全量同步 + eBPF XDP 增量同步
- **内存优化**：集成 jemalloc 内存分配器
- **Pipeline 支持**：批量命令执行，大幅提升吞吐量
- **配置驱动**：灵活的配置文件控制所有参数

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│                      KVStore Master                         │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │  Array   │  │  RBTree  │  │   Hash   │  │ Skiplist │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘   │
├─────────────────────────────────────────────────────────────┤
│                      存储引擎层                              │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │   Reactor    │  │   io_uring   │  │   RESP 解析器    │  │
│  │   (epoll)    │  │   异步 I/O   │  │   协议处理       │  │
│  └──────────────┘  └──────────────┘  └──────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │   AOF/RDB    │  │   RDMA       │  │   eBPF/XDP       │  │
│  │   持久化     │  │   主从复制   │  │   增量同步       │  │
│  └──────────────┘  └──────────────┘  └──────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## 支持的命令

| 类别 | 命令 |
|------|------|
| **字符串** | `SET`, `GET`, `DEL`, `MOD`, `EXIST` |
| **红黑树** | `RSET`, `RGET`, `RDEL`, `RMOD`, `REXIST` |
| **哈希表** | `HSET`, `HGET`, `HDEL`, `HMOD`, `HEXIST` |
| **跳表** | `SKSET`, `SKGET`, `SKDEL`, `SKMOD`, `SKEXIST` |
| **工具命令** | `SAVE`, `FLUSHALL`, `DBCOUNT`, `ECHO`, `COMMAND`, `SYNC` |

## 环境要求

### 系统要求

- Linux（内核 >= 5.1，支持 io_uring）
- GCC >= 9.0
- Clang/LLVM（编译 eBPF 程序）

### 依赖库

| 库 | 用途 | Ubuntu/Debian 安装包 |
|----|------|---------------------|
| liburing | io_uring 异步 I/O | `liburing-dev` |
| libibverbs | RDMA verbs | `libibverbs-dev` |
| librdmacm | RDMA 连接管理 | `librdmacm-dev` |
| libbpf | eBPF 库 | `libbpf-dev` |
| libelf | ELF 解析（eBPF 依赖） | `libelf-dev` |
| zlib | 压缩（eBPF 依赖） | `zlib1g-dev` |
| jemalloc | 内存分配器（可选） | `libjemalloc-dev` |
| hiredis | Redis 客户端（测试用） | `libhiredis-dev` |

一键安装所有依赖（Ubuntu/Debian）：

```bash
# 必需依赖
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    clang \
    llvm \
    liburing-dev \
    libibverbs-dev \
    librdmacm-dev \
    libbpf-dev \
    libelf-dev \
    zlib1g-dev

# 可选：jemalloc 内存分配器（推荐）
sudo apt-get install -y libjemalloc-dev

# 测试工具
sudo apt-get install -y libhiredis-dev
```

## 编译

### 快速开始

```bash
# 克隆仓库
git clone <repository-url>
cd kvstore
mkdir build log

# 编译（默认使用 jemalloc）
make

```

### 编译目标

| 目标 | 说明 |
|------|------|
| `make` | 编译主服务器（含 jemalloc 和 eBPF） |
| `make nojemalloc` | 编译主服务器（不含 jemalloc） |
| `make debug` | 编译调试版本（含调试符号） |
| `make slave` | 编译从服务器 |
| `make test` | 编译测试客户端 |
| `make bench` | 编译性能测试工具 |

### 编译产物

- `master` - 主服务器
- `slave` - 从服务器
- `kv_client` - 测试客户端
- `test/benchmark` - 性能测试工具

## 配置

运行前编辑 `kvstore.conf`：
### 使用 redis-cli 连接

```bash
redis-cli

# 示例命令
127.0.0.1:6379> SET mykey myvalue
OK
127.0.0.1:6379> GET mykey
"myvalue"
```

## 测试

### 运行性能测试

```bash
# 编译测试工具
make bench

# 基础测试
./test/benchmark -t set -n 100000 -c 1 -p 6379

# Pipeline 测试
./test/benchmark_pipeline -t hset -n 100000 -c 1 -P 10 -H <host> -p <port>
```

## RDMA 配置

主从复制需要配置 RDMA：

```bash
# 加载 RDMA 内核模块
sudo modprobe rdma_rxe

# 绑定网卡到 RDMA 设备
sudo ./scripts/setup_rdma.sh

# 或手动绑定：
sudo rdma link add rxe0 type rxe netdev ens33
```

## 项目结构

```
kvstore/
├── include/              # 头文件
│   ├── kvstore.h        # 核心数据结构
│   ├── config.h         # 配置管理
│   ├── reactor.h        # Epoll Reactor
│   ├── rbtree.h         # 红黑树
│   ├── hash.h           # 哈希表
│   ├── skiplist.h       # 跳表
│   ├── array.h          # 数组存储
│   ├── resp.h           # RESP 协议
│   ├── kvs_persist.h    # 持久化
│   ├── rdma_replication.h # RDMA 复制
│   ├── ebpf_forwarder.h # eBPF 同步
│   └── ...
├── src/                  # 源文件
│   ├── main.c           # 程序入口
│   ├── kvstore.c        # 核心逻辑
│   ├── reactor.c        # Epoll Reactor
│   ├── rbtree.c         # 红黑树实现
│   ├── hash.c           # 哈希表实现
│   ├── skiplist.c       # 跳表实现
│   ├── resp.c           # RESP 协议解析
│   ├── kvs_persist.c    # 持久化实现
│   ├── rdma_replication.c # RDMA 复制实现
│   ├── ebpf_forwarder.c # eBPF 同步实现
│   ├── XDP.bpf.c        # XDP BPF 程序
│   └── ...
├── test/                 # 测试和性能工具
│   ├── benchmark.c      # 基础性能测试
│   ├── benchmark_pipeline.c # Pipeline 测试
│   └── ...
├── scripts/              # 辅助脚本
│   └── setup_rdma.sh    # RDMA 配置脚本
├── kvstore.conf          # 配置文件
├── makefile              # 构建系统
└── README.md             # 项目说明
```
# KVSTORE 性能测试报告
## 二、echo benchmark 测试

### 2.1 单连接测试（无 pipeline）

> 测试指令：`redis-benchmark -n 100000 -P 1 -p 6379 -c 1`
> 并发连接数：1，Pipeline 大小：1（无 pipeline）

| 指标 | Redis | kvstore |
|------|-----------------|-----------------|
| QPS | 6035 请求/秒 | 5983 请求/秒 |
| 平均延迟 | 0.157 ms/请求 | 0.159 ms/请求 |

---

## 三、AOF 的性能对比

> 测试条件：单连接（c=1），无 pipeline（P=1）
> 目的：对比增量持久化/AOF 开关对性能的影响

| 测试场景 | QPS (请求/秒) | 平均延迟 | 性能影响（相对关闭AOF） |
|----------|---------------|----------|------------------------|
| kvstore 关闭 AOF | 5994 | 0.4355 ms | 基准 |
| kvstore 开启 AOF | 5867 | 0.4949 ms | -10.0% |
| Redis AOF 关闭 | 6041 | 0.3126 ms | 基准 |
| Redis AOF 开启 | 5084 | 0.3669 ms | -7.4% |

## 四、Pipeline 性能测试

> 测试命令：`redis-benchmark -n 1000000 -P <> -c 1 -t set`

| Pipeline | kvstore QPS | Redis QPS |
|----------|-------------|-----------|
| 1        | 5994       | 6041     |
| 10       | 60970      | 58854    |
| 20       | 120851      | 109205    |
| 40       | 239341    | 213367   |
| 80       | 433050     | 394562   |
| 160      | 811523     | 649456   |

## 五、内存测试

> 测试方法：通过 `/proc/<pid>/status` 读取 VmSize（虚拟内存）和 VmRSS（物理内存）
> 测试流程：启动 → 写入 100w 条 rset → flushall
> jemalloc 版本 FLUSHALL 后通过 `mallctl arena.purge` 主动归还 dirty 页给 OS

### 详细数据（jemalloc + arena.purge）

| 阶段 | VmSize (KB) | VmRSS (KB) | VmHWM (KB) | RssAnon (KB) |
|------|-------------|------------|------------|--------------|
| 初始状态 | 34,876 | 6,080 | 6,080 | 644 |
| 100w rset 后 | 111,676 | 78,368 | 78,368 | 72,932 |
| flushall 后 | 111,676 | 24,808 | 78,504 | 19,244 |
| **释放量** | 0 | **-53,560 (-68.3%)** | - | -53,688 |

### 详细数据（glibc malloc）

| 阶段 | VmSize (KB) | VmRSS (KB) | VmHWM (KB) | RssAnon (KB) |
|------|-------------|------------|------------|--------------|
| 初始状态 | 20,544 | 3,340 | 3,340 | 320 |
| 100w rset 后 | 145,944 | 129,300 | 129,300 | 126,280 |
| flushall 后 | 145,880 | 32,592 | 129,080 | 29,572 |
| **释放量** | -64 | **-96,708 (-74.8%)** | - | -96,708 |

### 汇总对比

| 分配器 | VmRSS 启动 | VmRSS 峰值 | VmRSS 结束 | 每条占用 | flushall 释放率 |
|--------|------------|------------|------------|----------|----------------|
| jemalloc + purge | 6,080 | 78,368 | 24,808 | ~72 字节 | 68.3% |
| glibc malloc | 3,340 | 129,300 | 32,592 | ~126 字节 | **74.8%** |

### 结论

| 指标 | jemalloc + purge | glibc malloc | 差异 |
|------|------------------|--------------|------|
| 100w rset 后 VmRSS | 78,368 KB | 129,300 KB | **jemalloc 省 50 MB** |
| flushall 后 VmRSS | 24,808 KB | 32,592 KB | **jemalloc 省 7.6 MB** |
| flushall 内存释放率 | 68.3% | 74.8% | glibc 回收率略高 |
| 每条 rset 平均占用 | 72 字节 | 126 字节 | **jemalloc 省 43%** |

> jemalloc 的核心优势在**分配阶段**：每条数据省 54 字节，100w 条节省约 50 MB RSS。flushall 后两者 RSS 接近（24 MB vs 32 MB），但 glibc 在峰值时多占用了 50 MB。jemalloc 的 `arena.purge` 可主动归还 dirty 页，避免内存长期驻留。

## 六、Save 持久化性能影响测试

> 测试条件：单连接（c=1），Pipeline=1，命令 SET
> 总命令数：1,000,000（100 万条）
> 基准：无 SAVE 时 QPS = 6000
> 目的：评估不同 SAVE 间隔对整体 QPS 的影响

### 不同 SAVE 间隔的 QPS 对比

| SAVE 间隔 | SAVE 次数 | 总耗时 | QPS | 相对基准 |
|-----------|----------|-------|-----|---------|
| 不落盘 | 0 | 167s | **6000** | 基准 |
| 1,000 | 1000 | 317s | **3155** | -47.4% |
| 10,000 | 100 | 178s | **5618** | -6.4% |
| 100,000 | 10 | 168s | **5949** | -0.9% |
| 1,000,000 | 1 | 167s | **5990** | -0.2% |


## 七、eBPF 增量同步性能测试

> 测试条件：单连接（c=1），无 pipeline（P=1），端口 6379
> 目的：评估 eBPF XDP 拦截转发对主路径 QPS 的影响
> 命令：`./benchmark_pipeline -t <cmd> -n 100000 -c 1 -P 1 -H 192.168.5.17 -b 192.168.5.20 -p 6379`

| 测试场景 | 命令 | QPS (请求/秒) |
|----------|------|---------------|
| 未开启 BPF | set | 1215 |
| 开启 BPF| hset | 980 |


## 八、sendfile 文件传输测试

> 测试条件：50 MB 文件，两台虚拟机通过 ens33 网卡（桥接模式）传输
> 工具：`test_rdma_vs_sendfile`
> 注：RDMA 测试因虚拟机使用 Soft-RoCE（软件模拟），速度仅 ~5.15 MB/s

| 角色 | 耗时 | 传输速度 |
|------|------|----------|
| 客户端（发送端） | 88.51 ms | 564.92 MB/s |
| 服务端（接收端） | 92.01 ms | 543.43 MB/s |
