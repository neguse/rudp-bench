using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Concurrent;
using BenchKit.CS;
using MagicOnion.Server;
using MagicOnion.Server.Hubs;
using Microsoft.AspNetCore.Server.Kestrel.Core;

var config = ServerConfig.Parse(args);
if (config.Describe)
{
    Console.WriteLine(BenchDescribe.Json);
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine("usage: MagicOnionBench.Server --port PORT");
    return 1;
}

using var stopCts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    stopCts.Cancel();
};
using var sigTerm = PosixSignalRegistration.Create(PosixSignal.SIGTERM, ctx =>
{
    ctx.Cancel = true;
    stopCts.Cancel();
});
using var sigInt = PosixSignalRegistration.Create(PosixSignal.SIGINT, ctx =>
{
    ctx.Cancel = true;
    stopCts.Cancel();
});

var runtime = new AuthoritativeRuntime(config.Scenario, config.StalenessPeriodNs);
var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = args });
builder.Logging.ClearProviders();
builder.WebHost.ConfigureKestrel(options =>
{
    options.ListenAnyIP(config.Port, listen => listen.Protocols = HttpProtocols.Http2);
    // HTTP/2 flow-control window(既定 conn 128KB / stream 96KB)は
    // 高 conns の受信集約で律速になりうる。公式ノブで拡大(--describe に開示)
    options.Limits.Http2.InitialConnectionWindowSize = 8 * 1024 * 1024;
    options.Limits.Http2.InitialStreamWindowSize = 4 * 1024 * 1024;
});
builder.Services.AddSingleton<ServerStats>();
builder.Services.AddSingleton(runtime);
builder.Services.AddMagicOnion();

var app = builder.Build();
app.MapMagicOnionService();
await app.StartAsync(stopCts.Token).ConfigureAwait(false);

var stats = app.Services.GetRequiredService<ServerStats>();
await using var control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
try
{
    BenchSchedule? schedule = null;
    if (control is not null)
    {
        await control.HelloAsync("server", "magiconion", 0, stopCts.Token).ConfigureAwait(false);
        await control.ReadyAsync(0, stopCts.Token).ConfigureAwait(false);
        schedule = await control.WaitScheduleAsync(stopCts.Token).ConfigureAwait(false);
    }

    if (config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState)
    {
        var rosterDeadline = schedule?.StartAtNs ??
            BenchClock.AddSaturating(BenchClock.NowNs(), ServerConfig.DevConnectTimeoutNs);
        var roster = await runtime.FreezeRosterAsync(
            config.Scenario.TotalConns,
            rosterDeadline,
            stopCts.Token).ConfigureAwait(false);
        if (schedule is null)
        {
            var now = BenchClock.NowNs();
            schedule = new BenchSchedule(
                BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs),
                BenchClock.AddSaturating(BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs), ServerConfig.DevDurationNs),
                BenchClock.AddSaturating(
                    BenchClock.AddSaturating(BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs), ServerConfig.DevDurationNs),
                    ServerConfig.DevDrainNs));
        }
        await RunAuthoritativeAsync(
            config.Scenario,
            schedule.Value,
            roster,
            runtime,
            control,
            stopCts.Token).ConfigureAwait(false);
    }
    else if (schedule is { } activeSchedule)
    {
        activeSchedule = await PollWindowUntilFinalAsync(control!, activeSchedule, stopCts.Token).ConfigureAwait(false);
        await DelayUntilNsAsync(activeSchedule.DrainUntilNs, stopCts.Token).ConfigureAwait(false);
    }
    else
    {
        await Task.Delay(Timeout.InfiniteTimeSpan, stopCts.Token).ConfigureAwait(false);
    }
}
catch (OperationCanceledException)
{
}

var reportJson = config.Scenario is null ? stats.ToJson() : runtime.MetricsJson();
var metricsOut = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
if (!string.IsNullOrWhiteSpace(metricsOut))
{
    await File.WriteAllTextAsync(metricsOut, reportJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);
}

var doneJson = reportJson;
if (runtime.Progress is not null)
{
    var snapshot = runtime.Progress.ServerSnapshot();
    BenchProgress.WriteDiagnostics(snapshot, stats.InvalidPayloadCount);
    doneJson = BenchProgress.AttachToDoneStats(reportJson, snapshot, stats.InvalidPayloadCount);
}
else if (config.Scenario is not null)
{
    doneJson = BenchProgress.AttachValidationToDoneStats(reportJson, stats.InvalidPayloadCount);
}

