using System.Globalization;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using BenchKit.CS;
using Grpc.Net.Client;
using MagicOnion.Client;

var config = ClientConfig.Parse(args);

// 計測器(client farm)側: ThreadPool の hill-climbing 起動遅延が送信 pacing の
// stall(数百 ms 級)として観測されるため、最低スレッド数を先に確保する。
// client 専用の farm 設定であり server には入れない
System.Threading.ThreadPool.SetMinThreads(Environment.ProcessorCount * 4, Environment.ProcessorCount * 4);

if (config.Describe)
{
    Console.WriteLine(BenchDescribe.Json);
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine(
        "usage: MagicOnionBench.Client --host HOST --port PORT --conns N --proc-index N " +
        "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES " +
        "[--payload-lt BYTES] [--payload-md BYTES] " +
        "--deadline-ns NS --staleness-period-ns NS");
    return 1;
}
var rampConfig = BenchRampConfig.FromEnvironment(config.Conns);

AppContext.SetSwitch("System.Net.Http.SocketsHttpHandler.Http2UnencryptedSupport", true);

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

var maxOriginId = config.Scenario is { } configuredScenario
    ? checked((uint)configuredScenario.TotalConns + 1U)
    : checked(config.OriginBase + (uint)config.Conns);
var metrics = new BenchMetrics(new BenchMetricsConfig(
    maxOriginId == 0 ? 1 : maxOriginId,
    config.DeadlineNs,
    config.StalenessPeriodNs,
    config.Scenario is null ? 0U : (uint)config.Conns));
config.Scenario?.ConfigureMetrics(metrics);
var metricsGate = new object();
var connections = new List<ClientConnection>(config.Conns);
var payloadValidation = new BenchPayloadValidation();
var progress = config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState
    ? new BenchAuthoritativeProgressTracker(config.Conns, config.Scenario.TotalConns)
    : null;
BenchControl? control = null;

