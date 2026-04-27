CC = gcc
TARGET = kvstore
LINK =$(BUILD)kvs_array.o $(BUILD)reactor.o $(BUILD)kvs_rbtree.o \
	$(BUILD)nty_server.o $(BUILD)kvs_hash.o $(BUILD)kvs_skiplist.o \
	$(BUILD)kvstore.o $(BUILD)kvs_persist.o $(BUILD)Cmd.o\
	$(BUILD)kvs_mempool.o $(BUILD)resp.o $(BUILD)io_uringTodesk.o \
	$(BUILD)traversal.o $(BUILD)config.o $(BUILD)rdma_replication.o
NTYCO_COMPLIE = -I ./core/ -L ./core/ -lntyco -lpthread
RDMA_LIBS = -libverbs -lrdmacm
Jemalloc = -ljemalloc
Include_path = ./include/
SRC = ./src/
BUILD = ./build/
kvstore:compile
	$(CC) main.c $(LINK) -I ./include/ $(NTYCO_COMPLIE) -luring $(RDMA_LIBS) $(Jemalloc) -o master
debug:compile
	$(CC) main.c $(LINK) -I ./include/ $(NTYCO_COMPLIE) -luring $(RDMA_LIBS) $(Jemalloc) -o debug
slave:compile
	$(CC) main.c $(LINK) -I ./include/ $(NTYCO_COMPLIE) -luring $(RDMA_LIBS) $(Jemalloc) -o slave
test:
	gcc hiredis_test.c $(Jemalloc) -o kv_client -lhiredis
compile:
	gcc -c $(SRC)kvs_array.c -o $(BUILD)kvs_array.o -I $(Include_path) && \
	gcc -c $(SRC)reactor.c -o $(BUILD)reactor.o -I $(Include_path) && \
	gcc -c $(SRC)kvs_rbtree.c -o $(BUILD)kvs_rbtree.o -I $(Include_path) && \
	gcc -c $(SRC)kvs_hash.c -o $(BUILD)kvs_hash.o -I $(Include_path) && \
	gcc -c $(SRC)kvs_skiplist.c -o $(BUILD)kvs_skiplist.o -I $(Include_path) && \
	gcc -c $(SRC)kvstore.c -o $(BUILD)kvstore.o -I $(Include_path) && \
	gcc -c $(SRC)kvs_persist.c -o $(BUILD)kvs_persist.o -I $(Include_path) && \
	gcc -c $(SRC)kvs_mempool.c -o $(BUILD)kvs_mempool.o -I $(Include_path) && \
	gcc -c $(SRC)resp.c -o $(BUILD)resp.o -I $(Include_path) && \
	gcc -c $(SRC)nty_server.c -I ./core/ -o $(BUILD)nty_server.o && \
	gcc -c $(SRC)Cmd.c -o $(BUILD)Cmd.o && \
	gcc -c $(SRC)io_uringTodesk.c -o $(BUILD)io_uringTodesk.o -I $(Include_path) && \
	gcc -c $(SRC)traversal.c -o $(BUILD)traversal.o -I $(Include_path) && \
	gcc -c $(SRC)config.c -o $(BUILD)config.o -I $(Include_path) && \
	gcc -c $(SRC)rdma_replication.c -o $(BUILD)rdma_replication.o -I $(Include_path)