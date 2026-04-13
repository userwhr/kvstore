import socket
import struct
import sys
import time
from typing import Optional

# 服务器配置（固定不变）
SERVER_IP = "192.168.140.134"
SERVER_PORT = 4000
BUFFER_SIZE = 4096
TEST_COUNT = 10000  # 固定10000条 key1~key10000

# 预置大字符串（Value）
LARGE_VALUE = "测试大Value内容：" + "abcdefghijklmnopqrstuvwxyz1234567890" * 100

# 预置大Key（超长Key名称，用于rset等操作）
BIG_KEY = "bigkey_" + "a" * 5000   # 超长Key，长度约5000+
BIG_KEY_VALUE = "value_for_big_key"  # 关联的普通Value

class TCPClient:
    """【完全保留原版代码，协议100%不变】4字节小端头+指令\r\n"""
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
            print("   - 其他输入为手动发送指令")
        except Exception as e:
            print(f"❌ 连接失败: {e}")
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
            print("❌ 错误：未建立连接")
            return False
        try:
            packed_msg = self._pack_message(cmd)
            self.sock.sendall(packed_msg)
            return True
        except Exception as e:
            print(f"❌ 发送失败: {e}")
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
                print("❌ 连接被服务器关闭")
                return None
            return response.decode("utf-8").strip()
        except Exception as e:
            print(f"❌ 接收失败: {e}")
            return None

    def close(self):
        """关闭连接"""
        if self.sock:
            self.sock.close()
            self.sock = None
            print("\n🔌 连接已关闭")

# ===================== 批量测试核心函数（扩展r/h/sk前缀） =====================
def run_batch(client: TCPClient, op: str, mul: int):
    success = 0
    fail = 0
    start_time = time.time()
    print(f"\n🚀 启动【{op}】批量测试：key1~key{TEST_COUNT} | 倍数={mul}")
    print("="*65)

    # 解析前缀和基础操作（核心：自动拆分r/h/sk前缀）
    prefix = ""
    if op.startswith("sk"):  # sk是双字符前缀，优先判断
        prefix = "sk"
        base_op = op[2:]
    elif op.startswith(("r", "h")):  # r/h是单字符前缀
        prefix = op[0]
        base_op = op[1:]
    else:  # 无前缀（原指令）
        prefix = ""
        base_op = op

    # 校验基础操作是否合法
    valid_base_ops = ["set", "get", "mod", "exist", "del"]
    if base_op not in valid_base_ops:
        print(f"❌ 不支持的基础操作：{base_op}")
        return

    for i in range(1, TEST_COUNT+1):
        val = i * mul
        # 构造带前缀的指令 + 预期响应（和原指令完全一致）
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

        # 发送+接收+校验（原逻辑不变）
        if not client.send_message(cmd, is_batch=True):
            fail +=1
            continue
        resp = client.recv_message(is_batch=True)
        if resp == expect:
            success +=1
        else:
            fail +=1
            print(f"❌ 第{i}条失败：{cmd} | 预期：{expect} | 实际：{resp}")

        # 进度打印（每1000条输出）
        if i % 1000 == 0:
            print(f"📊 进度：{i}/{TEST_COUNT} | 成功：{success} | 失败：{fail}")

    # 统计结果（原逻辑不变）
    total_time = time.time() - start_time
    print("\n" + "="*65)
    print("📊 测试完成")
    print(f"操作：{op} | 倍数：{mul} | 总数：{TEST_COUNT}")
    print(f"✅ 成功：{success} | ❌ 失败：{fail}")
    print(f"⏱️ 总耗时：{total_time:.2f}s | 🚀 速率：{TEST_COUNT/total_time:.2f}条/秒")
    print("="*65)

# ===================== 新增：大Key完整测试 =====================
def test_big_key(client: TCPClient):
    """测试超长Key的 rset / rget / rmod / rexist / rdel 操作"""
    print("\n🔑 开始大Key完整测试（超长Key名称）")
    print("=" * 65)

    # 1. rset 设置大Key
    cmd_set = f"rset {BIG_KEY} {BIG_KEY_VALUE}"
    print(f"📤 发送: {cmd_set[:100]}... (总长度: {len(cmd_set)})")
    if not client.send_message(cmd_set):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rset响应: {resp}")
    if resp != "ok":
        print("❌ 大Key rset失败，终止测试")
        return

    # 2. rget 读取大Key
    cmd_get = f"rget {BIG_KEY}"
    print(f"\n📤 发送: {cmd_get[:100]}...")
    if not client.send_message(cmd_get):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rget响应: {resp}")
    if resp != BIG_KEY_VALUE:
        print(f"❌ 大Key rget失败，预期值: {BIG_KEY_VALUE}")

    # 3. rmod 修改大Key的值
    new_value = "modified_value_for_big_key"
    cmd_mod = f"rmod {BIG_KEY} {new_value}"
    print(f"\n📤 发送: {cmd_mod[:100]}...")
    if not client.send_message(cmd_mod):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rmod响应: {resp}")
    if resp != "ok":
        print("❌ 大Key rmod失败")
    else:
        # 验证修改结果
        client.send_message(cmd_get)
        resp = client.recv_message()
        print(f"📩 修改后rget响应: {resp}")
        if resp == new_value:
            print("✅ 大Key rmod验证通过")

    # 4. rexist 检查存在性
    cmd_exist = f"rexist {BIG_KEY}"
    print(f"\n📤 发送: {cmd_exist[:100]}...")
    if not client.send_message(cmd_exist):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rexist响应: {resp}")
    if resp != "exist":
        print("❌ 大Key rexist失败")

    # 5. rdel 删除大Key
    cmd_del = f"rdel {BIG_KEY}"
    print(f"\n📤 发送: {cmd_del[:100]}...")
    if not client.send_message(cmd_del):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 rdel响应: {resp}")
    if resp != "success":
        print("❌ 大Key rdel失败")
    else:
        # 验证删除
        client.send_message(cmd_exist)
        resp = client.recv_message()
        print(f"📩 删除后rexist响应: {resp}")
        if resp == "not exist":
            print("✅ 大Key rdel验证通过")

    print("\n" + "=" * 65)
    print("🔑 大Key测试完成")
    print("=" * 65)

