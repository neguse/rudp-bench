// Package losstrace は決定的なパケットロス注入を提供する。
//
// netem の loss は毎 run 独立の乱数で、短い計測窓では p99 が乱数の引きに
// 支配される(再生計画 D2)。本パッケージは seed から事前生成した
// drop パターン(Gilbert-Elliott 連鎖のビット列)を eBPF(TCX egress)で
// パケット連番ベースに再生し、同一 seed なら全 run・全 transport に
// 同じ落ち方を適用する。遅延・帯域は従来どおり netem が担う(loss だけを
// eBPF に移す。TCX egress フックは qdisc より前段なので合成順も netem loss と
// 等価)。
//
// attach は対象 netns 内で実行すること(ifindex とネットリンクが netns
// スコープのため)。orchestrator は `ip netns exec <ns> orchestrator losstrace
// attach ...` の形で呼ぶ。TCX link は bpffs に pin して attach プロセスの
// 終了後も維持する。netns 削除で veth が消えれば link も外れるが、pin の
// ファイルは残るので teardown で pin ディレクトリを消すこと。
package losstrace

import (
	"bytes"
	_ "embed"
	"errors"
	"fmt"
	bits2 "math/bits"
	"math/rand"
	"net"
	"os"
	"path/filepath"

	"github.com/cilium/ebpf"
	"github.com/cilium/ebpf/link"
)

//go:generate clang -O2 -g -target bpf -c bpf/losstrace.c -o bpf/losstrace.o

// コンパイル済み BPF オブジェクト(bpf/losstrace.c 参照)。
// cilium/ebpf の asm ビルダーは v0.22.0 時点で atomic fetch-add の
// BPF_FETCH ビットを落とすため(TestAtomicProbe で実測)、命令列ではなく
// clang でコンパイルした ELF を同梱する。
//
//go:embed bpf/losstrace.o
var bpfObject []byte

// DefaultBits は trace 長(パケット数)の既定値。2 の冪であること。
// 1M パケットで一巡 ≈ 60s run の br 級トラフィックでも重複再生しない規模。
const DefaultBits = 1 << 20

// Generate は Gilbert-Elliott 連鎖(平均 loss 率 lossPct%、平均バースト長
// burstLen)を seed から決定的に生成し、64bit ワード列のビットマップと
// 実現 loss 率(%)を返す。bits は 2 の冪。burstLen < 1 は独立ロス。
func Generate(seed uint64, lossPct, burstLen float64, bits int) ([]uint64, float64, error) {
	if bits <= 0 || bits&(bits-1) != 0 {
		return nil, 0, fmt.Errorf("trace bits must be a positive power of two, got %d", bits)
	}
	if lossPct <= 0 || lossPct >= 100 {
		return nil, 0, fmt.Errorf("loss_pct must be in (0, 100), got %g", lossPct)
	}
	loss := lossPct / 100
	if burstLen < 1 {
		burstLen = 1
	}
	// netem gemodel と同じ逆算(netops.gemodelParams と同式):
	// r = 1/burst、p = loss*r/(1-loss)
	r := 1.0 / burstLen
	p := loss * r / (1.0 - loss)
	if p >= 1 {
		return nil, 0, fmt.Errorf("loss_pct %g with burst_len %g is infeasible (p >= 1)", lossPct, burstLen)
	}

	rng := rand.New(rand.NewSource(int64(seed)))
	words := make([]uint64, bits/64)
	bad := false
	dropped := 0
	for i := 0; i < bits; i++ {
		if bad {
			if rng.Float64() < r {
				bad = false
			}
		} else {
			if rng.Float64() < p {
				bad = true
			}
		}
		if bad {
			words[i/64] |= 1 << (uint(i) % 64)
			dropped++
		}
	}
	return words, float64(dropped) / float64(bits) * 100, nil
}

// pinPath は pin ディレクトリ内の TCX link の置き場。
func pinPath(pinDir, dev string) string {
	return filepath.Join(pinDir, "tcx-egress-"+dev)
}

// statePinPath はパケットカウンタ map の pin 置き場(リセット用)。
func statePinPath(pinDir, dev string) string {
	return filepath.Join(pinDir, "state-"+dev)
}

// Attach は現在の netns 内で dev の egress に drop trace を取り付ける。
// pinDir(bpffs 上)に link を pin する。
func Attach(dev string, words []uint64, pinDir string) error {
	iface, err := net.InterfaceByName(dev)
	if err != nil {
		return fmt.Errorf("interface %s: %w", dev, err)
	}
	if len(words) == 0 || len(words)&(len(words)-1) != 0 {
		return fmt.Errorf("trace words must be a positive power of two, got %d", len(words))
	}

	prog, maps, err := loadProgram(len(words))
	if err != nil {
		return err
	}
	defer prog.Close()
	defer maps.state.Close()
	defer maps.trace.Close()
	for i, w := range words {
		if err := maps.trace.Put(uint32(i), w); err != nil {
			return fmt.Errorf("populate trace map [%d]: %w", i, err)
		}
	}

	l, err := link.AttachTCX(link.TCXOptions{
		Program:   prog,
		Attach:    ebpf.AttachTCXEgress,
		Interface: iface.Index,
	})
	if err != nil {
		return fmt.Errorf("attach tcx egress on %s: %w", dev, err)
	}
	if err := os.MkdirAll(pinDir, 0o755); err != nil {
		_ = l.Close()
		return fmt.Errorf("mkdir pin dir: %w", err)
	}
	if err := l.Pin(pinPath(pinDir, dev)); err != nil {
		_ = l.Close()
		return fmt.Errorf("pin tcx link: %w", err)
	}
	// カウンタ map も pin する(gate トラフィック等が消費した trace 位置を
	// ResetCounter で 0 に戻し、計測トラフィックの開始位置を run 間で
	// 揃えるため)
	if err := maps.state.Pin(statePinPath(pinDir, dev)); err != nil {
		_ = l.Close()
		return fmt.Errorf("pin state map: %w", err)
	}
	return l.Close() // pin が参照を保持する
}

