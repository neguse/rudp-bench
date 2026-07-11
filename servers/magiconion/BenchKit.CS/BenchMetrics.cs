using System.Globalization;
using System.Text;

namespace BenchKit.CS;

public readonly record struct BenchMetricsConfig(
    uint MaxOriginId,
    ulong DeadlineNs,
    ulong StalenessPeriodNs,
    uint MaxLocalIndex = 0);

public struct BenchClassCounts
{
    public ulong Slots;
    public ulong SlotsBroadcast;
    public ulong Submitted;
    public ulong DeliveredUnique;
    public ulong Duplicates;
    public ulong DeadlineHit;
    public ulong ExpectedFlows;
    public ulong ObservedFlows;
    public ulong NeverReceivedFlows;
}

public readonly record struct BenchRawCounts(
    ulong Slots,
    ulong Submitted,
    ulong RecvMeasured,
    ulong RecvUnmeasured,
    ulong TimestampOrderViolations);

public sealed class BenchMetrics
{
    private sealed class Hist
    {
        public ulong Count;
        public readonly ulong[] Bins = new ulong[BenchConstants.HistBins];

        public void Add(ulong valueNs)
        {
            Bins[HistIndex(valueNs)]++;
            Count++;
        }

        public ulong Percentile(double p)
        {
            if (Count == 0)
            {
                return 0;
            }

            var exact = Count * Math.Clamp(p, 0.0, 1.0);
            var rank = (ulong)exact;
            if (rank < exact)
            {
                rank++;
            }
            rank = Math.Max(rank, 1UL);

            ulong cumulative = 0;
            for (var i = 0; i < Bins.Length; i++)
            {
                cumulative += Bins[i];
                if (cumulative >= rank)
                {
                    return HistBinUpperNs(i);
                }
            }
            return BenchConstants.HistMaxNs;
        }
    }

    private sealed class Latest
    {
        public bool Expected;
        public bool Has;
        public ulong SchedTsNs;
        public ulong RecvTsNs;
    }

    private readonly record struct FlowKey(
        uint LocalIndex,
        uint OriginId,
        byte TrafficId,
        BenchDirection Direction,
        byte ClassIndex);

    private readonly record struct SeenKey(
        uint LocalIndex,
        uint OriginId,
        byte TrafficId,
        BenchDirection Direction,
        byte ClassIndex,
        ulong Seq);

    private readonly record struct TrafficKey(
        byte TrafficId,
        BenchDirection Direction,
        byte ClassIndex);

    private readonly record struct DeadlineKey(byte TrafficId, BenchDirection Direction);

    private class ClassMetrics
    {
        public BenchClassCounts Counts;
        public readonly Hist LatencySched = new();
        public readonly Hist LatencySend = new();
        public readonly Hist UpdateGap = new();
    }

    private sealed class TrafficMetrics : ClassMetrics
    {
        public ulong DeadlineNs;
        public readonly Hist Staleness = new();
    }

    private readonly BenchMetricsConfig config;
    private readonly bool legacySingleLatest;
    private readonly ClassMetrics[] classes = [new ClassMetrics(), new ClassMetrics()];
    private readonly Hist staleness = new();
    private readonly Dictionary<FlowKey, Latest> latest = [];
    private readonly HashSet<SeenKey> seen = [];
    private readonly Dictionary<TrafficKey, TrafficMetrics> traffic = [];
    private readonly Dictionary<DeadlineKey, ulong> deadlines = [];
    private ulong nextStalenessSampleNs;
    private ulong rawSlots;
    private ulong rawSubmitted;
    private ulong rawRecvMeasured;
    private ulong rawRecvUnmeasured;
    private ulong rawTimestampOrderViolations;

    public BenchMetrics(BenchMetricsConfig config)
    {
        var maxOriginId = config.MaxOriginId == 0 ? 1 : config.MaxOriginId;
        legacySingleLatest = config.MaxLocalIndex == 0;
        var maxLocalIndex = legacySingleLatest ? 1U : config.MaxLocalIndex;
        var stalenessPeriodNs = config.StalenessPeriodNs == 0
            ? BenchConstants.DefaultStalenessPeriodNs
            : config.StalenessPeriodNs;
        this.config = config with
        {
            MaxOriginId = maxOriginId,
            MaxLocalIndex = maxLocalIndex,
            StalenessPeriodNs = stalenessPeriodNs,
        };
    }

