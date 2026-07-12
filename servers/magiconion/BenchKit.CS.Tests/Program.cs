using System.IO.Compression;
using System.Text.Json;
using BenchKit.CS;

var payloadHeader = new BenchHeader(
    Seq: 0x0102030405060708UL,
    SchedTsNs: 0x1112131415161718UL,
    SendTsNs: 0x2122232425262728UL,
    Flags: (byte)(BenchConstants.FlagMustDeliver | BenchConstants.FlagMeasure |
        BenchConstants.FlagBroadcast | BenchConstants.DirectionFlags(BenchDirection.ServerToClient)),
    OriginId: 0xa1b2c3d4U,
    TrafficId: 7);
var payload = new byte[1_000];
Require(BenchPayload.TryWrite(payload, payloadHeader), "failed to write payload header");
Require(BenchPayload.TryFillBody(payload, payloadHeader), "failed to fill payload body");
Require(BenchPayload.ValidateBody(payload, payloadHeader), "payload body did not validate");
var expectedBody = new byte[]
{
    0x51, 0x78, 0xa7, 0xf1, 0x04, 0x73, 0x94, 0x5c,
    0x08, 0x30, 0x24, 0x40, 0xcc, 0x32, 0x41, 0x3e,
};
Require(payload.AsSpan(BenchConstants.HeaderSize, expectedBody.Length).SequenceEqual(expectedBody),
    "splitmix64-v1 known-answer vector differs");
Require(payload[BenchConstants.HeaderSize + 223] == 0xdb &&
        payload[BenchConstants.HeaderSize + 224] == 0x90 &&
        payload[BenchConstants.HeaderSize + 255] == 0x96 &&
        payload[BenchConstants.HeaderSize + 256] == 0xd8 &&
        payload[999] == 0x1c,
    "splitmix64-v1 block-index vector differs");

using (var compressed = new MemoryStream())
{
    using (var compressor = new DeflateStream(compressed, CompressionLevel.SmallestSize, leaveOpen: true))
    {
        compressor.Write(payload);
    }
    Require(compressed.Length >= 950,
        $"representative payload compressed unexpectedly: {compressed.Length}/1000 bytes");
}

var metrics = new BenchMetrics(new BenchMetricsConfig(
    MaxOriginId: 1,
    DeadlineNs: 1,
    StalenessPeriodNs: BenchConstants.DefaultStalenessPeriodNs,
    MaxLocalIndex: 1));

metrics.SetTrafficDeadline(1, BenchDirection.ClientToServer, 100);
metrics.SetTrafficDeadline(2, BenchDirection.ServerToClient, 200);

var input = Header(1, BenchDirection.ClientToServer);
var state = Header(2, BenchDirection.ServerToClient);
metrics.OnSlot(input, true);
metrics.OnSlot(state, true);
metrics.OnRecv(0, input, input.SchedTsNs + 50);
metrics.OnRecv(0, state, state.SchedTsNs + 150);

using var document = JsonDocument.Parse(metrics.ToJson());
var root = document.RootElement;
var aggregate = root.GetProperty("classes").GetProperty("must_deliver");
var traffic = root.GetProperty("traffic").EnumerateArray()
    .Where(item => item.GetProperty("class").GetString() == "must_deliver")
    .ToArray();

foreach (var field in new[]
{
    "slots", "slots_broadcast", "submitted", "delivered_unique", "duplicates",
    "deadline_hit", "expected_flows", "observed_flows", "never_received_flows",
})
{
    var expected = traffic.Aggregate(0UL, (sum, item) => sum + item.GetProperty(field).GetUInt64());
    var actual = aggregate.GetProperty(field).GetUInt64();
    Require(actual == expected, $"{field}: class={actual}, traffic sum={expected}");
}

foreach (var field in new[] { "latency_sched_ns", "latency_send_ns", "update_gap_ns" })
{
    var aggregateHistogram = aggregate.GetProperty(field);
    var expectedCount = traffic.Aggregate(0UL,
        (sum, item) => sum + item.GetProperty(field).GetProperty("count").GetUInt64());
    Require(aggregateHistogram.GetProperty("count").GetUInt64() == expectedCount,
        $"{field}.count differs from traffic sum");

    var aggregateBins = aggregateHistogram.GetProperty("bins").EnumerateArray()
        .Select(value => value.GetUInt64()).ToArray();
    for (var i = 0; i < aggregateBins.Length; i++)
    {
        var index = i;
        var expected = traffic.Aggregate(0UL, (sum, item) =>
            sum + item.GetProperty(field).GetProperty("bins")[index].GetUInt64());
        Require(aggregateBins[i] == expected,
            $"{field}.bins[{i}]: class={aggregateBins[i]}, traffic sum={expected}");
    }
}