// ResetCounter はパケットカウンタを 0 に戻す。netem gate(ping/iperf3)や
// 接続前の雑多なトラフィックが消費した trace 位置をリセットし、計測本体が
// 毎 run 同じ位置から trace を再生するようにする。
func ResetCounter(dev string, pinDir string) error {
	m, err := ebpf.LoadPinnedMap(statePinPath(pinDir, dev), nil)
	if err != nil {
		return fmt.Errorf("load pinned state map: %w", err)
	}
	defer m.Close()
	if err := m.Put(uint32(0), uint64(0)); err != nil {
		return fmt.Errorf("reset counter: %w", err)
	}
	return nil
}

// ReadCounter は egress プログラムが観測したパケット数(attach または
// ResetCounter 以降)を pinned state map から読む。bpffs の pin は netns
// スコープでないため、どの netns からでも読める。
func ReadCounter(dev string, pinDir string) (uint64, error) {
	m, err := ebpf.LoadPinnedMap(statePinPath(pinDir, dev), nil)
	if err != nil {
		return 0, fmt.Errorf("load pinned state map: %w", err)
	}
	defer m.Close()
	var value uint64
	if err := m.Lookup(uint32(0), &value); err != nil {
		return 0, fmt.Errorf("read counter: %w", err)
	}
	return value, nil
}

// CountDropsInRange はパケット連番の半開区間 [from, to) のうち trace が
// drop する数を返す。連番は trace 長で巡回する(eBPF 側の idx & mask と同じ)。
func CountDropsInRange(words []uint64, from, to uint64) (uint64, error) {
	bits := uint64(len(words)) * 64
	if bits == 0 || bits&(bits-1) != 0 {
		return 0, fmt.Errorf("trace length must be a positive power of two, got %d bits", bits)
	}
	if to < from {
		return 0, fmt.Errorf("invalid packet range [%d, %d)", from, to)
	}
	span := to - from
	var total uint64
	for _, word := range words {
		total += uint64(bits2.OnesCount64(word))
	}
	drops := (span / bits) * total
	start := from & (bits - 1)
	for counted := uint64(0); counted < span%bits; {
		idx := (start + counted) & (bits - 1)
		word := words[idx>>6]
		offset := idx & 63
		chunk := uint64(64 - offset)
		if remaining := span%bits - counted; chunk > remaining {
			chunk = remaining
		}
		mask := ^uint64(0)
		if chunk < 64 {
			mask = (uint64(1)<<chunk - 1)
		}
		drops += uint64(bits2.OnesCount64((word >> offset) & mask))
		counted += chunk
	}
	return drops, nil
}

type programMaps struct {
	state *ebpf.Map
	trace *ebpf.Map
}

// loadProgram は同梱 ELF から trace 長を差し替えてプログラムをロードする。
func loadProgram(words int) (*ebpf.Program, programMaps, error) {
	spec, err := ebpf.LoadCollectionSpecFromReader(bytes.NewReader(bpfObject))
	if err != nil {
		return nil, programMaps{}, fmt.Errorf("parse bpf object: %w", err)
	}
	traceSpec, ok := spec.Maps["lt_trace"]
	if !ok {
		return nil, programMaps{}, fmt.Errorf("bpf object has no lt_trace map")
	}
	traceSpec.MaxEntries = uint32(words)
	if err := spec.Variables["lt_mask"].Set(uint64(words*64 - 1)); err != nil {
		return nil, programMaps{}, fmt.Errorf("set lt_mask: %w", err)
	}
	coll, err := ebpf.NewCollection(spec)
	if err != nil {
		return nil, programMaps{}, fmt.Errorf("load bpf collection: %w", err)
	}
	prog := coll.Programs["losstrace_egress"]
	state := coll.Maps["lt_state"]
	trace := coll.Maps["lt_trace"]
	if prog == nil || state == nil || trace == nil {
		coll.Close()
		return nil, programMaps{}, fmt.Errorf("bpf object missing program or maps")
	}
	// coll.Close() は個々の参照も閉じるため、必要な参照だけ取り出して
	// coll からは切り離す
	delete(coll.Programs, "losstrace_egress")
	delete(coll.Maps, "lt_state")
	delete(coll.Maps, "lt_trace")
	coll.Close()
	return prog, programMaps{state: state, trace: trace}, nil
}

// Detach は pin された link を外す。link が既に無くても(netns 削除済み等)
// pin の掃除だけ行う。
func Detach(dev string, pinDir string) error {
	_ = os.Remove(statePinPath(pinDir, dev))
	path := pinPath(pinDir, dev)
	l, err := link.LoadPinnedLink(path, nil)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil
		}
		// link 実体が死んでいる場合は pin ファイルだけ消す
		_ = os.Remove(path)
		return nil
	}
	err = l.Unpin()
	_ = l.Close()
	return err
}

