package run

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var (
	errRampSnapshotPending = errors.New("ramp snapshot is pending")
	rampActiveSuffix       = regexp.MustCompile(`^c([0-9]{6})\.json$`)
)

type rampSnapshot struct {
	Index       int
	ActiveConns int
	Path        string
}

type rampTrafficBreak struct {
	name  string
	class string
	cause string
}

// locateRampSnapshot resolves exactly one endpoint artifact for one phase.
// It deliberately inspects only the requested index: once an earlier phase
// has crossed an SLO, malformed or absent tail artifacts are irrelevant.
func locateRampSnapshot(basePath string, index, wantActive int) (rampSnapshot, error) {
	dir := filepath.Dir(basePath)
	prefix := fmt.Sprintf("%s.ramp-%06d-", filepath.Base(basePath), index)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return rampSnapshot{}, fmt.Errorf("read ramp snapshot directory for %s: %w", basePath, err)
	}
	var found []rampSnapshot
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasPrefix(entry.Name(), prefix) {
			continue
		}
		match := rampActiveSuffix.FindStringSubmatch(strings.TrimPrefix(entry.Name(), prefix))
		if match == nil {
			return rampSnapshot{}, fmt.Errorf("ramp snapshot %s does not match .ramp-%%06d-c%%06d.json",
				filepath.Join(dir, entry.Name()))
		}
		active, err := strconv.Atoi(match[1])
		if err != nil {
			return rampSnapshot{}, fmt.Errorf("parse ramp snapshot connections %s: %w", entry.Name(), err)
		}
		found = append(found, rampSnapshot{
			Index: index, ActiveConns: active, Path: filepath.Join(dir, entry.Name()),
		})
	}
	if len(found) == 0 {
		return rampSnapshot{}, fmt.Errorf("%w: %s phase=%d", errRampSnapshotPending, basePath, index)
	}
	if len(found) != 1 {
		paths := make([]string, 0, len(found))
		for _, snapshot := range found {
			paths = append(paths, snapshot.Path)
		}
		return rampSnapshot{}, fmt.Errorf("duplicate ramp snapshot index %d for %s: %v", index, basePath, paths)
	}
	if found[0].ActiveConns != wantActive {
		return rampSnapshot{}, fmt.Errorf("ramp phase %d active connections=%d, want %d (%s)",
			index, found[0].ActiveConns, wantActive, found[0].Path)
	}
	return found[0], nil
}

// evaluateRampPhase reads and judges one complete server/client snapshot pair.
// Contract errors are returned as INVALID. Insufficient offered load or
// observation coverage is represented as a failed evaluation, because it is
// precisely the first capacity break the ramp is trying to locate.
func evaluateRampPhase(cfg RunConfig, index int, serverBase, clientBase string) (RampPoint, error) {
	if cfg.Ramp == nil || cfg.Scenario == nil {
		return RampPoint{}, fmt.Errorf("ramp config and scenario are required")
	}
	wantActive := cfg.Ramp.activeConns(index, cfg.TotalConns)
	serverSnapshot, err := locateRampSnapshot(serverBase, index, wantActive)
	if err != nil {
		return RampPoint{}, fmt.Errorf("server: %w", err)
	}
	clientSnapshot, err := locateRampSnapshot(clientBase, index, wantActive)
	if err != nil {
		return RampPoint{}, fmt.Errorf("client: %w", err)
	}

	var mergePaths []string
	switch cfg.Scenario.Kind {
	case ScenarioAuthoritativeState:
		mergePaths = []string{serverSnapshot.Path, clientSnapshot.Path}
	case ScenarioRoomRelay, ScenarioEnvironmentBaseline:
		// Relay/baseline accounting is client-owned. Still parse the server
		// artifact so malformed endpoint output cannot silently pass.
		if _, err := readMetricsFileWithOptions(serverSnapshot.Path, true); err != nil {
			return RampPoint{}, err
		}
		mergePaths = []string{clientSnapshot.Path}
	default:
		return RampPoint{}, fmt.Errorf("ramp does not support scenario kind %q", cfg.Scenario.Kind)
	}
	metrics, err := MergeMetricsFiles(mergePaths, wantActive)
	if err != nil {
		return RampPoint{}, fmt.Errorf("metrics: %w", err)
	}
	if metrics.Version != 2 {
		return RampPoint{}, fmt.Errorf("metrics version=%d, want 2", metrics.Version)
	}
	if err := ValidateMergedMetricsConsistency(metrics); err != nil {
		return RampPoint{}, fmt.Errorf("metrics consistency: %w", err)
	}

	evaluation := EvaluateScenarioMetrics(metrics, *cfg.Scenario)
	breaks, err := assessRampTraffic(metrics, cfg, wantActive)
	if err != nil {
		return RampPoint{}, fmt.Errorf("metrics contract: %w", err)
	}
	for _, failure := range breaks {
		addRampTrafficBreak(&evaluation, failure)
	}
	return RampPoint{Index: index, ActiveConns: wantActive, Evaluation: evaluation}, nil
}