    public void SetTrafficDeadline(byte trafficId, BenchDirection direction, ulong deadlineNs)
    {
        ValidateDirection(direction);
        deadlines[new DeadlineKey(trafficId, direction)] = deadlineNs;
        foreach (var item in traffic)
        {
            if (item.Key.TrafficId == trafficId && item.Key.Direction == direction)
            {
                item.Value.DeadlineNs = deadlineNs;
            }
        }
    }

    public void EnsureTraffic(byte trafficId, BenchDirection direction, bool mustDeliver)
    {
        ValidateDirection(direction);
        GetTraffic(new TrafficKey(
            trafficId,
            direction,
            (byte)(mustDeliver ? BenchConstants.ClassMustDeliver : BenchConstants.ClassLossTolerant)));
    }

    public bool ExpectLatest(
        uint localIndex,
        uint originId,
        byte trafficId,
        BenchDirection direction,
        ulong firstSchedTsNs)
    {
        ValidateDirection(direction);
        if (!TryNormalizeFlow(ref localIndex, originId))
        {
            return false;
        }

        var key = new FlowKey(
            localIndex,
            originId,
            trafficId,
            direction,
            BenchConstants.ClassLossTolerant);
        var item = GetLatest(key);
        var tm = GetTraffic(new TrafficKey(
            trafficId,
            direction,
            BenchConstants.ClassLossTolerant));
        if (!item.Expected)
        {
            classes[BenchConstants.ClassLossTolerant].Counts.ExpectedFlows++;
            tm.Counts.ExpectedFlows++;
        }
        if (!item.Has && (!item.Expected || firstSchedTsNs < item.SchedTsNs))
        {
            item.SchedTsNs = firstSchedTsNs;
        }
        item.Expected = true;
        return true;
    }

    public void OnSlot(BenchHeader header, bool submitted)
    {
        rawSlots++;
        if (submitted)
        {
            rawSubmitted++;
        }
        if ((header.Flags & BenchConstants.FlagMeasure) == 0)
        {
            return;
        }

        var direction = BenchConstants.DirectionFromFlags(header.Flags);
        if (direction > BenchDirection.ServerToClient)
        {
            return;
        }
        var classIndex = (byte)BenchConstants.ClassIndexFromFlags(header.Flags);
        var aggregate = classes[classIndex];
        var tm = GetTraffic(new TrafficKey(header.TrafficId, direction, classIndex));
        aggregate.Counts.Slots++;
        tm.Counts.Slots++;
        if ((header.Flags & BenchConstants.FlagBroadcast) != 0)
        {
            aggregate.Counts.SlotsBroadcast++;
            tm.Counts.SlotsBroadcast++;
        }
        if (submitted)
        {
            aggregate.Counts.Submitted++;
            tm.Counts.Submitted++;
        }
    }

