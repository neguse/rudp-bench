package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/neguse/rudp-bench/orchestrator/conformance"
	orun "github.com/neguse/rudp-bench/orchestrator/run"
)

func mappingConformanceMain(args []string) {
	fs := flag.NewFlagSet("mapping-conformance", flag.ExitOnError)
	configPath := fs.String("config", "", "class-mapping session config JSON path")
	outputDir := fs.String("output-dir", "", "override config output_dir")
	doctorReport := fs.String("doctor-report", "", "override config doctor_report")
	planOnly := fs.Bool("plan", false, "preflight endpoints and print the fixed acquisition plan")
	exitOnErr(fs.Parse(args))
	if *configPath == "" {
		fmt.Fprintln(os.Stderr, "mapping-conformance -config is required")
		os.Exit(1)
	}

	config, err := conformance.LoadSessionConfig(*configPath)
	exitOnErr(err)
	if *outputDir != "" {
		config.OutputDir = *outputDir
	}
	if *doctorReport != "" {
		config.DoctorReport = *doctorReport
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if *planOnly {
		plan, err := conformance.BuildSessionPlan(ctx, config)
		exitOnErr(err)
		encoder := json.NewEncoder(os.Stdout)
		encoder.SetIndent("", "  ")
		exitOnErr(encoder.Encode(plan))
		return
	}

	report, err := conformance.RunSession(ctx, config)
	exitOnErr(err)
	reportPath := filepath.Join(config.OutputDir, ".conformance", "report.json")
	manifestPath := filepath.Join(config.OutputDir, ".conformance", "manifest.json")
	fmt.Fprintf(os.Stderr, "mapping conformance report: %s\n", reportPath)
	fmt.Fprintf(os.Stderr, "mapping conformance manifest: %s\n", manifestPath)
	fmt.Fprintf(os.Stderr, "mapping conformance outcome: %s promotable=%v\n", report.Outcome, report.Promotable)

	if report.Outcome == orun.OutcomePass && report.Promotable {
		return
	}
	switch report.Outcome {
	case orun.OutcomeFail:
		os.Exit(3)
	case orun.OutcomeInvalid:
		os.Exit(2)
	case orun.OutcomeUnsupported:
		os.Exit(4)
	case orun.OutcomePass, orun.OutcomeInconclusive, orun.OutcomeCensored:
		os.Exit(5)
	default:
		os.Exit(2)
	}
}
