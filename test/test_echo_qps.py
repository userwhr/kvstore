import socket
import time
import threading

SERVER_IP = "127.0.0.1"
SERVER_PORT = 2000
BUFFER_SIZE = 4096

TOTAL_REQUESTS = 300000   # 发送总量
THREAD_COUNT = 10        # 并发线程数


def encode_resp(*args):
    """编码 RESP 协议: *N\r\n$len\r\narg\r\n..."""
    parts = [f"*{len(args)}\r\n"]
    for arg in args:
        s = str(arg)
        parts.append(f"${len(s)}\r\n{s}\r\n")
    return "".join(parts).encode("utf-8")


def decode_resp(data):
    """解码 RESP bulk string 响应: $len\r\nvalue\r\n"""
    s = data.decode("utf-8") if isinstance(data, bytes) else data
    if s.startswith("$"):
        idx = s.find("\r\n")
        if idx >= 0:
            length = int(s[1:idx])
            value = s[idx + 2: idx + 2 + length]
            return value
    return s


class TCPClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.connect((SERVER_IP, SERVER_PORT))

    def send_command(self, *args):
        """发送 RESP 格式命令"""
        self.sock.sendall(encode_resp(*args))

    def recv_response(self):
        """接收 RESP 响应"""
        response = b""
        while True:
            chunk = self.sock.recv(BUFFER_SIZE)
            if not chunk:
                break
            response += chunk
            # 检查是否收到完整 RESP 响应
            try:
                decoded = response.decode("utf-8")
                if decoded.startswith("$"):
                    idx = decoded.find("\r\n")
                    if idx >= 0:
                        length = int(decoded[1:idx])
                        if len(decoded) >= idx + 2 + length + 2:
                            return decoded[:idx + 2 + length + 2]
                elif decoded.startswith("+") or decoded.startswith("-"):
                    if "\r\n" in decoded:
                        return decoded
            except UnicodeDecodeError:
                continue
        return response.decode("utf-8", errors="replace")

    def close(self):
        self.sock.close()


def worker(thread_id, count, results):
    """单个线程的工作函数"""
    success = 0
    fail = 0
    try:
        client = TCPClient()
        for i in range(count):
            msg = f"hello_{thread_id}_{i}"
            client.send_command("echo", msg)
            resp = client.recv_response()
            expected = f"${len(msg)}\r\n{msg}\r\n"
            if resp == expected:
                success += 1
            else:
                fail += 1
                if fail <= 3:
                    print(f"  线程{thread_id} 第{i}条: 预期[{expected}] 实际[{resp}]")
        client.close()
    except Exception as e:
        print(f"线程 {thread_id} 异常: {e}")
        fail += count - success
    results[thread_id] = (success, fail)


def test_echo_qps(total_requests=10000, thread_count=10):
    """测试 echo 命令的 QPS"""
    print(f"Echo QPS 测试 (RESP 协议)")
    print(f"  总请求数: {total_requests}")
    print(f"  并发线程: {thread_count}")
    print(f"  服务器:   {SERVER_IP}:{SERVER_PORT}")
    print("=" * 50)

    # 功能验证
    print("\n[1] 功能验证...")
    try:
        client = TCPClient()
        client.send_command("echo", "test123")
        resp = client.recv_response()
        client.close()
        if resp == "$7\r\ntest123\r\n":
            print("    echo 功能正常")
        else:
            print(f"    echo 响应异常: {repr(resp)}")
            return
    except Exception as e:
        print(f"    连接失败: {e}")
        return

    # QPS 测试
    print(f"\n[2] QPS 测试 ({thread_count} 线程)...")
    requests_per_thread = total_requests // thread_count
    results = [None] * thread_count

    start_time = time.time()

    threads = []
    for i in range(thread_count):
        t = threading.Thread(target=worker, args=(i, requests_per_thread, results))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    end_time = time.time()
    total_time = end_time - start_time

    # 统计结果
    total_success = sum(r[0] for r in results if r)
    total_fail = sum(r[1] for r in results if r)
    actual_total = total_success + total_fail
    qps = actual_total / total_time if total_time > 0 else 0

    print("\n" + "=" * 50)
    print("测试结果:")
    print(f"  总耗时:   {total_time:.2f}s")
    print(f"  成功数:   {total_success}")
    print(f"  失败数:   {total_fail}")
    print(f"  QPS:      {qps:.0f} ops/s")
    print("=" * 50)


if __name__ == "__main__":
    test_echo_qps(TOTAL_REQUESTS, THREAD_COUNT)
