import socket
import struct
import sys
import time
from typing import Optional

# ===================== 【核心修改】双服务器配置（读写分离） =====================
# 写操作服务器：set/mod/del + r/h/sk前缀 全部走这里
WRITE_SERVER_IP = "192.168.140.135"
WRITE_SERVER_PORT = 2000
# 读操作服务器：get/exist + r/h/sk前缀 全部走这里
READ_SERVER_IP = "192.168.140.134"
READ_SERVER_PORT = 4000  # 请修改为你的真实读服务器端口

BUFFER_SIZE = 4096
TEST_COUNT = 1000  # 固定10000条 key1~key10000

# 预置大字符串（Value）
LARGE_VALUE = "测试大Value内容：" + "abcdefghijklmnopqrstuvwxyz1234567890" * 100

# 预置大Key（超长Key名称，用于rset等操作）
BIG_KEY = "bigkey_" + "a" * 5000   # 超长Key，长度约5000+
BIG_KEY_VALUE = "value_for_big_key"  # 关联的普通Value

# ===================== 【核心定义】读写命令分类 =====================
# 写命令集合（自动转发到写服务器）
WRITE_COMMANDS = {"set", "mod", "del",
                  "rset", "rmod", "rdel",
                  "hset", "hmod", "hdel",
                  "skset", "skmod", "skdel"}
# 读命令集合（自动转发到读服务器）
READ_COMMANDS = {"get", "exist",
                 "rget", "rexist",
                 "hget", "hexist",
                 "skget", "skexist"}

class TCPClient:
    """【完全保留原版代码，协议100%不变】4字节小端头+指令\r\n"""
    def __init__(self, server_ip: str, server_port: int, name: str):
        self.sock: Optional[socket.socket] = None
        self.server_ip = server_ip
        self.server_port = server_port
        self.name = name  # 标识：写客户端/读客户端
        self.connect()

    def connect(self):
        """建立TCP连接"""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sock.connect((self.server_ip, self.server_port))
            print(f"✅ 成功连接【{self.name}】: {self.server_ip}:{self.server_port}")
        except Exception as e:
            print(f"❌ 【{self.name}】连接失败: {e}")
            sys.exit(1)

    def _pack_message(self, cmd: str) -> bytes:
        """【核心协议不变】4字节小端长度头 + 指令\r\n"""
        cmd_with_rn = f"{cmd}\r\n"
        cmd_len = len(cmd_with_rn.encode("utf-8"))
        len_prefix = struct.pack("<I", cmd_len)
        packed_msg = len_prefix + cmd_with_rn.encode("utf-8")
        return packed_msg

    def send_message(self, cmd: str, is_batch: bool = False) -> bool:
        """发送指令（批量模式关闭冗余日志）"""
        if not self.sock:
            print(f"❌ 错误：{self.name}未建立连接")
            return False
        try:
            packed_msg = self._pack_message(cmd)
            self.sock.sendall(packed_msg)
            return True
        except Exception as e:
            print(f"❌ {self.name}发送失败: {e}")
            return False

    def recv_message(self, is_batch: bool = False) -> Optional[str]:
        """接收响应（批量模式关闭冗余日志）"""
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
                print(f"❌ {self.name}连接被服务器关闭")
                return None
            return response.decode("utf-8").strip()
        except Exception as e:
            print(f"❌ {self.name}接收失败: {e}")
            return None

    def close(self):
        """关闭连接"""
        if self.sock:
            self.sock.close()
            self.sock = None
            print(f"\n🔌 {self.name}连接已关闭")

