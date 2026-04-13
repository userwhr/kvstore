# KVStore - 高性能键值存储系统

## 项目概述

KVStore 是一个类 Redis 的高性能键值存储系统，支持主从同步架构和多种数据存储结构。项目采用 C 语言实现，具有高效的内存管理和网络通信能力。

## 系统架构

### 主从同步架构

```
┌─────────────┐     写请求(set/mod/del)     ┌─────────────┐
│             │ ─────────────────────────► │   主节点     │
│   客户端     | ◄───────────────────────── │ (写节点)    │
│             │                            │ 记录WAL日志  │
└─────────────┘                            └──────┬──────┘
                                                │
                                                │ 主从同步（后台日志推送/拉取）
                                                ▼
┌─────────────┐     读请求(get/exist)     ┌─────────────┐
│             │ ◄───────────────────────── │   从节点    │
│   客户端    │ ─────────────────────────► │ (读节点)    │
└─────────────┘                            │ (只读)      │
                                          └─────────────┘
```

### 核心特性

1. **主从分离**：读写分离架构，主节点处理写请求，从节点处理读请求
2. **多数据结构支持**：支持数组、红黑树、哈希表、跳表四种存储结构
3. **全量持久化**：基于 mmap 的文件持久化，支持数据恢复
4. **内存池**：自定义内存管理，提高内存分配效率
5. **Reactor 网络模型**：基于 epoll 的高性能网络框架

## 项目结构

```
kvstore_read/
├── main.c                    # 程序入口点
├── makefile                  # 构建配置
├── include/                  # 头文件目录
│   ├── config.h             # 配置文件
│   ├── kvstore.h            # 主头文件
│   ├── kvs_persist.h        # 持久化相关
│   ├── server.h             # 服务器相关
│   ├── resp.h               # 协议解析
│   └── 数据结构头文件...
├── src/                      # 源代码目录
│   ├── kvstore.c            # 核心逻辑
│   ├── reactor.c            # Reactor 网络框架
│   ├── kvs_persist.c        # 持久化实现
│   ├── kvs_array.c          # 数组实现
│   ├── kvs_rbtree.c         # 红黑树实现
│   ├── kvs_hash.c           # 哈希表实现
│   ├── kvs_skiplist.c       # 跳表实现
│   ├── kvs_mempool.c        # 内存池实现
│   ├── resp.c               # 协议解析实现
│   └── nty_server.c         # 协程服务器（备用）
├── core/                     # 协程库
│   └── libntyco.a           # 预编译的协程库
├── build/                    # 构建输出目录
└── log/                      # 日志文件目录
```

## 网络层 - Reactor 模型

### Reactor 架构

项目采用 Reactor 网络模型，基于 epoll 实现高效的事件驱动：

1. **双端口监听**：
   - 客户端端口：处理客户端的读写请求
   - 主从同步端口：接收主节点的数据同步

2. **事件回调机制**：
   - `accept_cb()`：处理新连接
   - `recv_cb()`：处理数据接收和协议解析
   - `send_cb()`：处理数据发送

3. **连接类型管理**：
   - `CONN_CLIENT`：普通客户端连接
   - `CONN_MASTER`：主节点同步连接

4. **协议解析**：
   - 4字节长度前缀 + 消息体
   - 支持 `\r\n` 分隔符
   - 粘包处理

### 核心函数

```c
// Reactor 入口函数
int react_entrence(
    unsigned short client_port,      // 客户端端口
    unsigned short master_sync_port, // 主从同步端口
    int (*cli_hd)(...),              // 客户端处理函数
    int (*mas_hd)(...)               // 主节点处理函数
);
```

## 数据存储

### 支持的数据结构

1. **数组 (Array)**：简单键值对数组，支持紧凑存储
2. **红黑树 (RBTree)**：自平衡二叉搜索树，O(log n) 操作
3. **哈希表 (Hash)**：链地址法哈希表，快速查找
4. **跳表 (SkipList)**：多层索引结构，高效范围查询

### 配置选项

在 `include/config.h` 中可配置：

```c
#define ENABLE_ARRAY 1      // 启用数组
#define ENABLE_RBTREE 1     // 启用红黑树
#define ENABLE_HASH 1       // 启用哈希表
#define ENABLE_SKIPLIST 1   // 启用跳表
#define ENABLE_SLAB_POOL 1  // 启用内存池
#define START_SLAVE_NODE 1  // 启动从节点模式
```

## 持久化机制

### 全量持久化

系统支持全量持久化，将内存数据持久化到文件中：

1. **mmap 双区映射**：
   - 文件只读区：映射文件真实大小，防止越界
   - 匿名可写区：动态扩容，安全写入

2. **指令级持久化**：
   - `set key value` → 追加到文件
   - `mod key new_value` → 更新文件中对应的 set 指令
   - `del key` → 删除文件中对应的 set 指令

3. **数据恢复**：
   - 启动时读取日志文件
   - 重新执行所有 set 指令
   - 重建内存数据结构

### 核心函数

```c
// 初始化文件映射
int map_file_to_struct(const char *filename, file_content_t *fc);

// 追加指令
int append_mmap_cmd(file_content_t *fc, const char *cmd_type, 
                    const char *key, const char *value);

// 修改指令
int modify_mmap_cmd(file_content_t *fc, const char *mod_type,
                    const char *key, const char *new_value);

// 删除指令
int delete_mmap_cmd(file_content_t *fc, const char *del_type,
                    const char *key);

// 数据恢复
int execute_recovered_cmds(const char *file);
```

## 内存管理

### 自定义内存池

项目实现了 slab 内存池，提高小对象分配效率：

1. **多槽位管理**：按对象大小分槽
2. **批量分配**：每次扩展分配多个对象
3. **内存回收**：正确识别池内内存并回收

### 内存分配函数

```c
void *kvs_malloc(size_t size);  // 使用内存池分配
void kvs_free(void *ptr);       // 使用内存池释放
```

## 协议格式

### 客户端协议

支持简单的文本协议，指令格式：
```
<command> <key> [value]
```

支持指令：
- 写操作：`set`, `del`, `mod`, `rset`, `rdel`, `rmod`, `hset`, `hdel`, `hmod`, `skset`, `skdel`, `skmod`
- 读操作：`get`, `exist`, `rget`, `rexist`, `hget`, `hexist`, `skget`, `skexist`

### 主从同步协议

使用二进制协议：
- 4字节长度前缀（小端）
- 消息体（文本指令）

## 编译与运行

### 编译

```bash
make
```

### 运行

```bash
# 启动从节点（读节点）
./kvstore <port>

# 示例：在端口 8889 启动
./kvstore 8889
```

### 配置说明

1. 端口配置在 `include/config.h`：
   ```c
   #define MASTER_SYNC_PORT 8888 // 主从同步端口
   ```

2. 日志文件路径：
   ```c
   #define LOG_PATH "./log/kvstore_full_log.txt"
   ```

## 测试

项目包含测试脚本：
```bash
# 运行测试
python test.py
```

## 性能特点

1. **高并发**：Reactor 模型支持大量并发连接
2. **低延迟**：内存操作和高效数据结构
3. **数据安全**：全量持久化保证数据不丢失
4. **可扩展**：支持多种数据结构和存储后端

## 未来扩展

1. **集群支持**：多节点分布式部署
2. **增量持久化**：AOF 日志模式
3. **数据分片**：基于键的分片策略
4. **监控指标**：性能监控和统计
