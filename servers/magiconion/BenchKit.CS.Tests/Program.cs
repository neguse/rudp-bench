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
