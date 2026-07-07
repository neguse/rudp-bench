package losstrace

import (
	"os"
	"testing"

	"github.com/cilium/ebpf"
	"github.com/cilium/ebpf/asm"
)

// cilium/ebpf v0.22.0 の asm ビルダーは FetchAdd の BPF_FETCH ビットを
// marshal 時に落とす(imm=0 の素の XADD になり、src レジスタが更新されない)。
// これが直っていれば命令列ビルドに戻れるので、挙動を観測して記録する
// (失敗にはしない)。losstrace が C + clang 同梱方式なのはこのため。
func TestCiliumAsmFetchAddBug(t *testing.T) {
	if os.Geteuid() != 0 {
		t.Skip("requires root")
	}
	state, err := ebpf.NewMap(&ebpf.MapSpec{Type: ebpf.Array, KeySize: 4, ValueSize: 8, MaxEntries: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer state.Close()
	prog, err := ebpf.NewProgram(&ebpf.ProgramSpec{
		Type: ebpf.SchedCLS, License: "MIT",
		Instructions: asm.Instructions{
			asm.Mov.Imm(asm.R2, 0),
			asm.StoreMem(asm.RFP, -4, asm.R2, asm.Word),
			asm.Mov.Reg(asm.R2, asm.RFP),
			asm.Add.Imm(asm.R2, -4),
			asm.LoadMapPtr(asm.R1, state.FD()),
			asm.FnMapLookupElem.Call(),
			asm.JEq.Imm(asm.R0, 0, "out"),
			asm.Mov.Imm(asm.R1, 1),
			asm.FetchAdd.Mem(asm.R0, asm.R1, asm.DWord, 0),
			asm.Mov.Reg(asm.R0, asm.R1),
			asm.Return(),
			asm.Mov.Imm(asm.R0, 99).WithSymbol("out"),
			asm.Return(),
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	defer prog.Close()
	pkt := make([]byte, 64)
	fetchWorks := true
	for i := 0; i < 3; i++ {
		ret, _, err := prog.Test(pkt)
		if err != nil {
			t.Fatal(err)
		}
		if ret != uint32(i) { // 正しい fetch_add なら旧値 0,1,2
			fetchWorks = false
		}
	}
	if fetchWorks {
		t.Log("cilium asm FetchAdd が旧値を返すようになった — C 同梱をやめて命令列ビルドに戻せる可能性あり")
	} else {
		t.Log("cilium asm FetchAdd は依然 BPF_FETCH を落とす(C 同梱方式を維持)")
	}
}