if (control is not null)
{
    await control.DoneAsync(doneJson, CancellationToken.None).ConfigureAwait(false);
}

await app.StopAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
return 0;

static async Task RunAuthoritativeAsync(
    BenchScenarioConfig scenario,
    BenchSchedule initialSchedule,
    HubPeer[] roster,
    AuthoritativeRuntime runtime,
    BenchControl? control,
    CancellationToken cancellationToken)
{
    var input = scenario.ClientInput!.Value;
    var state = scenario.ServerState!.Value;
    var schedule = initialSchedule;
    var plan = new BenchPlan(
        state.Streams(BenchDirection.ServerToClient),
        BenchClock.NowNs(),
        schedule.StartAtNs,
        schedule.StopAtNs);
    if (input.RateLt > 0)
    {
        foreach (var peer in roster)
        {
            runtime.ExpectInput(peer.OriginId, input, schedule.StartAtNs);
        }
    }

    var markedUnsent = false;
    while (!cancellationToken.IsCancellationRequested && BenchClock.NowNs() < schedule.DrainUntilNs)
    {
        var now = BenchClock.NowNs();
        if (control is not null &&
            await control.PollWindowAsync(cancellationToken).ConfigureAwait(false) is { } window)
        {
            schedule = window;
            plan.SetWindow(window.StartAtNs, window.StopAtNs);
        }
        if (now >= schedule.StartAtNs && now < schedule.StopAtNs)
        {
            runtime.Tick(now);
        }
        if (now < schedule.StopAtNs)
        {
            while (plan.TryNext(now, out var slot))
            {
                runtime.Progress!.RecordServerStateTick(slot);
                foreach (var target in roster)
                {
                    SubmitState(target, state, slot, scenario.TotalConns, runtime);
                }
            }
        }
        else if (!markedUnsent)
        {
            MarkStateUnsent(plan, roster.Length, scenario.TotalConns, schedule.StopAtNs, runtime);
            markedUnsent = true;
        }

        now = BenchClock.NowNs();
        var nextNs = schedule.DrainUntilNs;
        if (now < schedule.StopAtNs && plan.PeekNs() < nextNs)
        {
            nextNs = plan.PeekNs();
        }
        await DelayUntilNextAsync(now, nextNs, schedule.DrainUntilNs, cancellationToken).ConfigureAwait(false);
    }
    if (!markedUnsent)
    {
        MarkStateUnsent(plan, roster.Length, scenario.TotalConns, schedule.StopAtNs, runtime);
    }
}

static void SubmitState(
    HubPeer target,
    BenchTrafficConfig state,
    BenchSlot slot,
    int totalConns,
    AuthoritativeRuntime runtime)
{
    var header = new BenchHeader(
        slot.Seq,
        slot.SchedTsNs,
        BenchClock.NowNs(),
        slot.Flags,
        (uint)totalConns,
        slot.TrafficId);
    var payload = new byte[state.PayloadSize((slot.Flags & BenchConstants.FlagMustDeliver) != 0)];
    var submitted = false;
    if (BenchPayload.TryWrite(payload, header) &&
        BenchPayload.TryWriteAppliedInputSeq(payload, target.ReadAppliedInputSeq()) &&
        BenchPayload.TryFillTargetPad(payload, target.OriginId))
    {
        try
        {
            target.Receiver.OnPayload(payload);
            submitted = true;
        }
        catch
        {
            submitted = false;
        }
    }
    runtime.OnSlot(header, submitted);
}

static void MarkStateUnsent(
    BenchPlan plan,
    int rosterSize,
    int totalConns,
    ulong stopAtNs,
    AuthoritativeRuntime runtime)
{
    var cutoff = stopAtNs == 0 ? 0 : stopAtNs - 1;
    while (plan.TryNext(cutoff, out var slot))
    {
        var header = new BenchHeader(
            slot.Seq,
            slot.SchedTsNs,
            0,
            slot.Flags,
            (uint)totalConns,
            slot.TrafficId);
        for (var i = 0; i < rosterSize; i++)
        {
            runtime.OnSlot(header, false);
        }
    }
}

static async Task DelayUntilNextAsync(
    ulong nowNs,
    ulong nextNs,
    ulong limitNs,
    CancellationToken cancellationToken)
{
    var wakeNs = Math.Min(nextNs, BenchClock.AddSaturating(nowNs, 10_000_000UL));
    wakeNs = Math.Min(wakeNs, limitNs);
    if (wakeNs <= nowNs || wakeNs - nowNs < 1_000_000UL)
    {
        await Task.Yield();
        return;
    }
    await Task.Delay((int)((wakeNs - nowNs) / 1_000_000UL), cancellationToken).ConfigureAwait(false);
}

