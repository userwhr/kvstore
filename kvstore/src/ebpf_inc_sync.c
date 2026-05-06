/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>


#define MASTER_PORT 2000

// 手动定义TCP标志位，确保eBPF可用
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PUSH 0x08
#define TH_ACK 0x10

/* ===================== eBPF Maps ===================== */
struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, __u64);
} cmd_count SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
  __uint(key_size, sizeof(int));
  __uint(value_size, sizeof(int));
} cmd_events SEC(".maps");

/* ===================== 写命令识别 ===================== */
static __always_inline int is_write_command(const char *cmd, int len,
                                            const char *data_end) {
  struct {
    const char *name;
    int len;
  } write_cmds[] = {
      {"set", 3},  {"del", 3},   {"mod", 3},   {"rset", 4},
      {"rdel", 4}, {"rmod", 4},  {"hset", 4},  {"hdel", 4},
      {"hmod", 4}, {"skset", 5}, {"skdel", 5}, {"skmod", 5},
  };

  for (int i = 0; i < 12; i++) {
    if (len != write_cmds[i].len)
      continue;

    int match = 1;
    int j;
    if (j >= len || cmd + 0 >= data_end)
      match = 0;
    if (match) {
      char c1 = cmd[0];
      char c2 = write_cmds[i].name[0];
      if (c1 >= 'A' && c1 <= 'Z')
        c1 += 32;
      if (c1 != c2)
        match = 0;
    }

    if (len >= 2 && match) {
      if (cmd + 1 >= data_end)
        match = 0;
      else {
        char c1 = cmd[1];
        char c2 = write_cmds[i].name[1];
        if (c1 >= 'A' && c1 <= 'Z')
          c1 += 32;
        if (c1 != c2)
          match = 0;
      }
    }
    if (len >= 3 && match) {
      if (cmd + 2 >= data_end)
        match = 0;
      else {
        char c1 = cmd[2];
        char c2 = write_cmds[i].name[2];
        if (c1 >= 'A' && c1 <= 'Z')
          c1 += 32;
        if (c1 != c2)
          match = 0;
      }
    }
    if (len >= 4 && match) {
      if (cmd + 3 >= data_end)
        match = 0;
      else {
        char c1 = cmd[3];
        char c2 = write_cmds[i].name[3];
        if (c1 >= 'A' && c1 <= 'Z')
          c1 += 32;
        if (c1 != c2)
          match = 0;
      }
    }
    if (len >= 5 && match) {
      if (cmd + 4 >= data_end)
        match = 0;
      else {
        char c1 = cmd[4];
        char c2 = write_cmds[i].name[4];
        if (c1 >= 'A' && c1 <= 'Z')
          c1 += 32;
        if (c1 != c2)
          match = 0;
      }
    }

    if (match)
      return 1;
  }
  return 0;
}

/* ===================== 解析 RESP ===================== */
static __always_inline int parse_resp_command(const char *data,
                                              const char *data_end,
                                              int *cmd_off, int *cmd_len) {
  const char *p = data;

  if (p >= data_end || *p != '*')
    return -1;
  p++;

  for (int i = 0; i < 5; i++) {
    if (p >= data_end)
      break;
    char c = *p;
    if (c >= '0' && c <= '9')
      p++;
  }

  if (p + 1 >= data_end || *p != '\r' || *(p + 1) != '\n')
    return -1;
  p += 2;

  if (p >= data_end || *p != '$')
    return -1;
  p++;

  int str_len = 0;
  for (int i = 0; i < 5; i++) {
    if (p >= data_end)
      break;
    char c = *p;
    if (c >= '0' && c <= '9') {
      str_len = str_len * 10 + (c - '0');
      p++;
    }
  }

  if (p + 1 >= data_end || *p != '\r' || *(p + 1) != '\n')
    return -1;
  p += 2;

  if (p + str_len > data_end)
    return -1;

  if (!is_write_command(p, str_len, data_end))
    return -1;

  *cmd_off = p - data;
  *cmd_len = str_len;
  return 0;
}

/* ===================== XDP 入口 ===================== */
SEC("xdp")
int ebpf_inc_sync_func(struct xdp_md *ctx) {
  void *data_end = (void *)(long)ctx->data_end;
  void *data = (void *)(long)ctx->data;

  struct ethhdr *eth = data;
  if ((void *)(eth + 1) > data_end)
    return XDP_PASS;

  if (bpf_ntohs(eth->h_proto) != ETH_P_IP)
    return XDP_PASS;

  struct iphdr *ip = data + sizeof(struct ethhdr);
  if ((void *)(ip + 1) > data_end)
    return XDP_PASS;

  if (ip->protocol != IPPROTO_TCP)
    return XDP_PASS;

  int ip_hdr_len = ip->ihl * 4;

  struct tcphdr *tcp = (void *)ip + ip_hdr_len;
  if ((void *)(tcp + 1) > data_end)
    return XDP_PASS;

  // ==============================================
  // 【过滤TCP握手/空包】
  // 读取 TCP 头第 13 字节（flags 字段）
  // ==============================================
  __u8 *tcp_flags_ptr = (__u8 *)tcp + 13;
  if ((void *)(tcp_flags_ptr + 1) > data_end)
    return XDP_PASS;
  __u8 tcp_flags = *tcp_flags_ptr;
  // 丢掉 SYN / FIN / RST 包
  if (tcp_flags & (TH_SYN | TH_FIN | TH_RST))
    return XDP_PASS;
  // 丢掉 没有PUSH标志的纯ACK空包（无应用数据）
  if (!(tcp_flags & TH_PUSH))
    return XDP_PASS;

  __u16 dst_port = bpf_ntohs(tcp->dest);
  if (dst_port != MASTER_PORT)
    return XDP_PASS;

  int tcp_hdr_len = tcp->doff * 4;
  /* 用 IP tot_len 算真实 payload，避免以太网填充干扰 */
  int ip_total_len = bpf_ntohs(ip->tot_len);
  int payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
  const char *payload = (const char *)tcp + tcp_hdr_len;
  const char *payload_end = (const char *)data_end; /* 用于 copy 循环的边界检查 */

  // bpf_printk("[eBPF] port2000 payload_len=%d\n", payload_len);

  if (payload_len <= 0)
    return XDP_PASS;

  /* 复制到栈上，通过 perf_event_output 发送到用户态 */
  __u32 key = 0;
  __u64 *count = bpf_map_lookup_elem(&cmd_count, &key);
  if (count)
    __sync_fetch_and_add(count, 1);

  char buf[128];
  __builtin_memset(buf, 0, sizeof(buf));
  unsigned int copy_len = (unsigned int)payload_len;
  copy_len &= 0x7F;

#pragma unroll
  for (unsigned int i = 0; i < 128; i++) {
    if (i >= copy_len)
      break;
    if (payload + i >= payload_end)
      break;
    buf[i] = payload[i];
  }

  bpf_perf_event_output(ctx, &cmd_events, BPF_F_CURRENT_CPU, (void *)buf,
                        copy_len);

  return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