try
{
    control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
    if (control is not null)
    {
        await control.HelloAsync("client", "magiconion", config.ProcIndex, stopCts.Token).ConfigureAwait(false);
    }

    if (rampConfig is { } ramp)
    {
        return await RunRampAsync(
            config,
            ramp,
            connections,
            metrics,
            metricsGate,
            payloadValidation,
            progress,
            control,
            stopCts.Token).ConfigureAwait(false);
    }

    for (var i = 0; i < config.Conns; i++)
    {
        var originId = config.OriginBase + (uint)i;
        var receiver = new BenchReceiver(
            metrics,
            metricsGate,
            (uint)i,
            originId,
            config.Scenario,
            payloadValidation,
            progress);
        var handler = new SocketsHttpHandler
        {
            EnableMultipleHttp2Connections = true,
            PooledConnectionIdleTimeout = TimeSpan.FromMinutes(5)
        };
        var channel = GrpcChannel.ForAddress(FormatAddress(config.Host, config.Port), new GrpcChannelOptions
        {
            HttpHandler = handler,
            MaxReceiveMessageSize = BenchConstants.MaxPayloadBytes,
            MaxSendMessageSize = BenchConstants.MaxPayloadBytes
        });
        var hub = await StreamingHubClient
            .ConnectAsync<IBenchHub, IBenchHubReceiver>(channel, receiver)
            .ConfigureAwait(false);
        await hub.JoinAsync(originId).ConfigureAwait(false);
        connections.Add(new ClientConnection(originId, receiver, hub, channel, handler));
    }

    BenchSchedule schedule;
    if (control is not null)
    {
        await control.ReadyAsync(config.Conns, stopCts.Token).ConfigureAwait(false);
        schedule = await control.WaitScheduleAsync(stopCts.Token).ConfigureAwait(false);
    }
    else
    {
        var now = BenchClock.NowNs();
        schedule = new BenchSchedule(
            BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs),
            BenchClock.AddSaturating(BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs), ClientConfig.DevDurationNs),
            BenchClock.AddSaturating(
                BenchClock.AddSaturating(BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs), ClientConfig.DevDurationNs),
                ClientConfig.DevDrainNs));
    }

    var streams = BuildStreams(config);
    if (config.Scenario is { } activeScenario)
    {
        lock (metricsGate)
        {
            activeScenario.RegisterExpectedLatestFlows(
                metrics,
                (uint)connections.Count,
                config.OriginBase,
                schedule.StartAtNs);
        }
    }
    var planStartNs = BenchClock.NowNs();
    foreach (var connection in connections)
    {
        connection.Plan = new BenchPlan(streams, planStartNs, schedule.StartAtNs, schedule.StopAtNs);
        connection.SendPipe = new SendPipe(
            connection,
            config.PayloadLt,
            config.PayloadMd,
            metrics,
            metricsGate,
            payloadValidation,
            progress);
    }

    var markedUnsent = false;
    var steady = new BenchSteady();
    while (!stopCts.IsCancellationRequested && BenchClock.NowNs() < schedule.DrainUntilNs)
    {
        var now = BenchClock.NowNs();
        // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
        // raw counts は metricsGate 下で読む(receiver/pipe スレッドが書く)。
        // window を受けたら全 conn の plan に計測窓を差し替える
        if (control is not null)
        {
            BenchRawCounts raw;
            lock (metricsGate)
            {
                raw = metrics.RawCounts();
            }
            var window = await steady.TickAsync(
                control, raw.Submitted, raw.RecvMeasured + raw.RecvUnmeasured, schedule, now, stopCts.Token)
                .ConfigureAwait(false);
            if (window is { } w)
            {
                if (config.Scenario is { } windowScenario)
                {
                    lock (metricsGate)
                    {
                        windowScenario.RegisterExpectedLatestFlows(
                            metrics,
                            (uint)connections.Count,
                            config.OriginBase,
                            w.StartAtNs);
                    }
                }
                schedule = w;
                foreach (var connection in connections)
                {
                    connection.Plan!.SetWindow(w.StartAtNs, w.StopAtNs);
                }
            }
        }

        if (now >= schedule.StartAtNs && now < schedule.StopAtNs)
        {
            lock (metricsGate)
            {
                metrics.Tick(now);
            }
        }

        if (now < schedule.StopAtNs)
        {
            foreach (var connection in connections)
            {
                while (connection.Plan!.TryNext(now, out var slot))
                {
                    connection.SendPipe!.Submit(slot);
                }
            }
        }
        else if (!markedUnsent)
        {
            MarkUnsentAndStopPipes(connections, schedule.StopAtNs, metrics, metricsGate);
            markedUnsent = true;
        }

        now = BenchClock.NowNs();
        var nextNs = schedule.DrainUntilNs;
        if (now < schedule.StopAtNs)
        {
            var due = NextPlanDue(connections);
            if (due < nextNs)
            {
                nextNs = due;
            }
        }

        await DelayUntilNextAsync(now, nextNs, schedule.DrainUntilNs, stopCts.Token).ConfigureAwait(false);
    }

    if (!markedUnsent)
    {
        MarkUnsentAndStopPipes(connections, schedule.StopAtNs, metrics, metricsGate);
    }

    foreach (var connection in connections)
    {
        if (connection.SendPipe is not null)
        {
            await connection.SendPipe.Completion.ConfigureAwait(false);
        }
    }

    var metricsPath = MetricsPathOrDefault();
    string metricsJson;
    lock (metricsGate)
    {
        metricsJson = metrics.ToJson();
    }
    await File.WriteAllTextAsync(metricsPath, metricsJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);

    var doneJson = metricsJson;
    if (progress is not null)
    {
        var snapshot = progress.ClientSnapshot();
        BenchProgress.WriteDiagnostics(snapshot, payloadValidation.Count);
        doneJson = BenchProgress.AttachToDoneStats(metricsJson, snapshot, payloadValidation.Count);
    }
    else if (config.Scenario is not null)
    {
        doneJson = BenchProgress.AttachValidationToDoneStats(metricsJson, payloadValidation.Count);
    }

    if (control is not null)
    {
        await control.DoneAsync(doneJson, CancellationToken.None).ConfigureAwait(false);
    }

    return 0;
}
catch (OperationCanceledException)
{
    return 130;
}
catch (Exception ex)
{
    Console.Error.WriteLine(ex);
    return 1;
}
finally
{
    if (control is not null)
    {
        await control.DisposeAsync().ConfigureAwait(false);
    }

    foreach (var connection in connections)
    {
        await connection.DisposeAsync().ConfigureAwait(false);
    }
}

