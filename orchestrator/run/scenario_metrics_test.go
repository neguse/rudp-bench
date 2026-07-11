package run

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestValidateScenarioMetricsFilesAuthoritative(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	writeScenarioMetricsFile(t, serverPath, authoritativeTrafficFixture(scenario, "server", 3, 3))
	writeScenarioMetricsFile(t, clientPath, authoritativeTrafficFixture(scenario, "client", 3, 3))
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged); err != nil {
		t.Fatal(err)
	}
}

func TestValidateScenarioMetricsDataDoesNotReopenReplacedPaths(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	writeScenarioMetricsFile(t, serverPath, authoritativeTrafficFixture(scenario, "server", 3, 3))
	writeScenarioMetricsFile(t, clientPath, authoritativeTrafficFixture(scenario, "client", 3, 3))
	serverBytes, err := os.ReadFile(serverPath)
	if err != nil {
		t.Fatal(err)
	}
	clientBytes, err := os.ReadFile(clientPath)
	if err != nil {
		t.Fatal(err)
	}
	server := NewMetricsArtifactData(serverPath, serverBytes)
	client := NewMetricsArtifactData(clientPath, clientBytes)
	if err := os.WriteFile(serverPath, []byte(`{"tampered":true}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(clientPath, []byte(`{"tampered":true}`), 0o644); err != nil {
		t.Fatal(err)
	}
	merged, err := MergeMetricsData([]MetricsArtifactData{server, client}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsData(server, []MetricsArtifactData{client}, []int{3}, 3,
		time.Second, 10_000_000, scenario, merged); err != nil {
		t.Fatalf("snapshot validation reopened a replaced source path: %v", err)
	}
}

func TestValidateScenarioMetricsFilesAllowsUnobservedMustDeliverReceiver(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 3, 3)
	client := authoritativeTrafficFixture(scenario, "client", 3, 3)
	server.Traffic = removeTrafficMetric(server.Traffic, DirectionClientToServer, ClassMustDeliver)
	client.Traffic = removeTrafficMetric(client.Traffic, DirectionServerToClient, ClassMustDeliver)
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged); err != nil {
		t.Fatal(err)
	}
}

func TestValidateScenarioMetricsFilesAllowsEmptyRelayServer(t *testing.T) {
	dir := t.TempDir()
	scenario := ScenarioSpec{
		Name: "relay",
		Kind: ScenarioRoomRelay,
		RoomPublish: &TrafficSpec{
			TrafficID:    3,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 64},
			MustDeliver:  TrafficClassSpec{RateHz: 1, PayloadBytes: 64, DeadlineNS: 100_000_000},
		},
	}
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 3, 3)
	server.Traffic = []trafficMetric{}
	client := authoritativeTrafficFixture(scenario, "client", 3, 3)
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged); err != nil {
		t.Fatal(err)
	}
}

func TestValidateScenarioMetricsFilesRejectsContractMismatch(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 3, 3)
	client := authoritativeTrafficFixture(scenario, "client", 3, 3)
	client.Traffic[0].DeadlineNS++
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	if _, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3); err == nil || !strings.Contains(err.Error(), "deadline_ns") {
		t.Fatalf("deadline mismatch error = %v", err)
	}

	client = authoritativeTrafficFixture(scenario, "client", 3, 3)
	client.Traffic = client.Traffic[1:]
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged); err == nil || !strings.Contains(err.Error(), "missing client_input/") {
		t.Fatalf("missing traffic error = %v", err)
	}
}

func TestValidateScenarioMetricsFilesRejectsWrongOfferedRate(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 3, 3)
	client := authoritativeTrafficFixture(scenario, "client", 3, 3)
	for i := range client.Traffic {
		if client.Traffic[i].Direction == DirectionClientToServer && client.Traffic[i].Class == ClassLossTolerant {
			client.Traffic[i].Slots /= 2
			client.Traffic[i].Submitted = client.Traffic[i].Slots
		}
	}
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	err = ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged)
	if err == nil || !strings.Contains(err.Error(), "slots=") {
		t.Fatalf("offered rate error = %v", err)
	}
}

func TestValidateScenarioMetricsFilesRejectsSparseStalenessCoverage(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 3, 3)
	client := authoritativeTrafficFixture(scenario, "client", 3, 3)
	for i := range client.Traffic {
		if client.Traffic[i].Direction == DirectionServerToClient && client.Traffic[i].Class == ClassLossTolerant {
			client.Traffic[i].StalenessNS = sampledV2Histogram(1)
		}
	}
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 3)
	if err != nil {
		t.Fatal(err)
	}
	err = ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{3}, 3, time.Second, 10_000_000, scenario, merged)
	if err == nil || !strings.Contains(err.Error(), "staleness samples") {
		t.Fatalf("coverage error = %v", err)
	}
}

func TestValidateScenarioMetricsFilesRequiresV2(t *testing.T) {
	dir := t.TempDir()
	scenario := authoritativeFixture()
	serverPath := filepath.Join(dir, "server.json")
	clientPath := filepath.Join(dir, "client.json")
	server := authoritativeTrafficFixture(scenario, "server", 1, 1)
	client := authoritativeTrafficFixture(scenario, "client", 1, 1)
	server.Version = 1
	client.Version = 1
	writeScenarioMetricsFile(t, serverPath, server)
	writeScenarioMetricsFile(t, clientPath, client)
	merged, err := MergeMetricsFiles([]string{serverPath, clientPath}, 1)
	if err != nil {
		t.Fatal(err)
	}
	if err := ValidateScenarioMetricsFiles(serverPath, []string{clientPath}, []int{1}, 1, time.Second, 10_000_000, scenario, merged); err == nil || !strings.Contains(err.Error(), "require version 2") {
		t.Fatalf("v1 scenario error = %v", err)
	}
}

func TestMergeMetricsV2RejectsMissingRequiredCountField(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "broken.json")
	file := authoritativeTrafficFixture(authoritativeFixture(), "server", 1, 1)
	data, err := json.Marshal(file)
	if err != nil {
		t.Fatal(err)
	}
	broken := strings.Replace(string(data), `"never_received_flows":0,`, "", 1)
	if broken == string(data) {
		t.Fatal("fixture did not contain required count field")
	}
	if err := os.WriteFile(path, []byte(broken), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := MergeMetricsFiles([]string{path}, 1); err == nil || !strings.Contains(err.Error(), "missing required field") {
		t.Fatalf("shape error = %v", err)
	}
}

func authoritativeTrafficFixture(scenario ScenarioSpec, role string, localConns, totalConns int) metricsFile {
	empty := emptyV2Histogram()
	file := metricsFile{
		Version: 2,
		Histogram: HistogramLayout{
			Scheme: "log2x16", Subbins: 16, MinNS: 1_000, MaxNS: 100_000_000_000,
		},
		Classes: map[string]classMetric{
			ClassLossTolerant: {LatencySchedNS: empty, LatencySendNS: empty, UpdateGapNS: empty},
			ClassMustDeliver:  {LatencySchedNS: empty, LatencySendNS: empty, UpdateGapNS: empty},
		},
		StalenessNS: empty,
	}
	for _, trafficCase := range scenarioMetricCases(scenario) {
		for className := range enabledTrafficClasses(*trafficCase.spec) {
			expectedFlows := expectedLatestFlows(scenario.Kind, role, trafficCase.direction, localConns, totalConns)
			sender := (role == "client" && trafficCase.direction == DirectionClientToServer) ||
				(role == "server" && trafficCase.direction == DirectionServerToClient) ||
				scenario.Kind != ScenarioAuthoritativeState
			file.Traffic = append(file.Traffic, trafficMetric{
				TrafficID:  trafficCase.spec.TrafficID,
				Direction:  trafficCase.direction,
				Class:      className,
				DeadlineNS: trafficCase.spec.MustDeliver.DeadlineNS,
				ClassCounts: ClassCounts{
					ExpectedFlows: uint64(expectedFlows), ObservedFlows: uint64(expectedFlows),
				},
				LatencySchedNS: empty,
				LatencySendNS:  empty,
				UpdateGapNS:    empty,
				StalenessNS:    empty,
			})
			if sender {
				last := &file.Traffic[len(file.Traffic)-1]
				classSpec := trafficCase.spec.LossTolerant
				if className == ClassMustDeliver {
					classSpec = trafficCase.spec.MustDeliver
				}
				streams := localConns
				if role == "server" {
					streams = totalConns
				}
				slots, _, _ := expectedSlotRange(classSpec.RateHz, time.Second, streams)
				last.Slots = slots
				last.Submitted = slots
			}
			if className == ClassLossTolerant && expectedFlows > 0 {
				last := &file.Traffic[len(file.Traffic)-1]
				last.StalenessNS = sampledV2Histogram(uint64(expectedFlows * 100))
			}
		}
	}
	return file
}

func emptyV2Histogram() Histogram {
	return Histogram{
		Scheme: "log2x16", MinNS: 1_000, MaxNS: 100_000_000_000,
		Bins: make([]uint64, v2HistogramBins),
	}
}

func sampledV2Histogram(count uint64) Histogram {
	h := emptyV2Histogram()
	h.Count = count
	h.P50NS, h.P90NS, h.P99NS = 1_000, 1_000, 1_000
	h.Bins[0] = count
	return h
}

func writeScenarioMetricsFile(t *testing.T, path string, file metricsFile) {
	t.Helper()
	rebuildFixtureAggregates(t, &file)
	data, err := json.Marshal(file)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func rebuildFixtureAggregates(t *testing.T, file *metricsFile) {
	t.Helper()
	file.Classes = map[string]classMetric{
		ClassLossTolerant: {LatencySchedNS: emptyV2Histogram(), LatencySendNS: emptyV2Histogram(), UpdateGapNS: emptyV2Histogram()},
		ClassMustDeliver:  {LatencySchedNS: emptyV2Histogram(), LatencySendNS: emptyV2Histogram(), UpdateGapNS: emptyV2Histogram()},
	}
	file.StalenessNS = emptyV2Histogram()
	for _, traffic := range file.Traffic {
		aggregate := file.Classes[traffic.Class]
		aggregate.ClassCounts = addClassCounts(aggregate.ClassCounts, traffic.ClassCounts)
		if err := addHistogram(&aggregate.LatencySchedNS, traffic.LatencySchedNS, file.Histogram); err != nil {
			t.Fatal(err)
		}
		if err := addHistogram(&aggregate.LatencySendNS, traffic.LatencySendNS, file.Histogram); err != nil {
			t.Fatal(err)
		}
		if err := addHistogram(&aggregate.UpdateGapNS, traffic.UpdateGapNS, file.Histogram); err != nil {
			t.Fatal(err)
		}
		file.Classes[traffic.Class] = aggregate
		if traffic.Class == ClassLossTolerant {
			if err := addHistogram(&file.StalenessNS, traffic.StalenessNS, file.Histogram); err != nil {
				t.Fatal(err)
			}
		}
	}
	for class, aggregate := range file.Classes {
		finalizeHistogram(&aggregate.LatencySchedNS, file.Histogram)
		finalizeHistogram(&aggregate.LatencySendNS, file.Histogram)
		finalizeHistogram(&aggregate.UpdateGapNS, file.Histogram)
		file.Classes[class] = aggregate
	}
	finalizeHistogram(&file.StalenessNS, file.Histogram)
}

func removeTrafficMetric(metrics []trafficMetric, direction, class string) []trafficMetric {
	out := metrics[:0]
	for _, metric := range metrics {
		if metric.Direction != direction || metric.Class != class {
			out = append(out, metric)
		}
	}
	return out
}