// 確定窓(window)が届くか暫定 start_at に達するまで非ブロッキングで poll する
//(benchspec v2)。どちらでも窓は確定なので以降の poll は不要
static async Task<BenchSchedule> PollWindowUntilFinalAsync(
    BenchControl control,
    BenchSchedule schedule,
    CancellationToken cancellationToken)
{
    while (!cancellationToken.IsCancellationRequested && BenchClock.NowNs() < schedule.StartAtNs)
    {
        if (await control.PollWindowAsync(cancellationToken).ConfigureAwait(false) is { } window)
        {
            return window;
        }

        await Task.Delay(10, cancellationToken).ConfigureAwait(false);
    }

    return schedule;
}

static async Task DelayUntilNsAsync(ulong targetNs, CancellationToken cancellationToken)
{
    while (!cancellationToken.IsCancellationRequested)
    {
        var now = BenchClock.NowNs();
        if (now >= targetNs)
        {
            return;
        }

        var remainNs = targetNs - now;
        var delayMs = (int)Math.Clamp(remainNs / 1_000_000UL, 1UL, 50UL);
        await Task.Delay(delayMs, cancellationToken).ConfigureAwait(false);
    }
}

internal readonly record struct ServerConfig(
    bool Valid,
    bool Describe,
    int Port,
    BenchScenarioConfig? Scenario,
    ulong StalenessPeriodNs)
{
    public const ulong DevWarmupNs = 200_000_000UL;
    public const ulong DevDurationNs = 2_000_000_000UL;
    public const ulong DevDrainNs = 500_000_000UL;
    public const ulong DevConnectTimeoutNs = 10_000_000_000UL;

    public static ServerConfig Parse(string[] args)
    {
        if (!BenchScenarioConfig.TryParseArguments(args, out var scenario, out var remaining))
        {
            return new ServerConfig(false, false, 0, null, 0);
        }
        var port = 0;
        var havePort = false;
        var stalenessPeriodNs = BenchConstants.DefaultStalenessPeriodNs;
        for (var i = 0; i < remaining.Length; i++)
        {
            if (remaining[i] == "--describe")
            {
                return new ServerConfig(true, true, 0, scenario, stalenessPeriodNs);
            }
            if (remaining[i] == "--port" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out port) &&
                port is > 0 and <= ushort.MaxValue)
            {
                havePort = true;
                continue;
            }
            if (remaining[i] == "--staleness-period-ns" && i + 1 < remaining.Length &&
                ulong.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out stalenessPeriodNs) &&
                stalenessPeriodNs > 0)
            {
                continue;
            }

            return new ServerConfig(false, false, 0, null, 0);
        }

        return new ServerConfig(havePort, false, port, scenario, stalenessPeriodNs);
    }
}

public sealed class AuthoritativeRuntime
{
    private readonly ConcurrentDictionary<uint, HubPeer> peers = [];
    private readonly object metricsGate = new();
    private volatile bool rosterFrozen;

    public AuthoritativeRuntime(BenchScenarioConfig? scenario, ulong stalenessPeriodNs)
    {
        Scenario = scenario;
        Progress = scenario?.Kind == BenchScenarioKind.AuthoritativeState
            ? new BenchAuthoritativeProgressTracker(0, scenario.TotalConns)
            : null;
        Metrics = new BenchMetrics(new BenchMetricsConfig(
            scenario is null ? 1U : checked((uint)scenario.TotalConns + 1U),
            0,
            stalenessPeriodNs,
            scenario is null ? 0U : (uint)scenario.TotalConns));
        scenario?.ConfigureMetrics(Metrics);
    }

    public BenchScenarioConfig? Scenario { get; }
    public BenchMetrics Metrics { get; }
    public BenchAuthoritativeProgressTracker? Progress { get; }

    public HubPeer Register(uint originId, IBenchHubReceiver receiver)
    {
        if (Scenario is null || originId >= (uint)Scenario.TotalConns || rosterFrozen)
        {
            throw new InvalidOperationException("invalid authoritative hub registration");
        }
        var peer = new HubPeer(originId, receiver);
        if (!peers.TryAdd(originId, peer))
        {
            throw new InvalidOperationException($"duplicate origin_id {originId}");
        }
        return peer;
    }

    public void Unregister(HubPeer peer)
    {
        if (!rosterFrozen)
        {
            peers.TryRemove(new KeyValuePair<uint, HubPeer>(peer.OriginId, peer));
        }
    }

