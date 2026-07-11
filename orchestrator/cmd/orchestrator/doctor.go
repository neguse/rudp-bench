package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/neguse/rudp-bench/orchestrator/doctor"
	"github.com/neguse/rudp-bench/orchestrator/rig"
)

func doctorMain(args []string) {
	fs := flag.NewFlagSet("doctor", flag.ExitOnError)
	rigPath := fs.String("rig", "orchestrator/rigs/home.json", "rig description JSON")
	output := fs.String("output", "", "write report JSON to this path (default stdout)")
	repo := fs.String("repo", ".", "repository directory for source metadata")
	exitOnErr(fs.Parse(args))

	r, err := rig.Load(*rigPath)
	exitOnErr(err)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	report := doctor.Collect(ctx, r, *repo)
	if report.Git.Dirty && *output != "" {
		exitOnErr(doctor.CaptureSourceSnapshot(ctx, *repo, *output, &report))
	}
	exitOnErr(doctor.Write(*output, report))
	for _, check := range report.Checks {
		fmt.Fprintf(os.Stderr, "doctor: %-24s %s", check.Name, check.Status)
		if check.Observed != "" {
			fmt.Fprintf(os.Stderr, " observed=%s", check.Observed)
		}
		if check.Expected != "" {
			fmt.Fprintf(os.Stderr, " expected=%s", check.Expected)
		}
		fmt.Fprintln(os.Stderr)
	}
	if *output != "" {
		fmt.Fprintf(os.Stderr, "doctor report: %s\n", *output)
	}
	if !report.OK {
		os.Exit(2)
	}
}