func assessRampTraffic(metrics *MergedMetrics, cfg RunConfig, activeConns int) ([]rampTrafficBreak, error) {
	if cfg.Ramp == nil || cfg.Scenario == nil {
		return nil, fmt.Errorf("ramp config and scenario are required")
	}
	var breaks []rampTrafficBreak
	for _, trafficCase := range scenarioMetricCases(*cfg.Scenario) {
		if trafficCase.spec == nil {
			continue
		}
		for className, classSpec := range enabledTrafficClasses(*trafficCase.spec) {
			metric, ok := metrics.TrafficMetric(trafficCase.spec.TrafficID, trafficCase.direction, className)
			if !ok {
				// A completely stalled sender may emit no traffic entry at all. That
				// is offered-load shortage (the desired break), not malformed JSON.
				breaks = append(breaks, rampTrafficBreak{trafficCase.name, className, "metrics missing"})
				continue
			}
			if metric.DeadlineNS != trafficCase.spec.MustDeliver.DeadlineNS {
				return nil, fmt.Errorf("%s/%s deadline_ns=%d, want %d", trafficCase.name, className,
					metric.DeadlineNS, trafficCase.spec.MustDeliver.DeadlineNS)
			}
			minSlots, maxSlots, err := expectedSlotRange(classSpec.RateHz, cfg.Ramp.Sample.Duration, activeConns)
			if err != nil {
				return nil, fmt.Errorf("%s/%s expected slots: %w", trafficCase.name, className, err)
			}
			if metric.Slots > maxSlots {
				return nil, fmt.Errorf("%s/%s slots=%d exceeds expected maximum %d", trafficCase.name, className,
					metric.Slots, maxSlots)
			}
			if metric.Slots < minSlots {
				breaks = append(breaks, rampTrafficBreak{trafficCase.name, className,
					fmt.Sprintf("slots=%d below expected minimum %d", metric.Slots, minSlots)})
			}
			if metric.AttemptedRatio < cfg.AttemptedThreshold {
				breaks = append(breaks, rampTrafficBreak{trafficCase.name, className,
					fmt.Sprintf("attempted=%.6f below %.6f", metric.AttemptedRatio, cfg.AttemptedThreshold)})
			}

			if className != ClassLossTolerant {
				continue
			}
			expectedFlows := expectedLatestFlows(cfg.Scenario.Kind, "merged", trafficCase.direction,
				activeConns, activeConns)
			if metric.ExpectedFlows != uint64(expectedFlows) {
				return nil, fmt.Errorf("%s/%s expected_flows=%d, want %d", trafficCase.name, className,
					metric.ExpectedFlows, expectedFlows)
			}
			minSamples, maxSamples, err := expectedStalenessSampleRange(
				cfg.Ramp.Sample.Duration, cfg.StalenessPeriodNS, expectedFlows)
			if err != nil {
				return nil, fmt.Errorf("%s/%s expected staleness samples: %w", trafficCase.name, className, err)
			}
			if metric.StalenessNS.Count > maxSamples {
				return nil, fmt.Errorf("%s/%s staleness samples=%d exceeds expected maximum %d",
					trafficCase.name, className, metric.StalenessNS.Count, maxSamples)
			}
			if metric.StalenessNS.Count < minSamples {
				breaks = append(breaks, rampTrafficBreak{trafficCase.name, className,
					fmt.Sprintf("staleness samples=%d below expected minimum %d", metric.StalenessNS.Count, minSamples)})
			}
		}
	}
	return breaks, nil
}

