package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/neguse/rudp-bench/orchestrator/netops/losstrace"
)

// losstrace attach/detach。決定的 loss 注入(再生計画 D2)の netns 内
// 実行部で、netops の setup/teardown から
// `ip netns exec <ns> orchestrator losstrace ...` の形で呼ばれる。
func losstraceMain(args []string) {
	if len(args) < 1 || (args[0] != "attach" && args[0] != "detach") {
		fmt.Fprintln(os.Stderr, "usage: orchestrator losstrace attach|detach [flags]")
		os.Exit(1)
	}
	switch args[0] {
	case "attach":
		fs := flag.NewFlagSet("losstrace attach", flag.ExitOnError)
		dev := fs.String("dev", "", "veth device name (current netns)")
		seed := fs.Uint64("seed", 0, "trace seed (required, != 0)")
		lossPct := fs.Float64("loss-pct", 0, "average loss percent")
		burst := fs.Float64("burst", 1, "average burst length (packets)")
		bits := fs.Int("bits", losstrace.DefaultBits, "trace length in packets (power of two)")
		pinDir := fs.String("pin-dir", "", "bpffs directory to pin the TCX link")
		exitOnErr(fs.Parse(args[1:]))
		if *dev == "" || *seed == 0 || *lossPct <= 0 || *pinDir == "" {
			fmt.Fprintln(os.Stderr, "losstrace attach: -dev, -seed, -loss-pct, -pin-dir are required")
			os.Exit(1)
		}
		words, actualPct, err := losstrace.Generate(*seed, *lossPct, *burst, *bits)
		exitOnErr(err)
		exitOnErr(losstrace.Attach(*dev, words, *pinDir))
		fmt.Fprintf(os.Stderr, "losstrace attach dev=%s seed=%d loss=%g%% (realized %.4f%%) burst=%g bits=%d\n",
			*dev, *seed, *lossPct, actualPct, *burst, *bits)
	case "detach":
		fs := flag.NewFlagSet("losstrace detach", flag.ExitOnError)
		dev := fs.String("dev", "", "veth device name")
		pinDir := fs.String("pin-dir", "", "bpffs directory holding the pinned link")
		exitOnErr(fs.Parse(args[1:]))
		if *dev == "" || *pinDir == "" {
			fmt.Fprintln(os.Stderr, "losstrace detach: -dev and -pin-dir are required")
			os.Exit(1)
		}
		exitOnErr(losstrace.Detach(*dev, *pinDir))
	}
}