Require(aggregate.GetProperty("deadline_hit").GetUInt64() == 2,
    "traffic-specific deadlines were not applied to the class aggregate");
Require(root.GetProperty("raw").GetProperty("timestamp_order_violations").GetUInt64() == 0,
    "valid timestamps were reported as order violations");

var orderMetrics = new BenchMetrics(new BenchMetricsConfig(
    MaxOriginId: 1,
    DeadlineNs: 1,
    StalenessPeriodNs: BenchConstants.DefaultStalenessPeriodNs,
    MaxLocalIndex: 1));
var invalidOrder = Header(3, BenchDirection.ClientToServer) with
{
    Seq = 10,
    SchedTsNs = 300,
    SendTsNs = 200,
};
orderMetrics.OnRecv(0, invalidOrder, 400);
orderMetrics.OnRecv(0, invalidOrder, 400);
orderMetrics.OnRecv(0, invalidOrder with
{
    Seq = 11,
    SchedTsNs = 100,
    SendTsNs = 300,
}, 200);
orderMetrics.OnRecv(0, invalidOrder with { Seq = 12 }, 100);
Require(orderMetrics.RawCounts().TimestampOrderViolations == 3,
    "timestamp order violations must count measured unique receives once");
using var orderDocument = JsonDocument.Parse(orderMetrics.ToJson());
Require(orderDocument.RootElement.GetProperty("raw")
        .GetProperty("timestamp_order_violations").GetUInt64() == 3,
    "timestamp order violations were not serialized");

metrics.ExpectLatest(0, 0, 1, BenchDirection.ClientToServer, 1_000);
metrics.Tick(2_000);
metrics.Reset();
using (var resetDocument = JsonDocument.Parse(metrics.ToJson()))
{
    var resetRoot = resetDocument.RootElement;
    Require(resetRoot.GetProperty("raw").GetProperty("slots").GetUInt64() == 0,
        "reset did not clear raw slots");
    Require(resetRoot.GetProperty("staleness_ns").GetProperty("count").GetUInt64() == 0,
        "reset did not clear staleness");
    foreach (var item in resetRoot.GetProperty("traffic").EnumerateArray())
    {
        Require(item.GetProperty("slots").GetUInt64() == 0 &&
                item.GetProperty("delivered_unique").GetUInt64() == 0 &&
                item.GetProperty("expected_flows").GetUInt64() == 0,
            "reset did not clear traffic observation state");
        var trafficId = item.GetProperty("traffic_id").GetByte();
        Require(item.GetProperty("deadline_ns").GetUInt64() == (trafficId == 1 ? 100UL : 200UL),
            "reset did not preserve traffic deadline");
    }
}
metrics.OnSlot(input, true);
metrics.OnRecv(0, input, input.SchedTsNs + 50);
Require(metrics.Counts(mustDeliver: true).Slots == 1 &&
        metrics.Counts(mustDeliver: true).DeliveredUnique == 1,
    "metrics did not accept observations after reset");

var fixedWindowMetrics = new BenchMetrics(new BenchMetricsConfig(1, 100, 10, 1));
fixedWindowMetrics.SetTrafficDeadline(1, BenchDirection.ClientToServer, 100);
var zeroSched = Header(1, BenchDirection.ClientToServer) with { SchedTsNs = 0, SendTsNs = 0 };
var zeroSchedUnmeasured = zeroSched with
{
    Seq = 2,
    Flags = (byte)BenchConstants.DirectionFlags(BenchDirection.ClientToServer),
};
fixedWindowMetrics.OnSlot(zeroSched, true);
fixedWindowMetrics.OnRecv(0, zeroSched, 1);
fixedWindowMetrics.OnRecv(0, zeroSchedUnmeasured, 1);
Require(fixedWindowMetrics.Counts(mustDeliver: true).Slots == 1 &&
        fixedWindowMetrics.Counts(mustDeliver: true).DeliveredUnique == 1 &&
        fixedWindowMetrics.RawCounts().RecvUnmeasured == 1,
    "disabled observation window rejected sched_ts=0 fixed traffic");

