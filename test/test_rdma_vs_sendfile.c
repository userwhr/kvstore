/*
 * RDMA vs sendfile 传输速度对比测试
 *
 * 测试内容：
 *   1. RDMA 文件传输
 *   2. sendfile 文件传输（TCP socket）
 *
 * 使用方法：
 *   1. 先编译：make rdma_vs_sendfile
 *   2. 生成测试文件：./test/rdma_vs_sendfile generate <size_mb>
 *   3. 测试 sendfile：./test/rdma_vs_sendfile sendfile <file> <ip> <port>
 *   4. 测试 RDMA：./test/rdma_vs_sendfile rdma <file> <port>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <time.h>
#include <unistd.h>

#include "rdma_replication.h"

#define DEFAULT_PORT 18888
#define DEFAULT_RDMA_PORT 18889
#define BUFFER_SIZE (1024 * 1024)  // 1MB

// ====================== 工具函数 ======================

/*
 * 获取当前时间（毫秒）
 */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/*
 * 生成指定大小的测试文件
 */
static int generate_test_file(const char *path, size_t size_mb) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen failed");
        return -1;
    }

    size_t total = size_mb * 1024 * 1024;
    size_t written = 0;
    char buffer[BUFFER_SIZE];

    // 填充测试数据
    memset(buffer, 'A', sizeof(buffer));

    printf("Generating test file: %s (%zu MB)...\n", path, size_mb);

    while (written < total) {
        size_t to_write = total - written;
        if (to_write > sizeof(buffer))
            to_write = sizeof(buffer);

        size_t ret = fwrite(buffer, 1, to_write, fp);
        if (ret != to_write) {
            perror("fwrite failed");
            fclose(fp);
            return -1;
        }
        written += ret;

        // 进度显示
        printf("\rProgress: %zu/%zu MB (%.1f%%)",
               written / (1024*1024), size_mb,
               (double)written / total * 100);
        fflush(stdout);
    }

    printf("\nFile generated successfully.\n");
    fclose(fp);
    return 0;
}

// ====================== sendfile 测试 ======================

/*
 * sendfile 服务端（接收端）
 */
static int sendfile_server(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket failed");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen failed");
        close(listen_fd);
        return -1;
    }

    printf("[sendfile Server] Listening on port %d...\n", port);

    // 接受连接
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (conn_fd < 0) {
        perror("accept failed");
        close(listen_fd);
        return -1;
    }

    printf("[sendfile Server] Client connected.\n");

    // 接收文件大小
    size_t file_size = 0;
    recv(conn_fd, &file_size, sizeof(file_size), 0);
    printf("[sendfile Server] Expecting %zu bytes...\n", file_size);

    // 接收文件数据
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        perror("malloc failed");
        close(conn_fd);
        close(listen_fd);
        return -1;
    }

    size_t total_recv = 0;
    double start_time = get_time_ms();

    while (total_recv < file_size) {
        ssize_t n = recv(conn_fd, buffer, BUFFER_SIZE, 0);
        if (n <= 0) {
            if (n < 0) perror("recv failed");
            break;
        }
        total_recv += n;
    }

    double end_time = get_time_ms();
    double elapsed = end_time - start_time;
    double speed = (total_recv / 1024.0 / 1024.0) / (elapsed / 1000.0);

    printf("\n[sendfile Server] Transfer complete.\n");
    printf("  - Received: %zu bytes (%.2f MB)\n", total_recv, total_recv / 1024.0 / 1024.0);
    printf("  - Time: %.2f ms\n", elapsed);
    printf("  - Speed: %.2f MB/s\n", speed);

    free(buffer);
    close(conn_fd);
    close(listen_fd);
    return 0;
}

/*
 * sendfile 客户端（发送端）
 */
static int sendfile_client(const char *file_path, const char *server_ip, int port) {
    // 打开文件
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        perror("open file failed");
        return -1;
    }

    struct stat st;
    fstat(file_fd, &st);
    size_t file_size = st.st_size;
    printf("[sendfile Client] File: %s (%zu bytes, %.2f MB)\n",
           file_path, file_size, file_size / 1024.0 / 1024.0);

    // 创建 socket
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket failed");
        close(file_fd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    printf("[sendfile Client] Connecting to %s:%d...\n", server_ip, port);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        close(sock_fd);
        close(file_fd);
        return -1;
    }

    // 发送文件大小
    send(sock_fd, &file_size, sizeof(file_size), 0);

    // 使用 sendfile 发送文件
    printf("[sendfile Client] Sending file...\n");

    double start_time = get_time_ms();

    size_t total_sent = 0;
    off_t offset = 0;

    while (total_sent < file_size) {
        ssize_t sent = sendfile(sock_fd, file_fd, &offset, file_size - total_sent);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR)
                continue;
            perror("sendfile failed");
            break;
        }
        total_sent += sent;
    }

    double end_time = get_time_ms();
    double elapsed = end_time - start_time;
    double speed = (total_sent / 1024.0 / 1024.0) / (elapsed / 1000.0);

    printf("\n[sendfile Client] Transfer complete.\n");
    printf("  - Sent: %zu bytes (%.2f MB)\n", total_sent, total_sent / 1024.0 / 1024.0);
    printf("  - Time: %.2f ms\n", elapsed);
    printf("  - Speed: %.2f MB/s\n", speed);

    close(sock_fd);
    close(file_fd);
    return 0;
}