    public async Task<HubPeer[]> FreezeRosterAsync(
        int totalConns,
        ulong deadlineNs,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested && BenchClock.NowNs() < deadlineNs)
        {
            if (peers.Count == totalConns)
            {
                var roster = new HubPeer[totalConns];
                var complete = true;
                for (var i = 0; i < totalConns; i++)
                {
                    if (!peers.TryGetValue((uint)i, out roster[i]!))
                    {
                        complete = false;
                        break;
                    }
                }
                if (complete)
                {
                    rosterFrozen = true;
                    return roster;
                }
            }
            await Task.Delay(5, cancellationToken).ConfigureAwait(false);
        }
        throw new InvalidOperationException($"connection roster incomplete: got {peers.Count}, want {totalConns}");
    }

    public void OnInput(HubPeer peer, BenchHeader header)
    {
        if ((header.Flags & BenchConstants.FlagMustDeliver) == 0)
        {
            peer.ApplyInputSeq(header.Seq);
        }
        lock (metricsGate)
        {
            Metrics.OnRecv(peer.OriginId, header, BenchClock.NowNs());
        }
    }

    public void ExpectInput(uint originId, BenchTrafficConfig input, ulong firstSchedTsNs)
    {
        lock (metricsGate)
        {
            Metrics.ExpectLatest(
                originId,
                originId,
                input.TrafficId,
                BenchDirection.ClientToServer,
                firstSchedTsNs);
        }
    }

    public void OnSlot(BenchHeader header, bool submitted)
    {
        lock (metricsGate)
        {
            Metrics.OnSlot(header, submitted);
        }
    }

    public void Tick(ulong nowNs)
    {
        lock (metricsGate)
        {
            Metrics.Tick(nowNs);
        }
    }

    public string MetricsJson()
    {
        lock (metricsGate)
        {
            return Metrics.ToJson();
        }
    }

}

public sealed class HubPeer(uint originId, IBenchHubReceiver receiver)
{
    private ulong appliedInputSeq;

    public uint OriginId { get; } = originId;
    public IBenchHubReceiver Receiver { get; } = receiver;

    public void ApplyInputSeq(ulong seq)
    {
        BenchProgress.UpdateMax(ref appliedInputSeq, seq);
    }

    public ulong ReadAppliedInputSeq() => Volatile.Read(ref appliedInputSeq);
}

public sealed class ServerStats
{
    private const int DistEcho = 0;
    private const int DistBroadcast = 1;

    // カウンタは Interlocked: StreamingHub のメソッドは接続ごとに複数
    // thread-pool スレッドで並行に走るため、単一 lock だと全 hub が
    // メッセージごとに直列化される(msquic で同型の修正が broadcast の
    // 主要律速だった)。ToJson は全接続停止後にのみ呼ぶ。
    private readonly ulong[,] recv = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] recvMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submit = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submitMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] sendFailed = new ulong[BenchConstants.ClassCount, 2];
    private ulong invalidPayload;
    private int connections;

    public int ConnectionCount => Volatile.Read(ref connections);

    public ulong InvalidPayloadCount => Volatile.Read(ref invalidPayload);

    public void Connected() => Interlocked.Increment(ref connections);

    public void Disconnected() => Interlocked.Decrement(ref connections);

    public void InvalidPayload() => Interlocked.Increment(ref invalidPayload);

    public void CountRecv(byte flags)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        Interlocked.Increment(ref recv[cls, dist]);
        if ((flags & BenchConstants.FlagMeasure) != 0)
        {
            Interlocked.Increment(ref recvMeasured[cls, dist]);
        }
    }

    public void CountSubmit(byte flags, ulong okCount, ulong failedCount)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        if (okCount != 0)
        {
            Interlocked.Add(ref submit[cls, dist], okCount);
            if ((flags & BenchConstants.FlagMeasure) != 0)
            {
                Interlocked.Add(ref submitMeasured[cls, dist], okCount);
            }
        }
        if (failedCount != 0)
        {
            Interlocked.Add(ref sendFailed[cls, dist], failedCount);
        }
    }

    public string ToJson()
    {
        {
            var sb = new StringBuilder(1024);
            sb.Append("{\"recv\":{\"loss_tolerant\":{\"echo\":").Append(recv[0, 0])
                .Append(",\"broadcast\":").Append(recv[0, 1])
                .Append("},\"must_deliver\":{\"echo\":").Append(recv[1, 0])
                .Append(",\"broadcast\":").Append(recv[1, 1])
                .Append("}},\"submit\":{\"loss_tolerant\":{\"echo\":").Append(submit[0, 0])
                .Append(",\"broadcast\":").Append(submit[0, 1])
                .Append("},\"must_deliver\":{\"echo\":").Append(submit[1, 0])
                .Append(",\"broadcast\":").Append(submit[1, 1])
                .Append("}},\"recv_measured\":{\"loss_tolerant\":{\"echo\":").Append(recvMeasured[0, 0])
                .Append(",\"broadcast\":").Append(recvMeasured[0, 1])
                .Append("},\"must_deliver\":{\"echo\":").Append(recvMeasured[1, 0])
                .Append(",\"broadcast\":").Append(recvMeasured[1, 1])
                .Append("}},\"submit_measured\":{\"loss_tolerant\":{\"echo\":").Append(submitMeasured[0, 0])
                .Append(",\"broadcast\":").Append(submitMeasured[0, 1])
                .Append("},\"must_deliver\":{\"echo\":").Append(submitMeasured[1, 0])
                .Append(",\"broadcast\":").Append(submitMeasured[1, 1])
                .Append("}},\"send_failed\":{\"loss_tolerant\":{\"echo\":").Append(sendFailed[0, 0])
                .Append(",\"broadcast\":").Append(sendFailed[0, 1])
                .Append("},\"must_deliver\":{\"echo\":").Append(sendFailed[1, 0])
                .Append(",\"broadcast\":").Append(sendFailed[1, 1])
                .Append("}},\"invalid_payload\":").Append(invalidPayload).Append('}');
            return sb.ToString();
        }
    }

    private static int DistFromFlags(byte flags) =>
        (flags & BenchConstants.FlagBroadcast) != 0 ? DistBroadcast : DistEcho;
}