static async Task<int> RunRampAsync(
    ClientConfig config,
    BenchRampConfig ramp,
    List<ClientConnection> connections,
    BenchMetrics metrics,
    object metricsGate,
    BenchPayloadValidation payloadValidation,
    BenchAuthoritativeProgressTracker? progress,
    BenchControl? control,
    CancellationToken cancellationToken)
{
    async Task AddConnectionsAsync(int target, ulong deadlineNs, bool observeStop)
    {
        while (connections.Count < target)
        {
            if (observeStop)
            {
                ramp.ThrowIfStopRequested();
            }
            if (BenchClock.NowNs() >= deadlineNs)
            {
                return;
            }
            var localIndex = connections.Count;
            var originId = config.OriginBase + (uint)localIndex;
            var receiver = new BenchReceiver(
                metrics,
                metricsGate,
                (uint)localIndex,
                originId,
                config.Scenario,
                payloadValidation,
                progress);
            var handler = new SocketsHttpHandler
            {
                EnableMultipleHttp2Connections = true,
                PooledConnectionIdleTimeout = TimeSpan.FromMinutes(5)
            };
            var channel = GrpcChannel.ForAddress(FormatAddress(config.Host, config.Port), new GrpcChannelOptions
            {
                HttpHandler = handler,
                MaxReceiveMessageSize = BenchConstants.MaxPayloadBytes,
                MaxSendMessageSize = BenchConstants.MaxPayloadBytes
            });
            IBenchHub? hub = null;
            try
            {
                hub = await StreamingHubClient
                    .ConnectAsync<IBenchHub, IBenchHubReceiver>(channel, receiver)
                    .ConfigureAwait(false);
                await hub.JoinAsync(originId).ConfigureAwait(false);
                var connection = new ClientConnection(originId, receiver, hub, channel, handler);
                connection.SendPipe = new SendPipe(
                    connection,
                    config.PayloadLt,
                    config.PayloadMd,
                    metrics,
                    metricsGate,
                    payloadValidation,
                    progress);
                connections.Add(connection);
            }
            catch
            {
                if (hub is IAsyncDisposable asyncDisposable)
                {
                    await asyncDisposable.DisposeAsync().ConfigureAwait(false);
                }
                else if (hub is IDisposable disposable)
                {
                    disposable.Dispose();
                }
                channel.Dispose();
                handler.Dispose();
                throw;
            }
        }
    }

    await AddConnectionsAsync(ramp.StartConns, ulong.MaxValue, observeStop: false).ConfigureAwait(false);

    BenchSchedule schedule;
    if (control is not null)
    {
        await control.ReadyAsync(config.Conns, cancellationToken).ConfigureAwait(false);
        schedule = await control.WaitScheduleAsync(cancellationToken).ConfigureAwait(false);
    }
    else
    {
        var now = BenchClock.NowNs();
        var duration = ramp.RequiredWindowNs(config.Conns);
        schedule = new BenchSchedule(now, BenchClock.AddSaturating(now, duration), BenchClock.AddSaturating(now, duration));
    }

    var streams = BuildStreams(config);
    foreach (var connection in connections)
    {
        connection.Plan = new BenchPlan(streams, schedule.StartAtNs, schedule.StartAtNs, schedule.StopAtNs);
    }
    var metricsPath = MetricsPathOrDefault();
    var phaseIndex = 0;
    try
    {
        foreach (var target in ramp.Targets(config.Conns))
        {
            var phaseStart = ramp.PhaseStartNs(schedule.StartAtNs, phaseIndex);
            var resetDeadline = BenchClock.AddSaturating(phaseStart, ramp.GuardNs);
            var sampleEnd = BenchClock.AddSaturating(resetDeadline, ramp.SampleNs);
            var phaseEnd = BenchClock.AddSaturating(sampleEnd, ramp.DrainNs);
            await DriveRampTrafficAsync(
                connections, metrics, metricsGate, schedule, phaseStart, ramp, cancellationToken)
                .ConfigureAwait(false);

            lock (metricsGate)
            {
                metrics.Reset();
                metrics.SetObservationWindow(resetDeadline, sampleEnd);
                config.Scenario?.RegisterExpectedLatestFlows(
                    metrics, (uint)target, config.OriginBase, resetDeadline, (uint)target);
            }

            await AddConnectionsAsync(target, resetDeadline, observeStop: true).ConfigureAwait(false);
            foreach (var connection in connections)
            {
                connection.Plan ??= new BenchPlan(
                    streams,
                    BenchClock.NowNs(),
                    schedule.StartAtNs,
                    schedule.StopAtNs);
            }

            await DriveRampTrafficAsync(
                connections, metrics, metricsGate, schedule, resetDeadline, ramp, cancellationToken)
                .ConfigureAwait(false);

            await DriveRampTrafficAsync(
                connections, metrics, metricsGate, schedule, phaseEnd, ramp, cancellationToken)
                .ConfigureAwait(false);

            string snapshotJson;
            lock (metricsGate)
            {
                snapshotJson = metrics.ToJson();
            }
            await File.WriteAllTextAsync(
                BenchRampConfig.SnapshotPath(metricsPath, phaseIndex, target),
                snapshotJson + "\n",
                new UTF8Encoding(false),
                CancellationToken.None).ConfigureAwait(false);
            phaseIndex++;
            ramp.ThrowIfStopRequested();
        }
    }
    catch (BenchRampStopException)
    {
    }

    foreach (var connection in connections)
    {
        connection.SendPipe!.CompleteDroppingPending();
    }
    foreach (var connection in connections)
    {
        await connection.SendPipe!.Completion.ConfigureAwait(false);
    }

    string metricsJson;
    lock (metricsGate)
    {
        metricsJson = metrics.ToJson();
    }
    await File.WriteAllTextAsync(metricsPath, metricsJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);
    var doneJson = progress is null
        ? BenchProgress.AttachValidationToDoneStats(metricsJson, payloadValidation.Count)
        : BenchProgress.AttachToDoneStats(metricsJson, progress.ClientSnapshot(), payloadValidation.Count);
    if (control is not null)
    {
        await control.DoneAsync(doneJson, CancellationToken.None).ConfigureAwait(false);
    }
    return 0;
}

