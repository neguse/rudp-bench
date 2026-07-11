package run

import (
	"fmt"
	"math"
	"time"
)

type scenarioMetricCase struct {
	name      string
	direction string
	spec      *TrafficSpec
}

func scenarioMetricCases(scenario ScenarioSpec) []scenarioMetricCase {
	switch scenario.Kind {
	case ScenarioEnvironmentBaseline:
		return []scenarioMetricCase{{name: "baseline", direction: DirectionRoomRelay, spec: scenario.ClientInput}}
	case ScenarioAuthoritativeState:
		return []scenarioMetricCase{
			{name: "client_input", direction: DirectionClientToServer, spec: scenario.ClientInput},
			{name: "server_state", direction: DirectionServerToClient, spec: scenario.ServerState},
		}
	case ScenarioRoomRelay:
		return []scenarioMetricCase{{name: "room_publish", direction: DirectionRoomRelay, spec: scenario.RoomPublish}}
	default:
		return nil
	}
}

// ValidateScenarioMetricsFiles verifies the adapter/metrics contract before
// application SLOs are evaluated. Contract failures make a run INVALID; they
// are not evidence that the transport exceeded capacity.
func ValidateScenarioMetricsFiles(
	serverPath string,
	clientPaths []string,
	clientConns []int,
	totalConns int,
	duration time.Duration,
	stalenessPeriodNS uint64,
	scenario ScenarioSpec,
	merged *MergedMetrics,
) error {
	if len(clientPaths) != len(clientConns) {
		return fmt.Errorf("client metrics paths=%d, connection partitions=%d", len(clientPaths), len(clientConns))
	}
	server, err := readMetricsFileWithOptions(serverPath, true)
	if err != nil {
		return err
	}
	if err := validateScenarioProcessMetrics(server, serverPath, "server", totalConns, totalConns, duration, scenario); err != nil {
		return err
	}
	for i, path := range clientPaths {
		client, err := readMetricsFile(path)
		if err != nil {
			return err
		}
		if err := validateScenarioProcessMetrics(client, path, "client", clientConns[i], totalConns, duration, scenario); err != nil {
			return err
		}
	}
	if merged == nil || merged.Version != 2 {
		version := 0
		if merged != nil {
			version = merged.Version
		}
		return fmt.Errorf("merged metrics version=%d, scenario runs require version 2", version)
	}
	for _, trafficCase := range scenarioMetricCases(scenario) {
		if trafficCase.spec == nil {
			continue
		}
		for className := range enabledTrafficClasses(*trafficCase.spec) {
			metric, ok := merged.TrafficMetric(trafficCase.spec.TrafficID, trafficCase.direction, className)
			if !ok {
				return fmt.Errorf("merged metrics missing %s/%s traffic", trafficCase.name, className)
			}
			if metric.Slots == 0 {
				return fmt.Errorf("merged %s/%s has zero measured slots", trafficCase.name, className)
			}
			if metric.DeadlineNS != trafficCase.spec.MustDeliver.DeadlineNS {
				return fmt.Errorf("merged %s/%s deadline_ns=%d, want %d", trafficCase.name, className,
					metric.DeadlineNS, trafficCase.spec.MustDeliver.DeadlineNS)
			}
			if className == ClassLossTolerant {
				expectedFlows := expectedLatestFlows(scenario.Kind, "merged", trafficCase.direction, totalConns, totalConns)
				if metric.ExpectedFlows != uint64(expectedFlows) {
					return fmt.Errorf("merged %s/%s expected_flows=%d, want %d", trafficCase.name, className,
						metric.ExpectedFlows, expectedFlows)
				}
				minSamples, maxSamples, err := expectedStalenessSampleRange(duration, stalenessPeriodNS, expectedFlows)
				if err != nil || metric.StalenessNS.Count < minSamples || metric.StalenessNS.Count > maxSamples {
					return fmt.Errorf("merged %s/%s staleness samples=%d, want %d..%d for flows=%d duration=%s period_ns=%d",
						trafficCase.name, className, metric.StalenessNS.Count, minSamples, maxSamples,
						expectedFlows, duration, stalenessPeriodNS)
				}
			}
		}
	}
	return nil
}

func expectedStalenessSampleRange(duration time.Duration, periodNS uint64, flows int) (uint64, uint64, error) {
	if duration <= 0 || periodNS == 0 || flows <= 0 {
		return 0, 0, fmt.Errorf("duration, staleness period, and flows must be positive")
	}
	ticks := uint64(duration) / periodNS
	if uint64(duration)%periodNS != 0 {
		ticks++
	}
	minTicks := uint64(1)
	if ticks > 2 {
		minTicks = ticks - 2
	}
	maxTicks := ticks + 2
	if maxTicks > ^uint64(0)/uint64(flows) {
		return 0, 0, fmt.Errorf("staleness sample count overflows")
	}
	return minTicks * uint64(flows), maxTicks * uint64(flows), nil
}