var rampNames = new[]
{
    "BENCH_RAMP_START_CONNS",
    "BENCH_RAMP_STEP_CONNS",
    "BENCH_RAMP_GUARD_NS",
    "BENCH_RAMP_SAMPLE_NS",
    "BENCH_RAMP_DRAIN_NS",
};
var savedRampEnvironment = rampNames.ToDictionary(name => name, Environment.GetEnvironmentVariable);
try
{
    foreach (var name in rampNames)
    {
        Environment.SetEnvironmentVariable(name, null);
    }
    Require(BenchRampConfig.FromEnvironment(10) is null, "empty ramp environment enabled ramp mode");
    Environment.SetEnvironmentVariable(rampNames[0], "1");
    RequireThrows(() => BenchRampConfig.FromEnvironment(10), "partial ramp environment was accepted");
    Environment.SetEnvironmentVariable(rampNames[1], "4");
    Environment.SetEnvironmentVariable(rampNames[2], "100");
    Environment.SetEnvironmentVariable(rampNames[3], "200");
    Environment.SetEnvironmentVariable(rampNames[4], "0");
    var ramp = BenchRampConfig.FromEnvironment(10) ?? throw new InvalidOperationException("ramp was not enabled");
    Require(ramp.Targets(10).SequenceEqual(new[] { 1, 5, 9, 10 }), "ramp targets differ");
    Require(ramp.RequiredWindowNs(10) == 1_200, "ramp derived window differs");
    Require(ramp.PhaseStartNs(1_000, 3) == 1_900, "ramp absolute phase start differs");
    Require(BenchRampConfig.SnapshotPath("/tmp/m", 2, 9) == "/tmp/m.ramp-000002-c000009.json",
        "ramp snapshot path differs");
}
finally
{
    foreach (var pair in savedRampEnvironment)
    {
        Environment.SetEnvironmentVariable(pair.Key, pair.Value);
    }
}

var cohortMetrics = new BenchMetrics(new BenchMetricsConfig(1, 100, 10, 1));
cohortMetrics.SetTrafficDeadline(1, BenchDirection.ClientToServer, 100);
cohortMetrics.Reset();
cohortMetrics.SetObservationWindow(1_000, 2_000);
var beforeCohort = Header(1, BenchDirection.ClientToServer) with { SchedTsNs = 999, SendTsNs = 999 };
var startBoundary = beforeCohort with { Seq = 2, SchedTsNs = 1_000, SendTsNs = 1_000 };
var inCohort = beforeCohort with { Seq = 3, SchedTsNs = 1_500, SendTsNs = 1_500 };
var stopBoundary = beforeCohort with { Seq = 4, SchedTsNs = 2_000, SendTsNs = 2_000 };
var afterCohort = beforeCohort with { Seq = 5, SchedTsNs = 2_001, SendTsNs = 2_001 };
cohortMetrics.OnSlot(beforeCohort, true);
cohortMetrics.OnRecv(0, beforeCohort, 1_600);
cohortMetrics.OnSlot(startBoundary, true);
cohortMetrics.OnRecv(0, startBoundary, 1_600);
cohortMetrics.OnSlot(inCohort, true);
cohortMetrics.OnRecv(0, inCohort, 2_050);
cohortMetrics.OnSlot(stopBoundary, true);
cohortMetrics.OnRecv(0, stopBoundary, 2_100);
cohortMetrics.OnSlot(afterCohort, true);
cohortMetrics.OnRecv(0, afterCohort, 2_100);
Require(cohortMetrics.Counts(mustDeliver: true).Slots == 2 &&
        cohortMetrics.Counts(mustDeliver: true).DeliveredUnique == 2,
    "observation window did not isolate the (start, stop] scheduled cohort");

var stalenessWindowMetrics = new BenchMetrics(new BenchMetricsConfig(1, 100, 10, 1));
stalenessWindowMetrics.ExpectLatest(0, 0, 1, BenchDirection.ClientToServer, 1_000);
stalenessWindowMetrics.SetObservationWindow(1_000, 1_020);
stalenessWindowMetrics.Tick(999);
stalenessWindowMetrics.Tick(1_000);
stalenessWindowMetrics.Tick(1_020);
using (var stalenessWindowDocument = JsonDocument.Parse(stalenessWindowMetrics.ToJson()))
{
    Require(stalenessWindowDocument.RootElement.GetProperty("staleness_ns")
            .GetProperty("count").GetUInt64() == 2,
        "staleness observation window was not [start, stop)");
}

static BenchHeader Header(byte trafficId, BenchDirection direction) => new(
    Seq: 1,
    SchedTsNs: 1_000,
    SendTsNs: 1_000,
    Flags: (byte)(BenchConstants.FlagMeasure | BenchConstants.FlagMustDeliver |
        BenchConstants.DirectionFlags(direction)),
    OriginId: 0,
    TrafficId: trafficId);

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

static void RequireThrows(Action action, string message)
{
    try
    {
        action();
    }
    catch (InvalidOperationException)
    {
        return;
    }
    throw new InvalidOperationException(message);
}
