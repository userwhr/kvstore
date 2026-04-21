import socket
import sys
import time
from typing import Optional

# 服务器配置（固定不变）
SERVER_IP = "192.168.140.134"
SERVER_PORT = 2001
BUFFER_SIZE = 4096
TEST_COUNT = 1000  # 固定10000条 key1~key10000

# 预置大字符串（Value）
LARGE_VALUE = "测试大Value内容：" + "abcdefghijklmnopqrstuvwxyz1234567890" * 100

# 预置大Key（超长Key名称，用于rset等操作）
BIG_KEY = "bigkey_" + "a" * 5000   # 超长Key，长度约5000+
BIG_KEY_VALUE = "value_for_big_key"  # 关联的普通Value

class TCPClient:
    """【已改造】标准 RESP2 协议客户端，适配你的 C 服务端"""
    def __init__(self):
        self.sock: Optional[socket.socket] = None
        self.connect()

    def connect(self):
        """建立TCP连接"""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock.connect((SERVER_IP, SERVER_PORT))
            print(f"✅ 成功连接到 {SERVER_IP}:{SERVER_PORT}")
            print("📌 指令说明：")
            print("   - 输入 'exit' 退出")
            print("   - 输入 'large' 发送大value指令")
            print("   - 输入 'bigkey' 测试大Key操作 (rset/rget/rmod/rexist/rdel)")
            print("   - 输入 'bigvalue' 测试大Value的mod/get操作")
            print("   - 基础指令：set/get/mod/exist/del 批量测试")
            print("   - r前缀指令：rset/rget/rmod/rexist/rdel 批量测试")
            print("   - h前缀指令：hset/hget/hmod/hexist/hdel 批量测试")
            print("   - sk前缀指令：skset/skget/skmod/skexist/skdel 批量测试")
            print("   - 输入 'save' 保存全部数据")
            print("   - 其他输入为手动发送指令")
        except Exception as e:
            print(f"❌ 连接失败: {e}")
            sys.exit(1)

    # ===================== 【核心：RESP2 协议打包函数】 =====================
    def build_resp(self, cmd_str: str) -> bytes:
        args = cmd_str.strip().split()
        if not args:
            return b""
        
        resp = f"*{len(args)}\r\n"
        for arg in args:
            arg_bytes = arg.encode("utf-8")
            resp += f"${len(arg_bytes)}\r\n{arg}\r\n"
        
        return resp.encode("utf-8")

    # ===================== 【替换：发送函数（纯RESP）】 =====================
    def send_message(self, cmd: str, is_batch: bool = False) -> bool:
        if not self.sock:
            print("❌ 错误：未建立连接")
            return False
        try:
            resp_msg = self.build_resp(cmd)
            self.sock.sendall(resp_msg)
            return True
        except Exception as e:
            print(f"❌ 发送失败: {e}")
            return False

    # ===================== 接收函数（移除strip，保留原始\r\n） =====================
    def recv_message(self, is_batch: bool = False) -> Optional[str]:
        if not self.sock:
            return None
        try:
            response = b""
            while True:
                chunk = self.sock.recv(BUFFER_SIZE)
                if not chunk:
                    break
                response += chunk
                if len(chunk) < BUFFER_SIZE:
                    break
            if not response:
                print("❌ 连接被服务器关闭")
                return None
            # 🔥 关键修改：不strip，保留原始的 \r\n 结尾
            return response.decode("utf-8")
        except Exception as e:
            print(f"❌ 接收失败: {e}")
            return None

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None
            print("\n🔌 连接已关闭")

# ===================== 批量测试（校验规则添加 \r\n） =====================
def run_batch(client: TCPClient, op: str, mul: int):
    success = 0
    fail = 0
    start_time = time.time()
    print(f"\n🚀 启动【{op}】批量测试：key1~key{TEST_COUNT} | 倍数={mul}")
    print("="*65)

    prefix = ""
    if op.startswith("sk"):
        prefix = "sk"
        base_op = op[2:]
    elif op.startswith(("r", "h")):
        prefix = op[0]
        base_op = op[1:]
    else:
        prefix = ""
        base_op = op

    valid_base_ops = ["set", "get", "mod", "exist", "del"]
    if base_op not in valid_base_ops:
        print(f"❌ 不支持的基础操作：{base_op}")
        return

    for i in range(1, TEST_COUNT+1):
        val = i * mul
        if base_op == "set":
            cmd = f"{prefix}set key{i} {val}"
            expect = "OK\r\n"  # 🔥 加 CRLF
        elif base_op == "get":
            cmd = f"{prefix}get key{i}"
            expect = f"{val}\r\n"  # 🔥 加 CRLF
        elif base_op == "mod":
            cmd = f"{prefix}mod key{i} {val}"
            expect = "OK\r\n"  # 🔥 加 CRLF
        elif base_op == "exist":
            cmd = f"{prefix}exist key{i}"
            expect = "exist\r\n"  # 🔥 加 CRLF
        elif base_op == "del":
            cmd = f"{prefix}del key{i}"
            expect = "OK\r\n"  # 🔥 加 CRLF
        else:
            fail +=1
            continue

        if not client.send_message(cmd, is_batch=True):
            fail +=1
            continue
        resp = client.recv_message(is_batch=True)
        # 严格匹配带 \r\n 的响应
        if resp and resp == expect:
            success +=1
        else:
            fail +=1

        if i % 1000 == 0:
            print(f"📊 进度：{i}/{TEST_COUNT} | 成功：{success} | 失败：{fail}")

    total_time = time.time() - start_time
    print("\n" + "="*65)
    print("📊 测试完成")
    print(f"操作：{op} | 总数：{TEST_COUNT}")
    print(f"✅ 成功：{success} | ❌ 失败：{fail}")
    print(f"⏱️ 总耗时：{total_time:.2f}s | 🚀 速率：{TEST_COUNT/total_time:.2f}条/秒")
    print("="*65)

