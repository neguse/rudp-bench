using System.Globalization;
using System.Text;

namespace BenchKit.CS;

public readonly record struct BenchRampConfig(
    int StartConns,
    int StepConns,
    ulong GuardNs,
    ulong SampleNs,
    ulong DrainNs,
    string? StopPath)
{
    private const string StartName = "BENCH_RAMP_START_CONNS";
    private const string StepName = "BENCH_RAMP_STEP_CONNS";
    private const string GuardName = "BENCH_RAMP_GUARD_NS";
    private const string SampleName = "BENCH_RAMP_SAMPLE_NS";
    private const string DrainName = "BENCH_RAMP_DRAIN_NS";
    private const string StopName = "BENCH_RAMP_STOP_PATH";

    public static BenchRampConfig? FromEnvironment(int maxConns)
    {
        var startText = Environment.GetEnvironmentVariable(StartName);
        var stepText = Environment.GetEnvironmentVariable(StepName);
        var guardText = Environment.GetEnvironmentVariable(GuardName);
        var sampleText = Environment.GetEnvironmentVariable(SampleName);
        var drainText = Environment.GetEnvironmentVariable(DrainName);
        var present = new[] { startText, stepText, guardText, sampleText, drainText }
            .Count(value => value is not null);
        if (present == 0)
        {
            return null;
        }
        if (present != 5)
        {
            throw new InvalidOperationException(
                $"ramp environment is all-or-nothing: set {StartName}, {StepName}, {GuardName}, {SampleName}, and {DrainName}");
        }

        if (maxConns <= 0 ||
            !int.TryParse(startText, NumberStyles.None, CultureInfo.InvariantCulture, out var startConns) ||
            !int.TryParse(stepText, NumberStyles.None, CultureInfo.InvariantCulture, out var stepConns) ||
            !ulong.TryParse(guardText, NumberStyles.None, CultureInfo.InvariantCulture, out var guardNs) ||
            !ulong.TryParse(sampleText, NumberStyles.None, CultureInfo.InvariantCulture, out var sampleNs) ||
            !ulong.TryParse(drainText, NumberStyles.None, CultureInfo.InvariantCulture, out var drainNs) ||
            startConns <= 0 || startConns > maxConns || stepConns <= 0 || sampleNs == 0)
        {
            throw new InvalidOperationException(
                $"invalid ramp environment: {StartName}=1..{maxConns}, {StepName}>0, " +
                $"{GuardName}>=0, {SampleName}>0, and {DrainName}>=0 are required");
        }

        return new BenchRampConfig(
            startConns,
            stepConns,
            guardNs,
            sampleNs,
            drainNs,
            Environment.GetEnvironmentVariable(StopName));
    }

    public IEnumerable<int> Targets(int maxConns)
    {
        var target = StartConns;
        while (true)
        {
            yield return target;
            if (target >= maxConns)
            {
                yield break;
            }
            target = (int)Math.Min((long)maxConns, (long)target + StepConns);
        }
    }

    public ulong PhaseSpanNs => BenchClock.AddSaturating(
        BenchClock.AddSaturating(GuardNs, SampleNs),
        DrainNs);

    public bool StopRequested => !string.IsNullOrWhiteSpace(StopPath) && File.Exists(StopPath);

    public void ThrowIfStopRequested()
    {
        if (StopRequested)
        {
            throw new BenchRampStopException();
        }
    }

    public ulong PhaseStartNs(ulong scheduleStartNs, int phaseIndex)
    {
        if (phaseIndex < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(phaseIndex));
        }
        var span = PhaseSpanNs;
        var offset = span != 0 && (ulong)phaseIndex > ulong.MaxValue / span
            ? ulong.MaxValue
            : span * (ulong)phaseIndex;
        return BenchClock.AddSaturating(scheduleStartNs, offset);
    }

    public ulong RequiredWindowNs(int maxConns)
    {
        var levels = (ulong)Targets(maxConns).Count();
        var span = PhaseSpanNs;
        return span != 0 && levels > ulong.MaxValue / span ? ulong.MaxValue : levels * span;
    }

    public static string SnapshotPath(string metricsPath, int phaseIndex, int activeConns) =>
        metricsPath + ".ramp-" + phaseIndex.ToString("D6", CultureInfo.InvariantCulture) +
        "-c" + activeConns.ToString("D6", CultureInfo.InvariantCulture) + ".json";

    public static Task DumpSnapshotAsync(
        BenchMetrics metrics,
        string metricsPath,
        int phaseIndex,
        int activeConns,
        CancellationToken cancellationToken = default)
    {
        var path = SnapshotPath(metricsPath, phaseIndex, activeConns);
        return File.WriteAllTextAsync(path, metrics.ToJson() + "\n", new UTF8Encoding(false), cancellationToken);
    }
}

public sealed class BenchRampStopException : Exception
{
}