    public void OnRecv(uint localIndex, BenchHeader header, ulong recvTsNs)
    {
        if ((header.Flags & BenchConstants.FlagMeasure) == 0)
        {
            rawRecvUnmeasured++;
            return;
        }

        rawRecvMeasured++;
        var direction = BenchConstants.DirectionFromFlags(header.Flags);
        if (direction > BenchDirection.ServerToClient)
        {
            return;
        }
        var classIndex = (byte)BenchConstants.ClassIndexFromFlags(header.Flags);
        var aggregate = classes[classIndex];
        var trafficKey = new TrafficKey(header.TrafficId, direction, classIndex);
        var tm = GetTraffic(trafficKey);
        if (!seen.Add(new SeenKey(
                localIndex,
                header.OriginId,
                header.TrafficId,
                direction,
                classIndex,
                header.Seq)))
        {
            aggregate.Counts.Duplicates++;
            tm.Counts.Duplicates++;
            return;
        }

        aggregate.Counts.DeliveredUnique++;
        tm.Counts.DeliveredUnique++;
        if (header.SchedTsNs > header.SendTsNs || header.SendTsNs > recvTsNs)
        {
            rawTimestampOrderViolations++;
        }
        var schedLatency = BenchClock.SaturatingSub(recvTsNs, header.SchedTsNs);
        var sendLatency = BenchClock.SaturatingSub(recvTsNs, header.SendTsNs);
        aggregate.LatencySched.Add(schedLatency);
        aggregate.LatencySend.Add(sendLatency);
        tm.LatencySched.Add(schedLatency);
        tm.LatencySend.Add(sendLatency);
        if (classIndex == BenchConstants.ClassMustDeliver)
        {
            if (schedLatency <= tm.DeadlineNs)
            {
                aggregate.Counts.DeadlineHit++;
                tm.Counts.DeadlineHit++;
            }
        }

        if (!TryNormalizeFlow(ref localIndex, header.OriginId))
        {
            return;
        }
        var flowKey = new FlowKey(
            localIndex,
            header.OriginId,
            header.TrafficId,
            direction,
            classIndex);
        var item = GetLatest(flowKey);
        if (!item.Expected)
        {
            aggregate.Counts.ExpectedFlows++;
            tm.Counts.ExpectedFlows++;
        }
        if (!item.Has)
        {
            aggregate.Counts.ObservedFlows++;
            tm.Counts.ObservedFlows++;
        }
        if (!item.Has || header.SchedTsNs > item.SchedTsNs)
        {
            if (item.Has)
            {
                var gap = BenchClock.SaturatingSub(recvTsNs, item.RecvTsNs);
                aggregate.UpdateGap.Add(gap);
                tm.UpdateGap.Add(gap);
            }
            item.Expected = true;
            item.Has = true;
            item.SchedTsNs = header.SchedTsNs;
            item.RecvTsNs = recvTsNs;
        }
    }

    public void Tick(ulong nowNs)
    {
        if (nextStalenessSampleNs == 0)
        {
            nextStalenessSampleNs = nowNs;
        }
        while (nextStalenessSampleNs <= nowNs)
        {
            var sampleNs = nextStalenessSampleNs;
            foreach (var pair in latest)
            {
                if (pair.Key.ClassIndex != BenchConstants.ClassLossTolerant ||
                    (!pair.Value.Expected && !pair.Value.Has))
                {
                    continue;
                }
                var age = BenchClock.SaturatingSub(sampleNs, pair.Value.SchedTsNs);
                staleness.Add(age);
                GetTraffic(new TrafficKey(
                    pair.Key.TrafficId,
                    pair.Key.Direction,
                    BenchConstants.ClassLossTolerant)).Staleness.Add(age);
            }

            if (ulong.MaxValue - nextStalenessSampleNs < config.StalenessPeriodNs)
            {
                nextStalenessSampleNs = ulong.MaxValue;
                break;
            }
            nextStalenessSampleNs += config.StalenessPeriodNs;
        }
    }

    public BenchRawCounts RawCounts() =>
        new(rawSlots, rawSubmitted, rawRecvMeasured, rawRecvUnmeasured, rawTimestampOrderViolations);

    public BenchClassCounts Counts(bool mustDeliver) => FinalizedCounts(
        classes[mustDeliver ? BenchConstants.ClassMustDeliver : BenchConstants.ClassLossTolerant].Counts);

    public string ToJson()
    {
        var sb = new StringBuilder(32_768);
        sb.Append("{\"version\":2,\"histogram\":{\"scheme\":\"log2x16\",\"subbins\":")
            .Append(BenchConstants.HistSubBins.ToString(CultureInfo.InvariantCulture))
            .Append(",\"min_ns\":").Append(BenchConstants.HistMinNs.ToString(CultureInfo.InvariantCulture))
            .Append(",\"max_ns\":").Append(BenchConstants.HistMaxNs.ToString(CultureInfo.InvariantCulture))
            .Append("},\"classes\":{\"loss_tolerant\":");
        AppendClassJson(sb, classes[BenchConstants.ClassLossTolerant]);
        sb.Append(",\"must_deliver\":");
        AppendClassJson(sb, classes[BenchConstants.ClassMustDeliver]);
        sb.Append("},\"traffic\":[");
        var orderedTraffic = traffic.OrderBy(pair => pair.Key.TrafficId)
            .ThenBy(pair => pair.Key.Direction)
            .ThenBy(pair => pair.Key.ClassIndex);
        var first = true;
        foreach (var pair in orderedTraffic)
        {
            if (!first)
            {
                sb.Append(',');
            }
            AppendTrafficJson(sb, pair.Key, pair.Value);
            first = false;
        }
        sb.Append("],\"staleness_ns\":");
        AppendHistJson(sb, staleness);
        sb.Append(",\"raw\":{\"slots\":").Append(rawSlots.ToString(CultureInfo.InvariantCulture))
            .Append(",\"submitted\":").Append(rawSubmitted.ToString(CultureInfo.InvariantCulture))
            .Append(",\"recv_measured\":").Append(rawRecvMeasured.ToString(CultureInfo.InvariantCulture))
            .Append(",\"recv_unmeasured\":").Append(rawRecvUnmeasured.ToString(CultureInfo.InvariantCulture))
            .Append(",\"timestamp_order_violations\":").Append(rawTimestampOrderViolations.ToString(CultureInfo.InvariantCulture))
            .Append("}}");
        return sb.ToString();
    }