# ===================== 新增：大Value的mod/get测试 =====================
def test_big_value_mod_get(client: TCPClient):
    """测试大Value的mod和get操作（先set一个超大Value，再mod和get）"""
    print("\n💾 开始大Value的mod/get测试")
    print("=" * 65)

    test_key = "bigvalue_key"
    # 1. set 一个大Value
    cmd_set = f"set {test_key} {LARGE_VALUE}"
    print(f"📤 发送set大Value指令 (value长度: {len(LARGE_VALUE)} 字符)")
    if not client.send_message(cmd_set):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 set响应: {resp}")
    if resp != "ok":
        print("❌ set大Value失败，终止测试")
        return

    # 2. get 获取大Value
    cmd_get = f"get {test_key}"
    print("\n📤 发送get大Value指令")
    if not client.send_message(cmd_get):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 get响应长度: {len(resp) if resp else 0} 字符")
    if resp == LARGE_VALUE:
        print("✅ get大Value正确")
    else:
        print("❌ get大Value内容不匹配")

    # 3. mod 修改大Value（改为另一个大字符串）
    new_big_value = "修改后的大Value内容：" + "XYZ" * 2000
    cmd_mod = f"mod {test_key} {new_big_value}"
    print(f"\n📤 发送mod大Value指令 (新value长度: {len(new_big_value)})")
    if not client.send_message(cmd_mod):
        print("❌ 发送失败")
        return
    resp = client.recv_message()
    print(f"📩 mod响应: {resp}")
    if resp != "ok":
        print("❌ mod大Value失败")
    else:
        # 验证mod结果
        client.send_message(cmd_get)
        resp = client.recv_message()
        print(f"📩 mod后get响应长度: {len(resp) if resp else 0}")
        if resp == new_big_value:
            print("✅ mod大Value验证通过")
        else:
            print("❌ mod后内容不匹配")

    # 清理：删除测试key
    client.send_message(f"del {test_key}")
    client.recv_message()

    print("\n" + "=" * 65)
    print("💾 大Value的mod/get测试完成")
    print("=" * 65)

# ===================== 主交互逻辑（新增bigkey/bigvalue指令） =====================
def main():
    client = TCPClient()
    try:
        while True:
            user_input = input("\n请输入指令：").strip().lower()
            
            # 退出
            if user_input == "exit":
                print("👋 退出程序...")
                break
            
            # 空指令
            if not user_input:
                print("⚠️  指令不能为空，请重新输入")
                continue
            
            # 发送大value（原有功能）
            if user_input == "large":
                test_cmd = f"set testkey {LARGE_VALUE}"
                print(f"🚀 发送大value指令")
                if client.send_message(test_cmd):
                    resp = client.recv_message()
                    print(f"📩 服务器响应：{resp}")
                continue
            
            # 大Key完整测试（新增）
            if user_input == "bigkey":
                test_big_key(client)
                continue
            
            # 大Value的mod/get测试（新增）
            if user_input == "bigvalue":
                test_big_value_mod_get(client)
                continue
            
            # 支持的所有批量指令（原+新增r/h/sk）
            batch_ops = [
                "set","get","mod","exist","del",
                "rset","rget","rmod","rexist","rdel",
                "hset","hget","hmod","hexist","hdel",
                "skset","skget","skmod","skexist","skdel"
            ]
            
            # 批量操作处理
            if user_input in batch_ops:
                try:
                    mul = int(input(f"请输入{user_input}的倍数（如1、2、3）：").strip())
                    run_batch(client, user_input, mul)
                except ValueError:
                    print("❌ 倍数必须是整数！")
                continue
            
            # 手动发送任意指令
            print(f"📤 手动发送：{user_input}")
            if client.send_message(user_input):
                resp = client.recv_message()
                print(f"📩 服务器响应：{resp}")

    except KeyboardInterrupt:
        print("\n\n⚠️  程序被手动中断")
    finally:
        client.close()

if __name__ == "__main__":
    main()