using System.Globalization;
using System.Text;

namespace BenchKit.CS;

public readonly record struct BenchMetricsConfig(uint MaxOriginId, ulong DeadlineNs, ulong StalenessPeriodNs);

public struct BenchClassCounts
{
    public ulong Slots;
    public ulong SlotsBroadcast;
    public ulong Submitted;
    public ulong DeliveredUnique;
    public ulong Duplicates;
    public ulong DeadlineHit;
}

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

            var q = Math.Clamp(p, 0.0, 1.0);
            var exact = Count * q;
            var rank = (ulong)exact;
            if (rank < exact)
            {
                rank++;
            }
            if (rank == 0)
            {
                rank = 1;
            }

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

    private struct Latest
    {
        public bool Has;
        public ulong SchedTsNs;
    }

    private readonly record struct SeenKey(uint LocalIndex, uint OriginId, byte ClassIndex, ulong Seq);

    private sealed class ClassMetrics
    {
        public BenchClassCounts Counts;
        public readonly Hist LatencySched = new();
        public readonly Hist LatencySend = new();
    }

    private readonly BenchMetricsConfig config;
    private readonly ClassMetrics[] classes = [new ClassMetrics(), new ClassMetrics()];
    private readonly Hist staleness = new();
    private readonly Latest[] latest;
    private readonly HashSet<SeenKey> seen = [];
    private ulong nextStalenessSampleNs;
    private ulong rawSlots;
    private ulong rawSubmitted;
    private ulong rawRecvMeasured;
    private ulong rawRecvUnmeasured;

    public BenchMetrics(BenchMetricsConfig config)
    {
        var maxOriginId = config.MaxOriginId == 0 ? 1 : config.MaxOriginId;
        var stalenessPeriodNs = config.StalenessPeriodNs == 0
            ? BenchConstants.DefaultStalenessPeriodNs
            : config.StalenessPeriodNs;
        this.config = config with
        {
            MaxOriginId = maxOriginId,
            StalenessPeriodNs = stalenessPeriodNs
        };

        latest = new Latest[checked((int)maxOriginId * BenchConstants.ClassCount)];
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

        var classIndex = BenchConstants.ClassIndexFromFlags(header.Flags);
        classes[classIndex].Counts.Slots++;
        if ((header.Flags & BenchConstants.FlagBroadcast) != 0)
        {
            classes[classIndex].Counts.SlotsBroadcast++;
        }
        if (submitted)
        {
            classes[classIndex].Counts.Submitted++;
        }
    }

    // localIndex は受信した自 proc 内 conn の 0 起点 index(benchspec: 重複判定は
    // (受信側 local conn, origin, class, seq) — broadcast の複製を duplicate にしない)
    public void OnRecv(uint localIndex, BenchHeader header, ulong recvTsNs)
    {
        if ((header.Flags & BenchConstants.FlagMeasure) == 0)
        {
            rawRecvUnmeasured++;
            return;
        }

        rawRecvMeasured++;
        var classIndex = (byte)BenchConstants.ClassIndexFromFlags(header.Flags);
        if (!seen.Add(new SeenKey(localIndex, header.OriginId, classIndex, header.Seq)))
        {
            classes[classIndex].Counts.Duplicates++;
            return;
        }

        var cm = classes[classIndex];
        cm.Counts.DeliveredUnique++;
        var schedLatency = BenchClock.SaturatingSub(recvTsNs, header.SchedTsNs);
        var sendLatency = BenchClock.SaturatingSub(recvTsNs, header.SendTsNs);
        cm.LatencySched.Add(schedLatency);
        cm.LatencySend.Add(sendLatency);

        if (classIndex == BenchConstants.ClassMustDeliver && schedLatency <= config.DeadlineNs)
        {
            cm.Counts.DeadlineHit++;
        }

        if (header.OriginId < config.MaxOriginId)
        {
            var index = checked((int)header.OriginId * BenchConstants.ClassCount + classIndex);
            if (!latest[index].Has || header.SchedTsNs > latest[index].SchedTsNs)
            {
                latest[index].Has = true;
                latest[index].SchedTsNs = header.SchedTsNs;
            }
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
            for (uint origin = 0; origin < config.MaxOriginId; origin++)
            {
                var item = latest[(int)origin * BenchConstants.ClassCount + BenchConstants.ClassLossTolerant];
                if (item.Has)
                {
                    staleness.Add(BenchClock.SaturatingSub(sampleNs, item.SchedTsNs));
                }
            }

            if (ulong.MaxValue - nextStalenessSampleNs < config.StalenessPeriodNs)
            {
                nextStalenessSampleNs = ulong.MaxValue;
                break;
            }

            nextStalenessSampleNs += config.StalenessPeriodNs;
        }
    }

    public BenchClassCounts Counts(bool mustDeliver) =>
        classes[mustDeliver ? BenchConstants.ClassMustDeliver : BenchConstants.ClassLossTolerant].Counts;

    public string ToJson()
    {
        var sb = new StringBuilder(16_384);
        sb.Append("{\"version\":1,\"histogram\":{\"scheme\":\"log2x16\",\"subbins\":")
            .Append(BenchConstants.HistSubBins.ToString(CultureInfo.InvariantCulture))
            .Append(",\"min_ns\":").Append(BenchConstants.HistMinNs.ToString(CultureInfo.InvariantCulture))
            .Append(",\"max_ns\":").Append(BenchConstants.HistMaxNs.ToString(CultureInfo.InvariantCulture))
            .Append("},\"classes\":{\"loss_tolerant\":");
        AppendClassJson(sb, classes[BenchConstants.ClassLossTolerant]);
        sb.Append(",\"must_deliver\":");
        AppendClassJson(sb, classes[BenchConstants.ClassMustDeliver]);
        sb.Append("},\"staleness_ns\":");
        AppendHistJson(sb, staleness);
        sb.Append(",\"raw\":{\"slots\":").Append(rawSlots.ToString(CultureInfo.InvariantCulture))
            .Append(",\"submitted\":").Append(rawSubmitted.ToString(CultureInfo.InvariantCulture))
            .Append(",\"recv_measured\":").Append(rawRecvMeasured.ToString(CultureInfo.InvariantCulture))
            .Append(",\"recv_unmeasured\":").Append(rawRecvUnmeasured.ToString(CultureInfo.InvariantCulture))
            .Append("}}");
        return sb.ToString();
    }

    public void DumpJson(string path) => File.WriteAllText(path, ToJson() + "\n", new UTF8Encoding(false));

    private static void AppendClassJson(StringBuilder sb, ClassMetrics cm)
    {
        var c = cm.Counts;
        sb.Append("{\"slots\":").Append(c.Slots.ToString(CultureInfo.InvariantCulture))
            .Append(",\"slots_broadcast\":").Append(c.SlotsBroadcast.ToString(CultureInfo.InvariantCulture))
            .Append(",\"submitted\":").Append(c.Submitted.ToString(CultureInfo.InvariantCulture))
            .Append(",\"delivered_unique\":").Append(c.DeliveredUnique.ToString(CultureInfo.InvariantCulture))
            .Append(",\"duplicates\":").Append(c.Duplicates.ToString(CultureInfo.InvariantCulture))
            .Append(",\"deadline_hit\":").Append(c.DeadlineHit.ToString(CultureInfo.InvariantCulture))
            .Append(",\"latency_sched_ns\":");
        AppendHistJson(sb, cm.LatencySched);
        sb.Append(",\"latency_send_ns\":");
        AppendHistJson(sb, cm.LatencySend);
        sb.Append('}');
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
