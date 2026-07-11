using System.Text.Json;
using BenchKit.CS;

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
