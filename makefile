CC = gcc
CLANG = clang
TARGET = kvstore
LINK =$(BUILD)array.o $(BUILD)reactor.o $(BUILD)rbtree.o \
	$(BUILD)hash.o $(BUILD)skiplist.o \
	$(BUILD)kvstore.o $(BUILD)kvs_persist.o $(BUILD)Cmd.o\
	$(BUILD)kvs_mempool.o $(BUILD)resp.o $(BUILD)io_uringTodesk.o \
	$(BUILD)traversal.o $(BUILD)config.o $(BUILD)rdma_replication.o \
	$(BUILD)master_service.o $(BUILD)slave_service.o $(BUILD)ebpf_forwarder.o \
	$(BUILD)slave_manager.o
RDMA_LIBS = -libverbs -lrdmacm
Jemalloc = -ljemalloc
Include_path = ./include/
SRC = ./src/
BUILD = ./build/
EBPF_BUILD = ./build/ebpf/
BPF_CFLAGS = -O2 -target bpf -g
BPF_HEADERS = -I/usr/include/x86_64-linux-gnu
EBPF_LIBS = -lbpf -lelf -lz

kvstore:compile ebpf
	$(CC) main.c $(LINK) -I ./include/ -luring $(RDMA_LIBS) $(EBPF_LIBS) $(Jemalloc) -o kvstore
debug:compile_debug ebpf
	$(CC) -g main.c $(LINK) -I ./include/ -luring $(RDMA_LIBS) $(EBPF_LIBS) $(Jemalloc) -o debug
slave:compile
	$(CC) main.c $(LINK) -I ./include/ -luring $(RDMA_LIBS) $(EBPF_LIBS) $(Jemalloc) -o slave
nojemalloc:compile_nojemalloc ebpf
	$(CC) main.c $(LINK) -I ./include/ -luring $(RDMA_LIBS) $(EBPF_LIBS) -o noje
test:
	gcc -O2 hiredis_test.c -o kv_client -lhiredis -lpthread
bench:
	gcc -O2 test/benchmark.c -o test/benchmark

# ===================== eBPF 增量同步 =====================
ebpf: $(EBPF_BUILD)ebpf_inc_sync.o
	@echo "eBPF BPF程序编译完成！"

$(EBPF_BUILD)ebpf_inc_sync.o: $(SRC)XDP.bpf.c
	@mkdir -p $(EBPF_BUILD)
	$(CLANG) $(BPF_CFLAGS) $(BPF_HEADERS) -c $< -o $@

# =========================================================

# 独立的 eBPF forwarder 可执行文件（用于 fork+exec 方式）
ebpf_forwarder_main: $(EBPF_BUILD)ebpf_forwarder_main

$(EBPF_BUILD)ebpf_forwarder_main: $(SRC)ebpf_forwarder_main.c $(BUILD)ebpf_forwarder.o $(BUILD)config.o
	@mkdir -p $(EBPF_BUILD)
	$(CC) -O2 -g -DUSE_JEMALLOC $^ -I $(Include_path) $(EBPF_LIBS) -o $@
	@echo "独立 eBPF forwarder 编译完成！"

# =========================================================

compile:
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)array.c -o $(BUILD)array.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)reactor.c -o $(BUILD)reactor.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)rbtree.c -o $(BUILD)rbtree.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)hash.c -o $(BUILD)hash.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)skiplist.c -o $(BUILD)skiplist.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)kvstore.c -o $(BUILD)kvstore.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)kvs_persist.c -o $(BUILD)kvs_persist.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)kvs_mempool.c -o $(BUILD)kvs_mempool.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)resp.c -o $(BUILD)resp.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)Cmd.c -o $(BUILD)Cmd.o && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)io_uringTodesk.c -o $(BUILD)io_uringTodesk.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)traversal.c -o $(BUILD)traversal.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)config.c -o $(BUILD)config.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)rdma_replication.c -o $(BUILD)rdma_replication.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)master_service.c -o $(BUILD)master_service.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)slave_service.c -o $(BUILD)slave_service.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)ebpf_forwarder.c -o $(BUILD)ebpf_forwarder.o -I $(Include_path) && \
	gcc -O2 -DUSE_JEMALLOC -c $(SRC)slave_manager.c -o $(BUILD)slave_manager.o -I $(Include_path)

compile_debug:
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)array.c -o $(BUILD)array.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)reactor.c -o $(BUILD)reactor.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)rbtree.c -o $(BUILD)rbtree.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)hash.c -o $(BUILD)hash.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)skiplist.c -o $(BUILD)skiplist.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)kvstore.c -o $(BUILD)kvstore.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)kvs_persist.c -o $(BUILD)kvs_persist.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)kvs_mempool.c -o $(BUILD)kvs_mempool.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)resp.c -o $(BUILD)resp.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)Cmd.c -o $(BUILD)Cmd.o && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)io_uringTodesk.c -o $(BUILD)io_uringTodesk.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)traversal.c -o $(BUILD)traversal.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)config.c -o $(BUILD)config.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)rdma_replication.c -o $(BUILD)rdma_replication.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)master_service.c -o $(BUILD)master_service.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)slave_service.c -o $(BUILD)slave_service.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)ebpf_forwarder.c -o $(BUILD)ebpf_forwarder.o -I $(Include_path) && \
	gcc -O2 -g -DUSE_JEMALLOC -c $(SRC)slave_manager.c -o $(BUILD)slave_manager.o -I $(Include_path)

compile_nojemalloc:
	gcc -O2 -c $(SRC)array.c -o $(BUILD)array.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)reactor.c -o $(BUILD)reactor.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)rbtree.c -o $(BUILD)rbtree.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)hash.c -o $(BUILD)hash.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)skiplist.c -o $(BUILD)skiplist.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)kvstore.c -o $(BUILD)kvstore.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)kvs_persist.c -o $(BUILD)kvs_persist.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)kvs_mempool.c -o $(BUILD)kvs_mempool.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)resp.c -o $(BUILD)resp.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)Cmd.c -o $(BUILD)Cmd.o && \
	gcc -O2 -c $(SRC)io_uringTodesk.c -o $(BUILD)io_uringTodesk.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)traversal.c -o $(BUILD)traversal.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)config.c -o $(BUILD)config.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)rdma_replication.c -o $(BUILD)rdma_replication.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)master_service.c -o $(BUILD)master_service.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)slave_service.c -o $(BUILD)slave_service.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)ebpf_forwarder.c -o $(BUILD)ebpf_forwarder.o -I $(Include_path) && \
	gcc -O2 -c $(SRC)slave_manager.c -o $(BUILD)slave_manager.o -I $(Include_path)