    public void DumpJson(string path) => File.WriteAllText(path, ToJson() + "\n", new UTF8Encoding(false));

    private bool TryNormalizeFlow(ref uint localIndex, uint originId)
    {
        if (originId >= config.MaxOriginId)
        {
            return false;
        }
        if (legacySingleLatest)
        {
            localIndex = 0;
            return true;
        }
        return localIndex < config.MaxLocalIndex;
    }

    private Latest GetLatest(FlowKey key)
    {
        if (!latest.TryGetValue(key, out var item))
        {
            item = new Latest();
            latest.Add(key, item);
        }
        return item;
    }

    private TrafficMetrics GetTraffic(TrafficKey key)
    {
        if (!traffic.TryGetValue(key, out var item))
        {
            var deadlineKey = new DeadlineKey(key.TrafficId, key.Direction);
            var hasDeadline = deadlines.TryGetValue(deadlineKey, out var deadlineNs);
            item = new TrafficMetrics
            {
                DeadlineNs = hasDeadline ? deadlineNs : config.DeadlineNs,
            };
            traffic.Add(key, item);
        }
        return item;
    }

    private static BenchClassCounts FinalizedCounts(BenchClassCounts counts)
    {
        counts.NeverReceivedFlows = counts.ExpectedFlows >= counts.ObservedFlows
            ? counts.ExpectedFlows - counts.ObservedFlows
            : 0;
        return counts;
    }

    private static void AppendClassJson(StringBuilder sb, ClassMetrics cm)
    {
        AppendCountsJson(sb, FinalizedCounts(cm.Counts));
        sb.Append(",\"latency_sched_ns\":");
        AppendHistJson(sb, cm.LatencySched);
        sb.Append(",\"latency_send_ns\":");
        AppendHistJson(sb, cm.LatencySend);
        sb.Append(",\"update_gap_ns\":");
        AppendHistJson(sb, cm.UpdateGap);
        sb.Append('}');
    }

    private static void AppendTrafficJson(StringBuilder sb, TrafficKey key, TrafficMetrics tm)
    {
        sb.Append("{\"traffic_id\":").Append(key.TrafficId.ToString(CultureInfo.InvariantCulture))
            .Append(",\"direction\":\"").Append(DirectionName(key.Direction))
            .Append("\",\"class\":\"")
            .Append(key.ClassIndex == BenchConstants.ClassMustDeliver ? "must_deliver" : "loss_tolerant")
            .Append("\",\"deadline_ns\":").Append(tm.DeadlineNs.ToString(CultureInfo.InvariantCulture));
        AppendCountsFieldsJson(sb, FinalizedCounts(tm.Counts));
        sb.Append(",\"latency_sched_ns\":");
        AppendHistJson(sb, tm.LatencySched);
        sb.Append(",\"latency_send_ns\":");
        AppendHistJson(sb, tm.LatencySend);
        sb.Append(",\"update_gap_ns\":");
        AppendHistJson(sb, tm.UpdateGap);
        sb.Append(",\"staleness_ns\":");
        AppendHistJson(sb, tm.Staleness);
        sb.Append('}');
    }

    private static void AppendCountsJson(StringBuilder sb, BenchClassCounts counts)
    {
        sb.Append('{');
        AppendCountsFieldsJson(sb, counts, false);
    }