func addRampTrafficBreak(evaluation *ScenarioEvaluation, failure rampTrafficBreak) {
	for i := range evaluation.Traffic {
		traffic := &evaluation.Traffic[i]
		if traffic.Name != failure.name || traffic.Class != failure.class {
			continue
		}
		traffic.OK = false
		if traffic.Cause == "" {
			traffic.Cause = failure.cause
		} else if !strings.Contains(traffic.Cause, failure.cause) {
			traffic.Cause += "; " + failure.cause
		}
		break
	}
	rebuildScenarioEvaluationCause(evaluation)
}

func rebuildScenarioEvaluationCause(evaluation *ScenarioEvaluation) {
	causes := make([]string, 0, len(evaluation.Traffic))
	for _, traffic := range evaluation.Traffic {
		if !traffic.OK {
			causes = append(causes, traffic.Name+"/"+traffic.Class+": "+traffic.Cause)
		}
	}
	evaluation.OK = len(causes) == 0
	evaluation.Cause = strings.Join(causes, "; ")
}

// scoreRampSnapshots consumes only the complete prefix through the first
// failed point. A censored result, by contrast, is valid only after every
// configured level has produced a passing snapshot pair.
func scoreRampSnapshots(cfg RunConfig, serverBase string, clientBases []string) (*RampResult, error) {
	if cfg.Ramp == nil {
		return nil, fmt.Errorf("ramp config is missing")
	}
	if cfg.Scenario == nil {
		return nil, fmt.Errorf("ramp scenario is missing")
	}
	if len(clientBases) != 1 {
		return nil, fmt.Errorf("ramp client metrics bases=%d, want 1", len(clientBases))
	}
	levels, err := cfg.Ramp.levels(cfg.TotalConns)
	if err != nil {
		return nil, err
	}

	result := &RampResult{Timeline: make([]RampPoint, 0, levels)}
	for index := 0; index < levels; index++ {
		point, err := evaluateRampPhase(cfg, index, serverBase, clientBases[0])
		if err != nil {
			return nil, fmt.Errorf("ramp phase %d: %w", index, err)
		}
		result.Timeline = append(result.Timeline, point)
		if !point.Evaluation.OK {
			result.ScoreConns = point.ActiveConns
			result.Cause = point.Evaluation.Cause
			return result, nil
		}
	}
	result.Censored = true
	result.Cause = fmt.Sprintf("no SLO violation through %d connections", cfg.TotalConns)
	return result, nil
}

// watchRampSnapshots evaluates completed phase pairs while endpoints are still
// alive. The first break creates a shared stop marker; endpoints observe it at
// their phase boundary, send DONE, and exit without driving deeper overload.
func watchRampSnapshots(ctx context.Context, cfg RunConfig, serverBase string, clientBases []string, stopPath string) error {
	if cfg.Ramp == nil || len(clientBases) != 1 || stopPath == "" {
		return fmt.Errorf("invalid ramp watcher configuration")
	}
	levels, err := cfg.Ramp.levels(cfg.TotalConns)
	if err != nil {
		return err
	}
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()
	for index := 0; index < levels; {
		point, err := evaluateRampPhase(cfg, index, serverBase, clientBases[0])
		if err == nil {
			if !point.Evaluation.OK {
				return writeRampStop(stopPath, point)
			}
			index++
			continue
		}
		// Missing and partially-written artifacts are expected while a phase is
		// in flight. Persistent structural errors are diagnosed by the final
		// offline scorer after the endpoints finish.
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
		}
	}
	return nil
}

func writeRampStop(path string, point RampPoint) error {
	contents := fmt.Sprintf("phase=%d\nactive_conns=%d\ncause=%s\n",
		point.Index, point.ActiveConns, point.Evaluation.Cause)
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(contents), 0o644); err != nil {
		return fmt.Errorf("write ramp stop marker: %w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("publish ramp stop marker: %w", err)
	}
	return nil
}
