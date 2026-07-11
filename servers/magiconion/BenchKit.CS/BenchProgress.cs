using System.Globalization;
using System.Text;

namespace BenchKit.CS;

public readonly record struct BenchAuthoritativeProgress(
    string Role,
    ulong LocalConns,
    ulong RosterConns,
    ulong InputLastSentMin,
    ulong InputLastSentMax,
    ulong StateHeaderSeqRecvMin,
    ulong StateHeaderSeqRecvMax,
    ulong StateAppliedInputSeqRecvMin,
    ulong StateAppliedInputSeqRecvMax,
    ulong ServerStateTicks)
{
    public string ToJson()
    {
        var sb = new StringBuilder(384);
        sb.Append("{\"role\":\"").Append(Role).Append("\",\"scenario\":\"authoritative-state\"")
            .Append(",\"local_conns\":").Append(LocalConns)
            .Append(",\"roster_conns\":").Append(RosterConns)
            .Append(",\"input_last_sent_min\":").Append(InputLastSentMin)
            .Append(",\"input_last_sent_max\":").Append(InputLastSentMax)
            .Append(",\"state_header_seq_recv_min\":").Append(StateHeaderSeqRecvMin)
            .Append(",\"state_header_seq_recv_max\":").Append(StateHeaderSeqRecvMax)
            .Append(",\"state_applied_input_seq_recv_min\":").Append(StateAppliedInputSeqRecvMin)
            .Append(",\"state_applied_input_seq_recv_max\":").Append(StateAppliedInputSeqRecvMax)
            .Append(",\"server_state_ticks\":").Append(ServerStateTicks).Append('}');
        return sb.ToString();
    }

    public string ToLogLine() =>
        "BENCH_PROGRESS role=" + Role +
        " scenario=authoritative-state" +
        " local_conns=" + LocalConns.ToString(CultureInfo.InvariantCulture) +
        " roster_conns=" + RosterConns.ToString(CultureInfo.InvariantCulture) +
        " input_last_sent_min=" + InputLastSentMin.ToString(CultureInfo.InvariantCulture) +
        " input_last_sent_max=" + InputLastSentMax.ToString(CultureInfo.InvariantCulture) +
        " state_header_seq_recv_min=" + StateHeaderSeqRecvMin.ToString(CultureInfo.InvariantCulture) +
        " state_header_seq_recv_max=" + StateHeaderSeqRecvMax.ToString(CultureInfo.InvariantCulture) +
        " state_applied_input_seq_recv_min=" + StateAppliedInputSeqRecvMin.ToString(CultureInfo.InvariantCulture) +
        " state_applied_input_seq_recv_max=" + StateAppliedInputSeqRecvMax.ToString(CultureInfo.InvariantCulture) +
        " server_state_ticks=" + ServerStateTicks.ToString(CultureInfo.InvariantCulture);
}

public static class BenchProgress
{
    public static void UpdateMax(ref ulong value, ulong candidate)
    {
        while (true)
        {
            var current = Volatile.Read(ref value);
            if (candidate <= current || Interlocked.CompareExchange(ref value, candidate, current) == current)
            {
                return;
            }
        }
    }

    public static (ulong Min, ulong Max) MinMax(ReadOnlySpan<ulong> values)
    {
        if (values.Length == 0)
        {
            return (0, 0);
        }

        var min = ulong.MaxValue;
        var max = 0UL;
        foreach (var value in values)
        {
            min = Math.Min(min, value);
            max = Math.Max(max, value);
        }
        return (min, max);
    }

    public static string AttachToDoneStats(
        string metricsJson,
        BenchAuthoritativeProgress progress,
        ulong invalidPayload)
    {
        return AppendFields(
            metricsJson,
            "\"invalid_payload\":" + invalidPayload.ToString(CultureInfo.InvariantCulture) +
            ",\"authoritative_progress\":" + progress.ToJson());
    }

    public static string AttachValidationToDoneStats(string metricsJson, ulong invalidPayload)
    {
        return AppendFields(
            metricsJson,
            "\"invalid_payload\":" + invalidPayload.ToString(CultureInfo.InvariantCulture));
    }