    private static void AppendCountsFieldsJson(
        StringBuilder sb,
        BenchClassCounts counts,
        bool leadingComma = true)
    {
        if (leadingComma)
        {
            sb.Append(',');
        }
        sb.Append("\"slots\":").Append(counts.Slots.ToString(CultureInfo.InvariantCulture))
            .Append(",\"slots_broadcast\":").Append(counts.SlotsBroadcast.ToString(CultureInfo.InvariantCulture))
            .Append(",\"submitted\":").Append(counts.Submitted.ToString(CultureInfo.InvariantCulture))
            .Append(",\"delivered_unique\":").Append(counts.DeliveredUnique.ToString(CultureInfo.InvariantCulture))
            .Append(",\"duplicates\":").Append(counts.Duplicates.ToString(CultureInfo.InvariantCulture))
            .Append(",\"deadline_hit\":").Append(counts.DeadlineHit.ToString(CultureInfo.InvariantCulture))
            .Append(",\"expected_flows\":").Append(counts.ExpectedFlows.ToString(CultureInfo.InvariantCulture))
            .Append(",\"observed_flows\":").Append(counts.ObservedFlows.ToString(CultureInfo.InvariantCulture))
            .Append(",\"never_received_flows\":").Append(counts.NeverReceivedFlows.ToString(CultureInfo.InvariantCulture));
    }

    private static void AppendHistJson(StringBuilder sb, Hist h)
    {
        sb.Append("{\"scheme\":\"log2x16\",\"min_ns\":")
            .Append(BenchConstants.HistMinNs.ToString(CultureInfo.InvariantCulture))
            .Append(",\"max_ns\":").Append(BenchConstants.HistMaxNs.ToString(CultureInfo.InvariantCulture))
            .Append(",\"count\":").Append(h.Count.ToString(CultureInfo.InvariantCulture))
            .Append(",\"p50_ns\":").Append(h.Percentile(0.50).ToString(CultureInfo.InvariantCulture))
            .Append(",\"p90_ns\":").Append(h.Percentile(0.90).ToString(CultureInfo.InvariantCulture))
            .Append(",\"p99_ns\":").Append(h.Percentile(0.99).ToString(CultureInfo.InvariantCulture))
            .Append(",\"bins\":[");
        for (var i = 0; i < h.Bins.Length; i++)
        {
            if (i != 0)
            {
                sb.Append(',');
            }
            sb.Append(h.Bins[i].ToString(CultureInfo.InvariantCulture));
        }
        sb.Append("]}");
    }

    private static string DirectionName(BenchDirection direction) => direction switch
    {
        BenchDirection.RoomRelay => "room_relay",
        BenchDirection.ClientToServer => "client_to_server",
        BenchDirection.ServerToClient => "server_to_client",
        _ => "invalid",
    };

    private static void ValidateDirection(BenchDirection direction)
    {
        if (direction > BenchDirection.ServerToClient)
        {
            throw new ArgumentOutOfRangeException(nameof(direction));
        }
    }

    private static int HistIndex(ulong valueNs)
    {
        if (valueNs <= BenchConstants.HistMinNs)
        {
            return 0;
        }
        if (valueNs >= BenchConstants.HistMaxNs)
        {
            return BenchConstants.HistBins - 1;
        }

        var low = BenchConstants.HistMinNs;
        var major = 0;
        while (major + 1 < BenchConstants.HistMajorBins && valueNs >= low * 2UL)
        {
            low *= 2UL;
            major++;
        }
        var offset = valueNs - low;
        var sub = (int)((offset * BenchConstants.HistSubBins) / low);
        if (sub >= BenchConstants.HistSubBins)
        {
            sub = BenchConstants.HistSubBins - 1;
        }
        return major * BenchConstants.HistSubBins + sub;
    }

    private static ulong HistBinUpperNs(int index)
    {
        if (index == 0)
        {
            return BenchConstants.HistMinNs;
        }
        if (index >= BenchConstants.HistBins - 1)
        {
            return BenchConstants.HistMaxNs;
        }

        var major = index / BenchConstants.HistSubBins;
        var sub = index % BenchConstants.HistSubBins;
        var low = BenchConstants.HistMinNs << major;
        var upper = low + ((low * (ulong)(sub + 1) + BenchConstants.HistSubBins - 1) /
                           BenchConstants.HistSubBins);
        return upper > BenchConstants.HistMaxNs ? BenchConstants.HistMaxNs : upper;
    }
}