public sealed class BenchHub(ServerStats stats, AuthoritativeRuntime runtime)
    : StreamingHubBase<IBenchHub, IBenchHubReceiver>, IBenchHub
{
    private const string GroupName = "bench";
    private IGroup<IBenchHubReceiver>? room;
    private HubPeer? authoritativePeer;
    private bool joined;

    public async ValueTask JoinAsync(uint originId)
    {
        if (joined)
        {
            return;
        }

        if (runtime.Scenario?.Kind == BenchScenarioKind.AuthoritativeState)
        {
            authoritativePeer = runtime.Register(originId, Client);
        }
        else
        {
            room = await Group.AddAsync(GroupName).ConfigureAwait(false);
        }
        joined = true;
        stats.Connected();
    }

    public ValueTask SendPayloadAsync(byte[] payload)
    {
        if (!BenchPayload.TryRead(payload, out var header) || !BenchPayload.ValidateBody(payload, header))
        {
            stats.InvalidPayload();
            return ValueTask.CompletedTask;
        }

        stats.CountRecv(header.Flags);
        if (runtime.Scenario?.Kind == BenchScenarioKind.AuthoritativeState)
        {
            var input = runtime.Scenario.ClientInput!.Value;
            if (authoritativePeer is null ||
                header.OriginId != authoritativePeer.OriginId ||
                header.TrafficId != input.TrafficId ||
                (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ClientToServer ||
                !input.Accepts(header.Flags, payload.Length))
            {
                stats.InvalidPayload();
                return ValueTask.CompletedTask;
            }
            runtime.OnInput(authoritativePeer, header);
            return ValueTask.CompletedTask;
        }
        if ((header.Flags & BenchConstants.FlagBroadcast) == 0)
        {
            try
            {
                Client.OnPayload(payload);
                stats.CountSubmit(header.Flags, 1, 0);
            }
            catch
            {
                stats.CountSubmit(header.Flags, 0, 1);
            }

            return ValueTask.CompletedTask;
        }

        var targetCount = (ulong)Math.Max(0, stats.ConnectionCount);
        try
        {
            if (room is null)
            {
                stats.CountSubmit(header.Flags, 0, 1);
            }
            else
            {
                room.All.OnPayload(payload);
                stats.CountSubmit(header.Flags, targetCount, 0);
            }
        }
        catch
        {
            stats.CountSubmit(header.Flags, 0, targetCount == 0 ? 1 : targetCount);
        }

        return ValueTask.CompletedTask;
    }

    protected override ValueTask OnDisconnected()
    {
        if (joined)
        {
            joined = false;
            if (authoritativePeer is not null)
            {
                runtime.Unregister(authoritativePeer);
                authoritativePeer = null;
            }
            stats.Disconnected();
        }

        return ValueTask.CompletedTask;
    }
}