// ====================== RDMA 测试 ======================

/*
 * RDMA 服务端（发送端）
 */
static int rdma_server(const char *file_path, int rdma_port) {
    printf("[RDMA Server] Sending file: %s\n", file_path);

    double start_time = get_time_ms();

    int ret = rdma_master_send_file(file_path, rdma_port);

    double end_time = get_time_ms();
    double elapsed = end_time - start_time;

    if (ret == 0) {
        // 获取文件大小
        struct stat st;
        stat(file_path, &st);
        size_t file_size = st.st_size;
        double speed = (file_size / 1024.0 / 1024.0) / (elapsed / 1000.0);

        printf("\n[RDMA Server] Transfer complete.\n");
        printf("  - File size: %zu bytes (%.2f MB)\n", file_size, file_size / 1024.0 / 1024.0);
        printf("  - Time: %.2f ms\n", elapsed);
        printf("  - Speed: %.2f MB/s\n", speed);
    } else {
        fprintf(stderr, "[RDMA Server] Transfer failed.\n");
    }

    return ret;
}

/*
 * RDMA 客户端（接收端）
 */
static int rdma_client(const char *server_ip, int rdma_port) {
    printf("[RDMA Client] Connecting to %s:%d...\n", server_ip, rdma_port);

    char *data = NULL;
    size_t data_size = 0;

    double start_time = get_time_ms();

    int ret = rdma_slave_recv_file(server_ip, rdma_port, &data, &data_size);

    double end_time = get_time_ms();
    double elapsed = end_time - start_time;

    if (ret == 0) {
        double speed = (data_size / 1024.0 / 1024.0) / (elapsed / 1000.0);

        printf("\n[RDMA Client] Transfer complete.\n");
        printf("  - Received: %zu bytes (%.2f MB)\n", data_size, data_size / 1024.0 / 1024.0);
        printf("  - Time: %.2f ms\n", elapsed);
        printf("  - Speed: %.2f MB/s\n", speed);

        free(data);
    } else {
        fprintf(stderr, "[RDMA Client] Transfer failed.\n");
    }

    return ret;
}

// ====================== 综合对比测试 ======================

/*
 * 综合对比测试
 * 需要在两台机器上分别运行 server 和 client
 */
static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s generate <size_mb> [output_file]  - Generate test file\n", prog);
    printf("  %s sendfile-server <port>             - Start sendfile receiver\n", prog);
    printf("  %s sendfile-client <file> <ip> <port>  - Send file via sendfile\n", prog);
    printf("  %s rdma-server <file> <port>           - Start RDMA sender\n", prog);
    printf("  %s rdma-client <ip> <port>             - Receive file via RDMA\n", prog);
    printf("\nExample:\n");
    printf("  # 生成 100MB 测试文件\n");
    printf("  %s generate 100\n", prog);
    printf("\n");
    printf("  # 测试 sendfile（在接收端运行 server，在发送端运行 client）\n");
    printf("  # Machine A (receiver):\n");
    printf("  %s sendfile-server 18888\n", prog);
    printf("  # Machine B (sender):\n");
    printf("  %s sendfile-client test.dat 192.168.x.x 18888\n", prog);
    printf("\n");
    printf("  # 测试 RDMA（在发送端运行 server，在接收端运行 client）\n");
    printf("  # Machine A (sender):\n");
    printf("  %s rdma-server test.dat 18889\n", prog);
    printf("  # Machine B (receiver):\n");
    printf("  %s rdma-client 192.168.x.x 18889\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "generate") == 0) {
        if (argc < 3) {
            printf("Usage: %s generate <size_mb> [output_file]\n", argv[0]);
            return 1;
        }
        size_t size_mb = atoi(argv[2]);
        const char *output = (argc > 3) ? argv[3] : "test.dat";
        return generate_test_file(output, size_mb);
    }
    else if (strcmp(cmd, "sendfile-server") == 0) {
        int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
        return sendfile_server(port);
    }
    else if (strcmp(cmd, "sendfile-client") == 0) {
        if (argc < 5) {
            printf("Usage: %s sendfile-client <file> <ip> <port>\n", argv[0]);
            return 1;
        }
        const char *file = argv[2];
        const char *ip = argv[3];
        int port = atoi(argv[4]);
        return sendfile_client(file, ip, port);
    }
    else if (strcmp(cmd, "rdma-server") == 0) {
        if (argc < 4) {
            printf("Usage: %s rdma-server <file> <port>\n", argv[0]);
            return 1;
        }
        const char *file = argv[2];
        int port = atoi(argv[3]);
        return rdma_server(file, port);
    }
    else if (strcmp(cmd, "rdma-client") == 0) {
        if (argc < 4) {
            printf("Usage: %s rdma-client <ip> <port>\n", argv[0]);
            return 1;
        }
        const char *ip = argv[2];
        int port = atoi(argv[3]);
        return rdma_client(ip, port);
    }
    else {
        printf("Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
