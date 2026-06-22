package sweep

import "fmt"

// ShowPlan prints a summary of the planned sweep execution without running it.
func ShowPlan(cfg RunConfig) {
	totalPoints := 0
	for _, p := range cfg.Profiles {
		activeLibs := 0
		for _, lib := range cfg.Libs {
			if unsupportedProfileReason(p, lib) == "" {
				activeLibs++
			}
		}
		totalPoints += activeLibs * len(p.Conns)
	}
	totalExecs := totalPoints * len(cfg.Runs)
	estSeconds := totalExecs * (cfg.Duration + 15)

	fmt.Println("=== Execution Plan ===")
	fmt.Printf("profiles:       %d\n", len(cfg.Profiles))
	fmt.Printf("libraries:      %d\n", len(cfg.Libs))
	fmt.Printf("sweep points:   %d\n", totalPoints)
	fmt.Printf("runs per point: %d\n", len(cfg.Runs))
	fmt.Printf("total execs:    %d\n", totalExecs)
	fmt.Printf("est. time:      %s+\n", formatDuration(estSeconds))
	fmt.Printf("netem:          %v\n", cfg.Netem)
	fmt.Printf("isolation:      %s\n", cfg.Isolate)
	fmt.Printf("adaptive:       %v", cfg.Adaptive)
	if cfg.Adaptive && cfg.Prior != nil {
		fmt.Printf(" (%d prior entries)", len(cfg.Prior))
	}
	fmt.Println()
	fmt.Printf("publish:        %v\n", cfg.Publish && !cfg.NoPublish)
	fmt.Printf("output:         %s\n", cfg.Out)
}

// formatDuration formats seconds into a human-readable "Xh Ym" or "Ym Zs" string.
func formatDuration(seconds int) string {
	if seconds < 0 {
		seconds = 0
	}
	h := seconds / 3600
	m := (seconds % 3600) / 60
	s := seconds % 60
	if h > 0 {
		return fmt.Sprintf("%dh %dm", h, m)
	}
	return fmt.Sprintf("%dm %ds", m, s)
}