static async Task DriveRampTrafficAsync(
    IReadOnlyList<ClientConnection> connections,
    BenchMetrics metrics,
    object metricsGate,
    BenchSchedule schedule,
    ulong deadlineNs,
    BenchRampConfig ramp,
    CancellationToken cancellationToken)
{
    if (deadlineNs > schedule.StopAtNs)
    {
        throw new InvalidOperationException("ramp phases exceed the scheduled measurement window");
    }
    while (BenchClock.NowNs() < deadlineNs)
    {
        ramp.ThrowIfStopRequested();
        cancellationToken.ThrowIfCancellationRequested();
        var now = BenchClock.NowNs();
        var sendThrough = now < deadlineNs ? now : deadlineNs - 1;
        if (now >= schedule.StartAtNs)
        {
            lock (metricsGate)
            {
                metrics.Tick(now);
            }
        }
        foreach (var connection in connections)
        {
            while (connection.Plan!.TryNext(sendThrough, out var slot))
            {
                connection.SendPipe!.Submit(slot);
            }
        }
        var next = Math.Min(deadlineNs, NextPlanDue(connections));
        await DelayUntilNextAsync(now, next, deadlineNs, cancellationToken).ConfigureAwait(false);
    }
}

static IReadOnlyList<BenchStream> BuildStreams(ClientConfig config)
{
    if (config.Scenario is { Kind: BenchScenarioKind.AuthoritativeState, ClientInput: { } input })
    {
        return input.Streams(BenchDirection.ClientToServer);
    }
    if (config.Scenario is { Kind: BenchScenarioKind.EnvironmentBaseline, ClientInput: { } baseline })
    {
        return baseline.Streams(BenchDirection.RoomRelay);
    }
    if (config.Scenario?.RoomPublish is { } publish)
    {
        return publish.Streams(BenchDirection.RoomRelay, true);
    }

    var streams = new List<BenchStream>(2);
    if (config.RateLt > 0)
    {
        streams.Add(new BenchStream(false, config.BroadcastLt, IntervalFromRate(config.RateLt)));
    }
    if (config.RateMd > 0)
    {
        streams.Add(new BenchStream(true, config.BroadcastMd, IntervalFromRate(config.RateMd)));
    }

    return streams;
}

static ulong IntervalFromRate(double rateHz)
{
    var interval = 1_000_000_000.0 / rateHz;
    if (!double.IsFinite(interval) || interval < 1.0 || interval > ulong.MaxValue)
    {
        throw new ArgumentOutOfRangeException(nameof(rateHz), "invalid rate");
    }

    var ns = (ulong)(interval + 0.5);
    return ns == 0 ? 1 : ns;
}

