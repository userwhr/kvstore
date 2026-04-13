/*
┌─────────────┐     写请求(set/mod/del)     ┌─────────────┐
│             │ ─────────────────────────► │   主节点    │
│   客户端    │ ◄───────────────────────── │ (写节点)    │
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
*/
#define ENABLE_FULL_PERSIST 1
#define ENABLE_INCREMENTAL_PERSIST 0

#define LOG_PATH "./log/kvstore_full_log.txt" // 日志位置
#define MAX_RECOVER_CMD 10000                 // 日志里能存储的最大文件数
#define LINE_MAX 1024 * 10 // SET KEY VALUE 这一个指令的最大长度

// mmap
#define DEFAULT_CAPACITY 4096 * 2 // 初始给匿名mmap分配的空间
#define MMAP_GROW_FACTOR 2        // 不够用时翻倍扩容

// reactor 发送接收缓冲区大小
#define BUFFER_LENGTH 1024 * 10

#define CMD_DELIM "&&" // 新增：多条指令分隔符
#define MAX_CMD_NUM 10000

// 启动四种存储模式
#define ENABLE_RBTREE 1
#define ENABLE_ARRAY 1
#define ENABLE_HASH 1
#define ENABLE_SKIPLIST 1

// 启动内存池
#define ENABLE_SLAB_POOL 1

// 启动主(写)节点
#define START_MASTER_NODE 1