    private static string AppendFields(string metricsJson, string fields)
    {
        var trimmed = metricsJson.TrimEnd();
        if (trimmed.Length < 2 || trimmed[0] != '{' || trimmed[^1] != '}')
        {
            throw new ArgumentException("stats must be a JSON object", nameof(metricsJson));
        }

        var separator = trimmed.Length == 2 ? string.Empty : ",";
        return trimmed[..^1] + separator + fields + "}";
    }

    public static void WriteDiagnostics(BenchAuthoritativeProgress progress, ulong invalidPayload)
    {
        Console.Error.WriteLine(progress.ToLogLine());
        Console.Error.WriteLine(
            "BENCH_INVALID_PAYLOAD value=" + invalidPayload.ToString(CultureInfo.InvariantCulture));
    }
}

public sealed class BenchPayloadValidation
{
    private long invalidPayload;

    public ulong Count => (ulong)Math.Max(0, Interlocked.Read(ref invalidPayload));

    public void Invalid() => Interlocked.Increment(ref invalidPayload);
}

public sealed class BenchAuthoritativeProgressTracker
{
    private readonly ulong[] inputLastSent;
    private readonly ulong[] stateHeaderSeqRecv;
    private readonly ulong[] stateAppliedInputSeqRecv;
    private readonly ulong rosterConns;
    private ulong serverStateTicks;

    public BenchAuthoritativeProgressTracker(int localConns, int rosterConns)
    {
        if (localConns < 0 || rosterConns < 0)
        {
            throw new ArgumentOutOfRangeException();
        }
        inputLastSent = new ulong[localConns];
        stateHeaderSeqRecv = new ulong[localConns];
        stateAppliedInputSeqRecv = new ulong[localConns];
        this.rosterConns = (ulong)rosterConns;
    }

    public void RecordInputLastSent(int localIndex, BenchHeader header, bool submitted)
    {
        if (!submitted || !IsMeasuredLossTolerant(header, BenchDirection.ClientToServer))
        {
            return;
        }
        UpdateMax(inputLastSent, localIndex, header.Seq);
    }

    public void RecordStateReceived(int localIndex, BenchHeader header, ulong appliedInputSeq)
    {
        if (!IsMeasuredLossTolerant(header, BenchDirection.ServerToClient))
        {
            return;
        }
        UpdateMax(stateHeaderSeqRecv, localIndex, header.Seq);
        UpdateMax(stateAppliedInputSeqRecv, localIndex, appliedInputSeq);
    }

    public void RecordServerStateTick(BenchSlot slot)
    {
        if ((slot.Flags & (BenchConstants.FlagMeasure | BenchConstants.FlagMustDeliver)) ==
            BenchConstants.FlagMeasure &&
            BenchConstants.DirectionFromFlags(slot.Flags) == BenchDirection.ServerToClient)
        {
            Interlocked.Increment(ref serverStateTicks);
        }
    }

    public BenchAuthoritativeProgress ClientSnapshot()
    {
        var input = Snapshot(inputLastSent);
        var state = Snapshot(stateHeaderSeqRecv);
        var applied = Snapshot(stateAppliedInputSeqRecv);
        return new BenchAuthoritativeProgress(
            "client",
            (ulong)inputLastSent.Length,
            rosterConns,
            input.Min,
            input.Max,
            state.Min,
            state.Max,
            applied.Min,
            applied.Max,
            0);
    }

    public BenchAuthoritativeProgress ServerSnapshot() => new(
        "server",
        0,
        rosterConns,
        0,
        0,
        0,
        0,
        0,
        0,
        Volatile.Read(ref serverStateTicks));

    private static bool IsMeasuredLossTolerant(BenchHeader header, BenchDirection direction) =>
        (header.Flags & (BenchConstants.FlagMeasure | BenchConstants.FlagMustDeliver)) ==
        BenchConstants.FlagMeasure &&
        BenchConstants.DirectionFromFlags(header.Flags) == direction;

    private static void UpdateMax(ulong[] values, int index, ulong value)
    {
        if ((uint)index >= (uint)values.Length)
        {
            return;
        }
        BenchProgress.UpdateMax(ref values[index], value);
    }

    private static (ulong Min, ulong Max) Snapshot(ulong[] values)
    {
        if (values.Length == 0)
        {
            return (0, 0);
        }
        var snapshot = new ulong[values.Length];
        for (var i = 0; i < values.Length; i++)
        {
            snapshot[i] = Volatile.Read(ref values[i]);
        }
        return BenchProgress.MinMax(snapshot);
    }
}