static void MarkUnsentAndStopPipes(
    IReadOnlyList<ClientConnection> connections,
    ulong stopAtNs,
    BenchMetrics metrics,
    object metricsGate)
{
    var cutoff = stopAtNs == 0 ? 0 : stopAtNs - 1;
    foreach (var connection in connections)
    {
        while (connection.Plan!.TryNext(cutoff, out var slot))
        {
            SendPipe.RecordSlot(metrics, metricsGate, connection.OriginId, slot, 0, false);
        }
        connection.SendPipe!.CompleteDroppingPending();
    }
}

static ulong NextPlanDue(IReadOnlyList<ClientConnection> connections)
{
    var next = ulong.MaxValue;
    foreach (var connection in connections)
    {
        var due = connection.Plan!.PeekNs();
        if (due < next)
        {
            next = due;
        }
    }

    return next;
}

static async Task DelayUntilNextAsync(ulong nowNs, ulong nextNs, ulong limitNs, CancellationToken cancellationToken)
{
    var wakeNs = nextNs;
    var sliceNs = BenchClock.AddSaturating(nowNs, 10_000_000UL);
    if (wakeNs > sliceNs)
    {
        wakeNs = sliceNs;
    }
    if (wakeNs > limitNs)
    {
        wakeNs = limitNs;
    }
    if (wakeNs <= nowNs)
    {
        await Task.Yield();
        return;
    }

    var deltaNs = wakeNs - nowNs;
    if (deltaNs < 1_000_000UL)
    {
        await Task.Yield();
        return;
    }

    await Task.Delay((int)(deltaNs / 1_000_000UL), cancellationToken).ConfigureAwait(false);
}

static string FormatAddress(string host, int port)
{
    var formattedHost = host.Contains(':', StringComparison.Ordinal) &&
                        !host.StartsWith('[')
        ? "[" + host + "]"
        : host;
    return "http://" + formattedHost + ":" + port.ToString(CultureInfo.InvariantCulture);
}

static string MetricsPathOrDefault()
{
    var path = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
    if (!string.IsNullOrWhiteSpace(path))
    {
        return path;
    }

    return "/tmp/rudp-bench-magiconion-client-" +
           Environment.ProcessId.ToString(CultureInfo.InvariantCulture) + ".json";
}

internal sealed class BenchReceiver : IBenchHubReceiver
{
    private readonly BenchMetrics metrics;
    private readonly object metricsGate;
    private readonly uint localIndex;
    private readonly uint originId;
    private readonly BenchScenarioConfig? scenario;
    private readonly BenchPayloadValidation payloadValidation;
    private readonly BenchAuthoritativeProgressTracker? progress;
    private ulong lastInputSeq;
    private ulong lastAppliedInputSeq;

    public uint LocalIndex => localIndex;

    public BenchReceiver(
        BenchMetrics metrics,
        object metricsGate,
        uint localIndex,
        uint originId,
        BenchScenarioConfig? scenario,
        BenchPayloadValidation payloadValidation,
        BenchAuthoritativeProgressTracker? progress)
    {
        this.metrics = metrics;
        this.metricsGate = metricsGate;
        this.localIndex = localIndex;
        this.originId = originId;
        this.scenario = scenario;
        this.payloadValidation = payloadValidation;
        this.progress = progress;
    }

    public void RecordInputSeq(ulong seq)
    {
        BenchProgress.UpdateMax(ref lastInputSeq, seq);
    }

