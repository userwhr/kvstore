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
mkdir build

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

# RDMA 配置
# 运行: sudo ./scripts/setup_rdma.sh
```

## 运行

### 启动主节点

```bash
# 启动主服务器
./master

# 输出：
# ========================================
#   KVStore Master Mode
#   Port: 6379
#   RDMA Sync Port: 4000
# ========================================
```

### 启动从节点

```bash
# 编辑 kvstore.conf：设置 ENABLE_SLAVE=1
# 然后启动从服务器
./slave
```

### 使用 redis-cli 连接

```bash
redis-cli -p 6379

# 示例命令
127.0.0.1:6379> SET mykey myvalue
OK
127.0.0.1:6379> GET mykey
"myvalue"
127.0.0.1:6379> HSET myhash field1 value1
(integer) 1
127.0.0.1:6379> HGET myhash field1
"value1"
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

### 性能测试结果

#### 测试环境

| 项目 | 配置 |
|------|------|
| 宿主机系统 | Windows 11 |
| 虚拟机系统 | Ubuntu (Linux 6.17.0-23-generic) |
| CPU | AMD 7500f |
| 内存 | DDR5 6000MT |
| 网络 | 桥接模式，两台虚拟机 |

#### Echo 测试（单连接）

| 指标 | Redis | KVStore |
|------|-------|---------|
| 总请求数 | 1,000,000 | 1,000,000 |
| QPS | 2,064 请求/秒 | 2,084 请求/秒 |
| 平均延迟 | 0.484 ms | 0.480 ms |

#### Pipeline 性能

| Pipeline | KVStore QPS | Redis QPS |
|----------|-------------|-----------|
| 1 | 2,080 | 1,947 |
| 10 | 20,468 | 19,452 |
| 20 | 38,491 | 38,053 |
| 40 | 95,615 | 97,065 |
| 80 | 205,529 | 193,496 |
| 160 | 1,120,795 | 310,816 |

#### AOF 性能影响

| 场景 | QPS | 平均延迟 | 影响 |
|------|-----|----------|------|
| KVStore（关闭 AOF） | 2,296 | 0.4355 ms | 基准 |
| KVStore（开启 AOF） | 2,021 | 0.4949 ms | -12.0% |
| Redis（关闭 AOF） | 3,199 | 0.3126 ms | 基准 |
| Redis（开启 AOF） | 2,725 | 0.3669 ms | -14.8% |

#### 内存使用（jemalloc vs glibc malloc）

| 指标 | jemalloc | glibc malloc |
|------|----------|--------------|
| 100万次 hset 后 RSS | 86,896 KB | 114,368 KB |
| flushall 后 RSS | 63,340 KB | 97,980 KB |
| 内存释放率 | 27% | 14% |
| 每条 hset 平均占用 | ~87 字节 | ~114 字节 |

> jemalloc 在小对象分配场景下内存效率提升 27%。

#### eBPF 增量同步

| 场景 | QPS |
|------|-----|
| 未开启 BPF | 2,030 |
| 开启 BPF（XDP + perf buffer） | 5,592 |

#### RDMA 文件传输（50MB）

| 角色 | 耗时 | 速度 |
|------|------|------|
| 发送端 | 88.51 ms | 564.92 MB/s |
| 接收端 | 92.01 ms | 543.43 MB/s |

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

## 致谢

- 受 Redis 架构设计启发
- 使用 io_uring 实现高性能异步 I/O
- 利用 eBPF/XDP 实现内核级数据包处理
- 通过 RDMA 实现高吞吐量数据复制
