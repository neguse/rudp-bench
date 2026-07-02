namespace BenchKit.CS;

public readonly record struct BenchStream(bool MustDeliver, bool Broadcast, ulong IntervalNs);

public readonly record struct BenchSlot(ulong SchedTsNs, ulong Seq, int StreamIndex, byte Flags);

public sealed class BenchPlan
{
    private readonly BenchStream[] streams;
    private readonly ulong[] nextSchedNs;
    private readonly ulong[] nextSeq;
    private readonly ulong measureStartNs;
    private readonly ulong measureStopNs;

    public BenchPlan(IReadOnlyList<BenchStream> streams, ulong startNs, ulong measureStartNs, ulong measureStopNs)
    {
        if (streams.Count == 0)
        {
            throw new ArgumentException("at least one stream is required", nameof(streams));
        }

        this.streams = streams.ToArray();
        foreach (var stream in this.streams)
        {
            if (stream.IntervalNs == 0)
            {
                throw new ArgumentException("stream interval must be > 0", nameof(streams));
            }
        }

        this.measureStartNs = measureStartNs;
        this.measureStopNs = measureStopNs;
        nextSchedNs = new ulong[this.streams.Length];
        nextSeq = new ulong[this.streams.Length];
        Array.Fill(nextSchedNs, startNs);
        Array.Fill(nextSeq, 1UL);
    }

    public ulong PeekNs()
    {
        var best = ulong.MaxValue;
        foreach (var due in nextSchedNs)
        {
            if (due < best)
            {
                best = due;
            }
        }

        return best;
    }

    public bool TryNext(ulong nowNs, out BenchSlot slot)
    {
        slot = default;
        var bestIndex = -1;
        var bestSched = ulong.MaxValue;
        for (var i = 0; i < nextSchedNs.Length; i++)
        {
            if (nextSchedNs[i] < bestSched)
            {
                bestSched = nextSchedNs[i];
                bestIndex = i;
            }
        }

        if (bestIndex < 0 || bestSched > nowNs)
        {
            return false;
        }

        var stream = streams[bestIndex];
        byte flags = 0;
        if (stream.MustDeliver)
        {
            flags |= BenchConstants.FlagMustDeliver;
        }
        if (stream.Broadcast)
        {
            flags |= BenchConstants.FlagBroadcast;
        }
        if (bestSched >= measureStartNs && bestSched < measureStopNs)
        {
            flags |= BenchConstants.FlagMeasure;
        }

        slot = new BenchSlot(bestSched, nextSeq[bestIndex], bestIndex, flags);
        nextSchedNs[bestIndex] = BenchClock.AddSaturating(nextSchedNs[bestIndex], stream.IntervalNs);
        if (nextSeq[bestIndex] != ulong.MaxValue)
        {
            nextSeq[bestIndex]++;
        }

        return true;
    }
}