    public void OnPayload(byte[] payload)
    {
        if (!BenchPayload.TryRead(payload, out var header))
        {
            payloadValidation.Invalid();
            return;
        }
        if (scenario?.Kind == BenchScenarioKind.AuthoritativeState)
        {
            var state = scenario.ServerState!.Value;
            if (header.OriginId != (uint)scenario.TotalConns ||
                header.TrafficId != state.TrafficId ||
                (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ServerToClient ||
                !state.Accepts(header.Flags, payload.Length) ||
                !BenchPayload.TryReadAppliedInputSeq(payload, out var appliedInputSeq) ||
                appliedInputSeq > Volatile.Read(ref lastInputSeq) ||
                !BenchPayload.ValidateTargetPad(payload, originId))
            {
                payloadValidation.Invalid();
                return;
            }
            BenchProgress.UpdateMax(ref lastAppliedInputSeq, appliedInputSeq);
            progress?.RecordStateReceived((int)localIndex, header, appliedInputSeq);
        }
        else if (!BenchPayload.ValidateBody(payload, header))
        {
            payloadValidation.Invalid();
            return;
        }

        var recvTsNs = BenchClock.NowNs();
        lock (metricsGate)
        {
            metrics.OnRecv(localIndex, header, recvTsNs);
        }
    }
}

internal sealed class ClientConnection(
    uint originId,
    BenchReceiver receiver,
    IBenchHub hub,
    GrpcChannel channel,
    SocketsHttpHandler handler) : IAsyncDisposable
{
    public uint OriginId { get; } = originId;
    public BenchReceiver Receiver { get; } = receiver;
    public IBenchHub Hub { get; } = hub;
    public BenchPlan? Plan { get; set; }
    public SendPipe? SendPipe { get; set; }

    public async ValueTask DisposeAsync()
    {
        if (SendPipe is not null)
        {
            SendPipe.CompleteDroppingPending();
            await SendPipe.Completion.ConfigureAwait(false);
        }

        if (Hub is IAsyncDisposable asyncDisposable)
        {
            await asyncDisposable.DisposeAsync().ConfigureAwait(false);
        }
        else if (Hub is IDisposable disposable)
        {
            disposable.Dispose();
        }

        channel.Dispose();
        handler.Dispose();
    }
}

internal sealed class SendPipe
{
    private readonly BenchReceiver receiver;
    private readonly IBenchHub hub;
    private readonly uint originId;
    private readonly int payloadSizeLt;
    private readonly int payloadSizeMd;
    private readonly BenchMetrics metrics;
    private readonly object metricsGate;
    private readonly BenchPayloadValidation payloadValidation;
    private readonly BenchAuthoritativeProgressTracker? progress;
    private readonly object gate = new();
    private readonly Queue<BenchSlot> mustDeliver = new();
    private BenchSlot? lossTolerantLatest;
    private bool accepting = true;

    public SendPipe(
        ClientConnection connection,
        int payloadSizeLt,
        int payloadSizeMd,
        BenchMetrics metrics,
        object metricsGate,
        BenchPayloadValidation payloadValidation,
        BenchAuthoritativeProgressTracker? progress)
    {
        // 送信は fire-and-forget proxy 経由にする。素の hub proxy はサーバ応答まで
        // ValueTask が完了せず、送信ループが RTT に律速されて loss-tolerant が
        // ほぼ全部 coalesce される(wired 10ms RTT で attempted 0.47 の実測)。
        // latest-value 送信で応答を待たないのが idiomatic な使い方。
        hub = connection.Hub.FireAndForget();
        originId = connection.OriginId;
        receiver = connection.Receiver;
        this.payloadSizeLt = payloadSizeLt;
        this.payloadSizeMd = payloadSizeMd;
        this.metrics = metrics;
        this.metricsGate = metricsGate;
        this.payloadValidation = payloadValidation;
        this.progress = progress;
        Completion = Task.Run(RunAsync);
    }

    public Task Completion { get; }

    public void Submit(BenchSlot slot)
    {
        BenchSlot? dropped = null;
        lock (gate)
        {
            if (!accepting)
            {
                dropped = slot;
            }
            else if ((slot.Flags & BenchConstants.FlagMustDeliver) != 0)
            {
                mustDeliver.Enqueue(slot);
                Monitor.Pulse(gate);
            }
            else
            {
                if (lossTolerantLatest.HasValue)
                {
                    dropped = lossTolerantLatest.Value;
                }
                lossTolerantLatest = slot;
                Monitor.Pulse(gate);
            }
        }

        if (dropped.HasValue)
        {
            RecordSlot(metrics, metricsGate, originId, dropped.Value, 0, false);
        }
    }

    public void CompleteDroppingPending()
    {
        List<BenchSlot> dropped = [];
        lock (gate)
        {
            if (!accepting)
            {
                return;
            }

            accepting = false;
            while (mustDeliver.Count != 0)
            {
                dropped.Add(mustDeliver.Dequeue());
            }
            if (lossTolerantLatest.HasValue)
            {
                dropped.Add(lossTolerantLatest.Value);
                lossTolerantLatest = null;
            }
            Monitor.PulseAll(gate);
        }

        foreach (var slot in dropped)
        {
            RecordSlot(metrics, metricsGate, originId, slot, 0, false);
        }
    }

    public static void RecordSlot(
        BenchMetrics metrics,
        object metricsGate,
        uint originId,
        BenchSlot slot,
        ulong sendTsNs,
        bool submitted)
    {
        var header = new BenchHeader(slot.Seq, slot.SchedTsNs, sendTsNs, slot.Flags, originId, slot.TrafficId);
        lock (metricsGate)
        {
            metrics.OnSlot(header, submitted);
        }
    }