# ===================== 批量测试核心函数（自动读写分离转发） =====================
def run_batch(write_client: TCPClient, read_client: TCPClient, op: str, mul: int):
    success = 0
    fail = 0
    start_time = time.time()
    print(f"\n🚀 启动【{op}】批量测试：key1~key{TEST_COUNT} | 倍数={mul}")
    print("="*65)

    # 解析前缀和基础操作
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

    # 校验基础操作
    valid_base_ops = ["set", "get", "mod", "exist", "del"]
    if base_op not in valid_base_ops:
        print(f"❌ 不支持的基础操作：{base_op}")
        return

    for i in range(1, TEST_COUNT+1):
        val = i * mul
        # 构造指令
        if base_op == "set":
            cmd = f"{prefix}set key{i} {val}"
            expect = "ok"
        elif base_op == "get":
            cmd = f"{prefix}get key{i}"
            expect = str(val)
        elif base_op == "mod":
            cmd = f"{prefix}mod key{i} {val}"
            expect = "ok"
        elif base_op == "exist":
            cmd = f"{prefix}exist key{i}"
            expect = "exist"
        elif base_op == "del":
            cmd = f"{prefix}del key{i}"
            expect = "success"
        else:
            fail +=1
            continue

        # ===================== 【自动读写分离】核心逻辑 =====================
        if op in WRITE_COMMANDS:
            client = write_client  # 写命令 → 写服务器
        elif op in READ_COMMANDS:
            client = read_client   # 读命令 → 读服务器
        else:
            fail +=1
            continue

        # 发送+接收+校验
        if not client.send_message(cmd, is_batch=True):
            fail +=1
            continue
        resp = client.recv_message(is_batch=True)
        if resp == expect:
            success +=1
        else:
            fail +=1
            print(f"❌ 第{i}条失败：{cmd} | 预期：{expect} | 实际：{resp}")

        # 进度打印
        if i % 1000 == 0:
            print(f"📊 进度：{i}/{TEST_COUNT} | 成功：{success} | 失败：{fail}")

    # 统计结果
    total_time = time.time() - start_time
    print("\n" + "="*65)
    print("📊 测试完成")
    print(f"操作：{op} | 倍数：{mul} | 总数：{TEST_COUNT}")
    print(f"✅ 成功：{success} | ❌ 失败：{fail}")
    print(f"⏱️ 总耗时：{total_time:.2f}s | 🚀 速率：{TEST_COUNT/total_time:.2f}条/秒")
    print("="*65)

# ===================== 大Key测试（自动读写分离） =====================
def test_big_key(write_client: TCPClient, read_client: TCPClient):
    print("\n🔑 开始大Key完整测试（超长Key名称）")
    print("=" * 65)

    # 1. rset 写操作 → 写服务器
    cmd_set = f"rset {BIG_KEY} {BIG_KEY_VALUE}"
    print(f"📤 发送: {cmd_set[:100]}... (总长度: {len(cmd_set)})")
    if write_client.send_message(cmd_set):
        resp = write_client.recv_message()
        print(f"📩 rset响应: {resp}")
        if resp != "ok":
            print("❌ 大Key rset失败，终止测试")
            return

    # 2. rget 读操作 → 读服务器
    cmd_get = f"rget {BIG_KEY}"
    print(f"\n📤 发送: {cmd_get[:100]}...")
    if read_client.send_message(cmd_get):
        resp = read_client.recv_message()
        print(f"📩 rget响应: {resp}")

    # 3. rmod 写操作 → 写服务器
    new_value = "modified_value_for_big_key"
    cmd_mod = f"rmod {BIG_KEY} {new_value}"
    print(f"\n📤 发送: {cmd_mod[:100]}...")
    if write_client.send_message(cmd_mod):
        resp = write_client.recv_message()
        print(f"📩 rmod响应: {resp}")
        # 验证：读操作 → 读服务器
        read_client.send_message(cmd_get)
        resp = read_client.recv_message()
        print(f"📩 修改后rget响应: {resp}")

    # 4. rexist 读操作 → 读服务器
    cmd_exist = f"rexist {BIG_KEY}"
    print(f"\n📤 发送: {cmd_exist[:100]}...")
    if read_client.send_message(cmd_exist):
        resp = read_client.recv_message()
        print(f"📩 rexist响应: {resp}")

    # 5. rdel 写操作 → 写服务器
    cmd_del = f"rdel {BIG_KEY}"
    print(f"\n📤 发送: {cmd_del[:100]}...")
    if write_client.send_message(cmd_del):
        resp = write_client.recv_message()
        print(f"📩 rdel响应: {resp}")
        # 验证：读操作 → 读服务器
        read_client.send_message(cmd_exist)
        resp = read_client.recv_message()
        print(f"📩 删除后rexist响应: {resp}")

    print("\n" + "=" * 65)
    print("🔑 大Key测试完成")
    print("=" * 65)