# ===================== 大Key测试 =====================
def test_big_key(client: TCPClient):
    print("\n🔑 开始大Key完整测试（超长Key名称）")
    print("=" * 65)

    cmd_set = f"rset {BIG_KEY} {BIG_KEY_VALUE}"
    print(f"📤 发送: {cmd_set[:100]}...")
    if not client.send_message(cmd_set):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rset响应: {repr(resp)}")  # repr 显示 \r\n

    cmd_get = f"rget {BIG_KEY}"
    print(f"\n📤 发送: {cmd_get[:100]}...")
    client.send_message(cmd_get)
    resp = client.recv_message()
    print(f"📩 rget响应: {resp[:100] if resp else '空'}...")

    new_value = "modified_value_for_big_key"
    cmd_mod = f"rmod {BIG_KEY} {new_value}"
    print(f"\n📤 发送: {cmd_mod[:100]}...")
    client.send_message(cmd_mod)
    resp = client.recv_message()
    print(f"📩 rmod响应: {repr(resp)}")

    cmd_exist = f"rexist {BIG_KEY}"
    print(f"\n📤 发送: {cmd_exist[:100]}...")
    client.send_message(cmd_exist)
    resp = client.recv_message()
    print(f"📩 rexist响应: {repr(resp)}")

    cmd_del = f"rdel {BIG_KEY}"
    print(f"\n📤 发送: {cmd_del[:100]}...")
    client.send_message(cmd_del)
    resp = client.recv_message()
    print(f"📩 rdel响应: {repr(resp)}")

    print("\n" + "=" * 65)
    print("🔑 大Key测试完成")
    print("=" * 65)

# ===================== 大Value测试 =====================
def test_big_value_mod_get(client: TCPClient):
    print("\n💾 开始大Value的mod/get测试")
    print("=" * 65)

    test_key = "bigvalue_key"
    cmd_set = f"set {test_key} {LARGE_VALUE}"
    print(f"📤 发送set大Value指令")
    client.send_message(cmd_set)
    resp = client.recv_message()
    print(f"📩 set响应: {repr(resp)}")

    cmd_get = f"get {test_key}"
    print("\n📤 发送get大Value指令")
    client.send_message(cmd_get)
    resp = client.recv_message()
    print(f"📩 get响应长度: {len(resp) if resp else 0}")

    new_big_value = "修改后的大Value内容：" + "XYZ" * 2000
    cmd_mod = f"mod {test_key} {new_big_value}"
    print(f"\n📤 发送mod大Value指令")
    client.send_message(cmd_mod)
    resp = client.recv_message()
    print(f"📩 mod响应: {repr(resp)}")

    client.send_message(f"del {test_key}")
    client.recv_message()

    print("\n" + "=" * 65)
    print("💾 大Value测试完成")
    print("=" * 65)

# ===================== 主逻辑 =====================
def main():
    client = TCPClient()
    try:
        while True:
            user_input = input("\n请输入指令：").strip().lower()
            
            if user_input == "exit":
                print("👋 退出程序...")
                break
            
            if not user_input:
                print("⚠️  指令不能为空，请重新输入")
                continue
            
            if user_input == "large":
                test_cmd = f"set testkey {LARGE_VALUE}"
                print(f"🚀 发送大value指令")
                if client.send_message(test_cmd):
                    resp = client.recv_message()
                    print(f"📩 服务器响应：{repr(resp)}")
                continue
            
            if user_input == "bigkey":
                test_big_key(client)
                continue
            
            if user_input == "bigvalue":
                test_big_value_mod_get(client)
                continue
            
            batch_ops = [
                "set","get","mod","exist","del",
                "rset","rget","rmod","rexist","rdel",
                "hset","hget","hmod","hexist","hdel",
                "skset","skget","skmod","skexist","skdel"
            ]
            
            if user_input in batch_ops:
                try:
                    mul = int(input(f"请输入{user_input}的倍数（如1、2、3）：").strip())
                    run_batch(client, user_input, mul)
                except ValueError:
                    print("❌ 倍数必须是整数！")
                continue
            
            print(f"📤 手动发送：{user_input}")
            if client.send_message(user_input):
                resp = client.recv_message()
                print(f"📩 服务器响应：{repr(resp)}")

    except KeyboardInterrupt:
        print("\n\n⚠️  程序被手动中断")
    finally:
        client.close()

if __name__ == "__main__":
    main()