func validateScenarioProcessMetrics(
	file metricsFile,
	path string,
	role string,
	localConns int,
	totalConns int,
	duration time.Duration,
	scenario ScenarioSpec,
) error {
	if file.Version != 2 {
		return fmt.Errorf("metrics %s version=%d, scenario runs require version 2", path, file.Version)
	}
	// Relay servers do not own sender/receiver accounting, but their metrics
	// file must still prove that the binary implements the v2 contract.
	if role == "server" && scenario.Kind != ScenarioAuthoritativeState {
		return nil
	}
	for _, trafficCase := range scenarioMetricCases(scenario) {
		if trafficCase.spec == nil {
			continue
		}
		for className := range enabledTrafficClasses(*trafficCase.spec) {
			sender := scenarioProcessSendsTraffic(role, trafficCase.direction, scenario.Kind)
			if !sender && !scenarioProcessRequiresTraffic(role, trafficCase.direction, className, scenario.Kind) {
				continue
			}
			metric, ok := fileTrafficMetric(file, trafficCase.spec.TrafficID, trafficCase.direction, className)
			if !ok {
				return fmt.Errorf("metrics %s missing %s/%s traffic", path, trafficCase.name, className)
			}
			if metric.DeadlineNS != trafficCase.spec.MustDeliver.DeadlineNS {
				return fmt.Errorf("metrics %s %s/%s deadline_ns=%d, want %d", path, trafficCase.name, className,
					metric.DeadlineNS, trafficCase.spec.MustDeliver.DeadlineNS)
			}
			if sender {
				classSpec := trafficCase.spec.LossTolerant
				if className == ClassMustDeliver {
					classSpec = trafficCase.spec.MustDeliver
				}
				streams := localConns
				if role == "server" {
					streams = totalConns
				}
				minSlots, maxSlots, err := expectedSlotRange(classSpec.RateHz, duration, streams)
				if err != nil {
					return fmt.Errorf("metrics %s %s/%s: %w", path, trafficCase.name, className, err)
				}
				if metric.Slots < minSlots || metric.Slots > maxSlots {
					return fmt.Errorf("metrics %s %s/%s slots=%d, want %d..%d for rate=%g duration=%s streams=%d",
						path, trafficCase.name, className, metric.Slots, minSlots, maxSlots,
						classSpec.RateHz, duration, streams)
				}
			}
			if className != ClassLossTolerant {
				continue
			}
			expectedFlows := expectedLatestFlows(scenario.Kind, role, trafficCase.direction, localConns, totalConns)
			if metric.ExpectedFlows != uint64(expectedFlows) {
				return fmt.Errorf("metrics %s %s/%s expected_flows=%d, want %d", path, trafficCase.name,
					className, metric.ExpectedFlows, expectedFlows)
			}
		}
	}
	return nil
}

func expectedLatestFlows(kind ScenarioKind, role, direction string, localConns, totalConns int) int {
	switch kind {
	case ScenarioEnvironmentBaseline:
		if role == "client" || role == "merged" {
			return localConns
		}
	case ScenarioRoomRelay:
		if role == "client" || role == "merged" {
			return localConns * totalConns
		}
	case ScenarioAuthoritativeState:
		switch {
		case role == "server" && direction == DirectionClientToServer:
			return totalConns
		case role == "client" && direction == DirectionServerToClient:
			return localConns
		case role == "merged":
			return totalConns
		}
	}
	return 0
}

func scenarioProcessRequiresTraffic(role, direction, className string, kind ScenarioKind) bool {
	if kind != ScenarioAuthoritativeState {
		return role == "client"
	}
	sender := scenarioProcessSendsTraffic(role, direction, kind)
	return sender || className == ClassLossTolerant
}

func scenarioProcessSendsTraffic(role, direction string, kind ScenarioKind) bool {
	if kind != ScenarioAuthoritativeState {
		return role == "client"
	}
	return (role == "client" && direction == DirectionClientToServer) ||
		(role == "server" && direction == DirectionServerToClient)
}

func expectedSlotRange(rateHz float64, duration time.Duration, streams int) (uint64, uint64, error) {
	if rateHz <= 0 || duration <= 0 || streams <= 0 {
		return 0, 0, fmt.Errorf("rate, duration, and streams must be positive")
	}
	intervalFloat := 1_000_000_000.0 / rateHz
	if math.IsNaN(intervalFloat) || math.IsInf(intervalFloat, 0) || intervalFloat < 1 || intervalFloat > float64(^uint64(0)) {
		return 0, 0, fmt.Errorf("invalid rate %g", rateHz)
	}
	interval := uint64(intervalFloat + 0.5)
	durationNS := uint64(duration)
	perStreamMin := durationNS / interval
	perStreamMax := perStreamMin
	if durationNS%interval != 0 {
		perStreamMax++
	}
	if perStreamMax > ^uint64(0)/uint64(streams) {
		return 0, 0, fmt.Errorf("slot count overflows")
	}
	return perStreamMin * uint64(streams), perStreamMax * uint64(streams), nil
}

func enabledTrafficClasses(spec TrafficSpec) map[string]TrafficClassSpec {
	out := map[string]TrafficClassSpec{}
	if spec.LossTolerant.RateHz > 0 {
		out[ClassLossTolerant] = spec.LossTolerant
	}
	if spec.MustDeliver.RateHz > 0 {
		out[ClassMustDeliver] = spec.MustDeliver
	}
	return out
}

func fileTrafficMetric(file metricsFile, trafficID uint8, direction, class string) (trafficMetric, bool) {
	for _, metric := range file.Traffic {
		if metric.TrafficID == trafficID && metric.Direction == direction && metric.Class == class {
			return metric, true
		}
	}
	return trafficMetric{}, false
}
