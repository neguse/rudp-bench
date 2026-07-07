// 決定的パケットロス注入(再生計画 D2)の TCX egress プログラム。
// パケット連番で drop trace(ビットマップ)を引き、立っていれば drop する。
//
// cilium/ebpf の asm ビルダーは v0.22.0 時点で atomic fetch-add の
// BPF_FETCH ビットを落とす(imm=0 になる)ため、C + clang でコンパイルして
// go:embed で同梱する。再生成は losstrace.go の go:generate を実行
// (要 clang + libbpf ヘッダ)。
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

char _license[] SEC("license") = "MIT";

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u64);
} lt_state SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1); /* 実サイズはロード時に差し替える */
	__type(key, __u32);
	__type(value, __u64);
} lt_trace SEC(".maps");

/* trace 長 - 1(2の冪 - 1)。ロード時に RewriteConstants で書き換える */
volatile const __u64 lt_mask = 0;

SEC("tc")
int losstrace_egress(struct __sk_buff *skb)
{
	__u32 zero = 0;
	__u64 *cnt = bpf_map_lookup_elem(&lt_state, &zero);
	if (!cnt)
		return 0; /* TC_ACT_OK */
	__u64 idx = __sync_fetch_and_add(cnt, 1) & lt_mask;
	__u32 word = idx >> 6;
	__u64 *w = bpf_map_lookup_elem(&lt_trace, &word);
	if (!w)
		return 0;
	return ((*w >> (idx & 63)) & 1) ? 2 /* TC_ACT_SHOT */ : 0;
}