    private async Task RunAsync()
    {
        while (true)
        {
            BenchSlot slot;
            lock (gate)
            {
                while (accepting && mustDeliver.Count == 0 && !lossTolerantLatest.HasValue)
                {
                    Monitor.Wait(gate);
                }

                if (mustDeliver.Count != 0)
                {
                    slot = mustDeliver.Dequeue();
                }
                else if (lossTolerantLatest.HasValue)
                {
                    slot = lossTolerantLatest.Value;
                    lossTolerantLatest = null;
                }
                else
                {
                    return;
                }
            }

            await SendSlotAsync(slot).ConfigureAwait(false);
        }
    }

    private async Task SendSlotAsync(BenchSlot slot)
    {
        var sendTsNs = BenchClock.NowNs();
        var header = new BenchHeader(slot.Seq, slot.SchedTsNs, sendTsNs, slot.Flags, originId, slot.TrafficId);
        var payloadSize = (slot.Flags & BenchConstants.FlagMustDeliver) != 0 ? payloadSizeMd : payloadSizeLt;
        var payload = new byte[payloadSize];
        if (!BenchPayload.TryWrite(payload, header) || !BenchPayload.TryFillBody(payload, header))
        {
            payloadValidation.Invalid();
            RecordSlot(metrics, metricsGate, originId, slot, 0, false);
            return;
        }

        if ((header.Flags & BenchConstants.FlagMustDeliver) == 0 &&
            BenchConstants.DirectionFromFlags(header.Flags) == BenchDirection.ClientToServer)
        {
            receiver.RecordInputSeq(header.Seq);
        }

        var submitted = false;
        try
        {
            await hub.SendPayloadAsync(payload).AsTask().ConfigureAwait(false);
            submitted = true;
        }
        catch
        {
            submitted = false;
        }

        lock (metricsGate)
        {
            metrics.OnSlot(header, submitted);
        }
        progress?.RecordInputLastSent((int)receiver.LocalIndex, header, submitted);
    }
}

internal readonly record struct ClientConfig(
    bool Valid,
    bool Describe,
    string Host,
    int Port,
    int Conns,
    int ProcIndex,
    uint OriginBase,
    double RateLt,
    double RateMd,
    bool BroadcastLt,
    bool BroadcastMd,
    int PayloadLt,
    int PayloadMd,
    ulong DeadlineNs,
    ulong StalenessPeriodNs,
    BenchScenarioConfig? Scenario = null)
{
    public const ulong DevWarmupNs = 200_000_000UL;
    public const ulong DevDurationNs = 2_000_000_000UL;
    public const ulong DevDrainNs = 500_000_000UL;

    public static ClientConfig Parse(string[] args)
    {
        if (!BenchScenarioConfig.TryParseArguments(args, out var scenario, out var remaining))
        {
            return Invalid();
        }
        args = remaining;
        var cfg = new MutableConfig();
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--describe")
            {
                return new ClientConfig(true, true, "", 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0);
            }
            if (args[i] == "--host" && i + 1 < args.Length)
            {
                cfg.Host = args[++i];
                cfg.HaveHost = cfg.Host.Length != 0;
                continue;
            }
            if (args[i] == "--port" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Port) &&
                cfg.Port is > 0 and <= ushort.MaxValue)
            {
                cfg.HavePort = true;
                continue;
            }
            if (args[i] == "--conns" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Conns) &&
                cfg.Conns > 0)
            {
                cfg.HaveConns = true;
                continue;
            }
            if (args[i] == "--proc-index" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.ProcIndex) &&
                cfg.ProcIndex >= 0)
            {
                cfg.HaveProcIndex = true;
                continue;
            }
            if (args[i] == "--origin-base" && i + 1 < args.Length &&
                uint.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.OriginBase))
            {
                cfg.HaveOriginBase = true;
                continue;
            }
            if (args[i] == "--rate-lt" && i + 1 < args.Length &&
                double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateLt) &&
                double.IsFinite(cfg.RateLt) && cfg.RateLt >= 0)
            {
                cfg.HaveRateLt = true;
                continue;
            }
            if (args[i] == "--rate-md" && i + 1 < args.Length &&
                double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateMd) &&
                double.IsFinite(cfg.RateMd) && cfg.RateMd >= 0)
            {
                cfg.HaveRateMd = true;
                continue;
            }
            if (args[i] == "--broadcast-lt")
            {
                cfg.BroadcastLt = true;
                continue;
            }
            if (args[i] == "--broadcast-md")
            {
                cfg.BroadcastMd = true;
                continue;
            }
            if (args[i] == "--payload" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out var payloadValue))
            {
                cfg.PayloadLt = payloadValue;
                cfg.PayloadMd = payloadValue;
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--payload-lt" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadLt))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--payload-md" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadMd))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--deadline-ns" && i + 1 < args.Length &&
                ulong.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.DeadlineNs))
            {
                cfg.HaveDeadline = true;
                continue;
            }
            if (args[i] == "--staleness-period-ns" && i + 1 < args.Length &&
                ulong.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.StalenessPeriodNs) &&
                cfg.StalenessPeriodNs > 0)
            {
                cfg.HaveStaleness = true;
                continue;
            }

            return Invalid();
        }

        if (scenario is { } activeScenario)
        {
            if (!cfg.BaseComplete)
            {
                return Invalid();
            }
            var activeTraffic = activeScenario.ClientInput ?? activeScenario.RoomPublish;
            if (activeTraffic is not { } source)
            {
                return Invalid();
            }
            cfg.RateLt = source.RateLt;
            cfg.RateMd = source.RateMd;
            cfg.PayloadLt = source.PayloadLt;
            cfg.PayloadMd = source.PayloadMd;
            cfg.BroadcastLt = activeScenario.Kind == BenchScenarioKind.RoomRelay;
            cfg.BroadcastMd = activeScenario.Kind == BenchScenarioKind.RoomRelay;
            if (!cfg.HaveDeadline)
            {
                cfg.DeadlineNs = source.DeadlineNs;
            }
            if (!cfg.HaveStaleness)
            {
                cfg.StalenessPeriodNs = BenchConstants.DefaultStalenessPeriodNs;
            }
        }
        else if (!cfg.Complete || (cfg.RateLt == 0 && cfg.RateMd == 0))
        {
            return Invalid();
        }
        // 有効な stream の payload が範囲内であること
        if (cfg.RateLt > 0 &&
            (cfg.PayloadLt < BenchConstants.MinPayloadBytes || cfg.PayloadLt > BenchConstants.MaxPayloadBytes))
        {
            return Invalid();
        }
        if (cfg.RateMd > 0 &&
            (cfg.PayloadMd < BenchConstants.MinPayloadBytes || cfg.PayloadMd > BenchConstants.MaxPayloadBytes))
        {
            return Invalid();
        }
        var endOrigin = (ulong)cfg.OriginBase + (ulong)cfg.Conns;
        if (endOrigin > uint.MaxValue ||
            (scenario is not null && endOrigin > (ulong)scenario.TotalConns))
        {
            return Invalid();
        }

        return new ClientConfig(
            true,
            false,
            cfg.Host,
            cfg.Port,
            cfg.Conns,
            cfg.ProcIndex,
            cfg.OriginBase,
            cfg.RateLt,
            cfg.RateMd,
            cfg.BroadcastLt,
            cfg.BroadcastMd,
            cfg.PayloadLt,
            cfg.PayloadMd,
            cfg.DeadlineNs,
            cfg.StalenessPeriodNs,
            scenario);
    }

    private static ClientConfig Invalid() => new(false, false, "", 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0);

    private sealed class MutableConfig
    {
        public string Host = "";
        public int Port;
        public int Conns;
        public int ProcIndex;
        public uint OriginBase;
        public double RateLt;
        public double RateMd;
        public bool BroadcastLt;
        public bool BroadcastMd;
        public int PayloadLt;
        public int PayloadMd;
        public ulong DeadlineNs;
        public ulong StalenessPeriodNs;
        public bool HaveHost;
        public bool HavePort;
        public bool HaveConns;
        public bool HaveProcIndex;
        public bool HaveOriginBase;
        public bool HaveRateLt;
        public bool HaveRateMd;
        public bool HavePayload;
        public bool HaveDeadline;
        public bool HaveStaleness;

        public bool Complete =>
            BaseComplete &&
            HaveRateLt && HaveRateMd && HavePayload && HaveDeadline && HaveStaleness;

        public bool BaseComplete => HaveHost && HavePort && HaveConns && HaveProcIndex && HaveOriginBase;
    }
}