# ===================== 大Value测试（自动读写分离） =====================
def test_big_value_mod_get(write_client: TCPClient, read_client: TCPClient):
    print("\n💾 开始大Value的mod/get测试")
    print("=" * 65)

    test_key = "bigvalue_key"
    # 1. set 写操作 → 写服务器
    cmd_set = f"set {test_key} {LARGE_VALUE}"
    print(f"📤 发送set大Value指令")
    if write_client.send_message(cmd_set):
        resp = write_client.recv_message()
        print(f"📩 set响应: {resp}")

    # 2. get 读操作 → 读服务器
    cmd_get = f"get {test_key}"
    print("\n📤 发送get大Value指令")
    if read_client.send_message(cmd_get):
        resp = read_client.recv_message()
        print(f"📩 get响应长度: {len(resp) if resp else 0} 字符")

    # 3. mod 写操作 → 写服务器
    new_big_value = "修改后的大Value内容：" + "XYZ" * 2000
    cmd_mod = f"mod {test_key} {new_big_value}"
    print(f"\n📤 发送mod大Value指令")
    if write_client.send_message(cmd_mod):
        resp = write_client.recv_message()
        print(f"📩 mod响应: {resp}")
        # 验证：读操作 → 读服务器
        read_client.send_message(cmd_get)
        resp = read_client.recv_message()
        print(f"📩 mod后get响应长度: {len(resp) if resp else 0}")

    # 清理 del 写操作 → 写服务器
    write_client.send_message(f"del {test_key}")
    write_client.recv_message()

    print("\n" + "=" * 65)
    print("💾 大Value的mod/get测试完成")
    print("=" * 65)

# ===================== 主交互逻辑（全自动读写转发） =====================
def main():
    # ===================== 【创建双连接】写客户端 + 读客户端 =====================
    write_client = TCPClient(WRITE_SERVER_IP, WRITE_SERVER_PORT, "写操作服务器")
    read_client = TCPClient(READ_SERVER_IP, READ_SERVER_PORT, "读操作服务器")
    
    print("\n📌 指令说明（全自动读写分离转发）：")
    print("   ✍️  写命令(set/mod/del+前缀) → 自动发送到写服务器")
    print("   👓 读命令(get/exist+前缀) → 自动发送到读服务器")
    print("   - 输入 'exit' 退出")
    print("   - 输入 'bigkey' / 'bigvalue' 测试大Key/大Value")
    print("   - 直接输入命令：自动转发对应服务器")

    try:
        while True:
            user_input = input("\n请输入指令：").strip().lower()
            
            if user_input == "exit":
                print("👋 退出程序...")
                break
            
            if not user_input:
                print("⚠️  指令不能为空，请重新输入")
                continue
            
            # 大Key测试（内部自动读写分离）
            if user_input == "bigkey":
                test_big_key(write_client, read_client)
                continue
            
            # 大Value测试（内部自动读写分离）
            if user_input == "bigvalue":
                test_big_value_mod_get(write_client, read_client)
                continue
            
            # 批量操作
            batch_ops = list(WRITE_COMMANDS) + list(READ_COMMANDS)
            if user_input in batch_ops:
                try:
                    mul = int(input(f"请输入{user_input}的倍数（如1、2、3）：").strip())
                    run_batch(write_client, read_client, user_input, mul)
                except ValueError:
                    print("❌ 倍数必须是整数！")
                continue
            
            # ===================== 【手动指令：自动读写转发】 =====================
            cmd_base = user_input.split()[0] if user_input else ""
            print(f"📤 手动发送：{user_input}")
            if cmd_base in WRITE_COMMANDS:
                # 写命令 → 写服务器
                if write_client.send_message(user_input):
                    resp = write_client.recv_message()
                    print(f"📩 写服务器响应：{resp}")
            elif cmd_base in READ_COMMANDS:
                # 读命令 → 读服务器
                if read_client.send_message(user_input):
                    resp = read_client.recv_message()
                    print(f"📩 读服务器响应：{resp}")
            else:
                print(f"⚠️  未知命令，默认发送到写服务器")
                if write_client.send_message(user_input):
                    resp = write_client.recv_message()
                    print(f"📩 写服务器响应：{resp}")

    except KeyboardInterrupt:
        print("\n\n⚠️  程序被手动中断")
    finally:
        # 关闭双连接
        write_client.close()
        read_client.close()

if __name__ == "__main__":
    main()