using System.Globalization;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using BenchKit.CS;

var config = ClientConfig.Parse(args);

// 計測器(client farm)側: ThreadPool の hill-climbing 起動遅延が送信 pacing の
// stall(数百 ms 級)として観測されるため、最低スレッド数を先に確保する。
// client 専用の farm 設定であり server には入れない
System.Threading.ThreadPool.SetMinThreads(Environment.ProcessorCount * 4, Environment.ProcessorCount * 4);

if (config.Describe)
{
    Console.WriteLine(BenchDescribeWs.Json());
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine(
        "usage: WebSocketBench.Client --host HOST --port PORT --conns N --proc-index N " +
        "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES " +
        "[--payload-lt BYTES] [--payload-md BYTES] " +
        "--deadline-ns NS --staleness-period-ns NS");
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
        await control.HelloAsync("client", "websocket", config.ProcIndex, stopCts.Token).ConfigureAwait(false);
    }

    for (var i = 0; i < config.Conns; i++)
    {
        var originId = config.OriginBase + (uint)i;
        var uri = new Uri(FormatUri(config.Host, config.Port, config.Scenario is null ? null : originId));
        // for 変数 i を lambda に直接キャプチャすると、Task.Run の起動が
        // ループ前進より遅れた場合に複数の受信ループが同じ index を読む
        // (dedup キー破壊 → 偽 duplicates)。ループ内ローカルに固定する
        var localIndex = (uint)i;
        var ws = new ClientWebSocket();
        // Disabled: matches server-side KeepAliveInterval=0 -- no ping/pong control
        // frames on the measured path. See servers/websocket/README.md.
        ws.Options.KeepAliveInterval = TimeSpan.Zero;
        await ws.ConnectAsync(uri, stopCts.Token).ConfigureAwait(false);
        var connection = new ClientConnection(originId, (int)localIndex, ws);
        connection.ReceiveTask = Task.Run(
            () => ReceiveLoopAsync(
                connection,
                localIndex,
                config.Scenario,
                metrics,
                metricsGate,
                payloadValidation,
                progress,
                stopCts.Token),
            CancellationToken.None);
        connections.Add(connection);
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
            progress,
            payloadValidation);
    }

    var markedUnsent = false;
    var steady = new BenchSteady();
    while (!stopCts.IsCancellationRequested && BenchClock.NowNs() < schedule.DrainUntilNs)
    {
        var now = BenchClock.NowNs();
        // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
        // raw counts は metricsGate 下で読む(receive loop/pipe スレッドが書く)。
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
            // loss 下では in-flight の SendAsync が HoL で無期限に塞がりうる。
            // 計測窓は閉じているので一定時間で見切り、Abort で送信を解放する
            // (in-flight slot は SendSlotAsync の例外経路で unsent として記録される)
            var completed = await Task.WhenAny(connection.SendPipe.Completion, Task.Delay(2000)).ConfigureAwait(false);
            if (completed != connection.SendPipe.Completion)
            {
                connection.Ws.Abort();
                try
                {
                    await connection.SendPipe.Completion.ConfigureAwait(false);
                }
                catch
                {
                }
            }
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

static string FormatUri(string host, int port, uint? connectionId)
{
    var formattedHost = host.Contains(':', StringComparison.Ordinal) &&
                        !host.StartsWith('[')
        ? "[" + host + "]"
        : host;
    var uri = "ws://" + formattedHost + ":" + port.ToString(CultureInfo.InvariantCulture) + "/bench";
    return connectionId is { } id
        ? uri + "?id=" + id.ToString(CultureInfo.InvariantCulture)
        : uri;
}

static string MetricsPathOrDefault()
{
    var path = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
    if (!string.IsNullOrWhiteSpace(path))
    {
        return path;
    }

    return "/tmp/rudp-bench-websocket-client-" +
           Environment.ProcessId.ToString(CultureInfo.InvariantCulture) + ".json";
}

static async Task ReceiveLoopAsync(
    ClientConnection connection,
    uint localIndex,
    BenchScenarioConfig? scenario,
    BenchMetrics metrics,
    object metricsGate,
    BenchPayloadValidation payloadValidation,
    BenchAuthoritativeProgressTracker? progress,
    CancellationToken ct)
{
    var ws = connection.Ws;
    var buffer = new byte[BenchConstants.MaxPayloadBytes];
    try
    {
        while (true)
        {
            var offset = 0;
            WebSocketReceiveResult result;
            while (true)
            {
                if (offset >= buffer.Length)
                {
                    payloadValidation.Invalid();
                    return;
                }

                result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer, offset, buffer.Length - offset), ct)
                    .ConfigureAwait(false);
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    return;
                }

                offset += result.Count;
                if (result.EndOfMessage)
                {
                    break;
                }
            }

            if (result.MessageType != WebSocketMessageType.Binary)
            {
                payloadValidation.Invalid();
                continue;
            }

            if (!BenchPayload.TryRead(buffer.AsSpan(0, offset), out var header))
            {
                payloadValidation.Invalid();
                continue;
            }
            if (scenario?.Kind == BenchScenarioKind.AuthoritativeState)
            {
                var state = scenario.ServerState!.Value;
                if (header.OriginId != (uint)scenario.TotalConns ||
                    header.TrafficId != state.TrafficId ||
                    (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                    BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ServerToClient ||
                    !state.Accepts(header.Flags, offset) ||
                    !BenchPayload.TryReadAppliedInputSeq(buffer.AsSpan(0, offset), out var appliedInputSeq) ||
                    appliedInputSeq > connection.ReadLastInputSeq() ||
                    !BenchPayload.ValidateTargetPad(buffer.AsSpan(0, offset), connection.OriginId))
                {
                    payloadValidation.Invalid();
                    continue;
                }
                connection.RecordAppliedInputSeq(appliedInputSeq);
                progress?.RecordStateReceived((int)localIndex, header, appliedInputSeq);
            }
            else if (!BenchPayload.ValidateBody(buffer.AsSpan(0, offset), header))
            {
                payloadValidation.Invalid();
                continue;
            }

            var recvTsNs = BenchClock.NowNs();
            lock (metricsGate)
            {
                metrics.OnRecv(localIndex, header, recvTsNs);
            }
        }
    }
    catch (OperationCanceledException)
    {
    }
    catch (WebSocketException)
    {
    }
    catch (ObjectDisposedException)
    {
    }
}

internal sealed class ClientConnection(uint originId, int localIndex, ClientWebSocket ws) : IAsyncDisposable
{
    private ulong lastInputSeq;
    private ulong lastAppliedInputSeq;

    public uint OriginId { get; } = originId;
    public int LocalIndex { get; } = localIndex;
    public ClientWebSocket Ws { get; } = ws;
    public BenchPlan? Plan { get; set; }
    public SendPipe? SendPipe { get; set; }
    public Task? ReceiveTask { get; set; }

    public void RecordInputSeq(ulong seq)
    {
        BenchProgress.UpdateMax(ref lastInputSeq, seq);
    }

    public ulong ReadLastInputSeq() => Volatile.Read(ref lastInputSeq);

    public void RecordAppliedInputSeq(ulong seq)
    {
        BenchProgress.UpdateMax(ref lastAppliedInputSeq, seq);
    }

    public async ValueTask DisposeAsync()
    {
        if (SendPipe is not null)
        {
            SendPipe.CompleteDroppingPending();
            var completed = await Task.WhenAny(SendPipe.Completion, Task.Delay(2000)).ConfigureAwait(false);
            if (completed != SendPipe.Completion)
            {
                Ws.Abort();
            }
        }

        try
        {
            if (Ws.State == WebSocketState.Open)
            {
                // graceful close も loss 下では返らないことがある — 有界にする
                using var closeCts = new CancellationTokenSource(1000);
                await Ws.CloseAsync(WebSocketCloseStatus.NormalClosure, null, closeCts.Token).ConfigureAwait(false);
            }
        }
        catch
        {
            Ws.Abort();
        }

        if (ReceiveTask is not null)
        {
            try
            {
                await Task.WhenAny(ReceiveTask, Task.Delay(1000)).ConfigureAwait(false);
            }
            catch
            {
            }
        }

        Ws.Dispose();
    }
}

// coalescing: "none" -- unlike magiconion's SendPipe (which overwrites a pending
// loss-tolerant slot with the newer one, i.e. app-level latest-value coalescing),
// this queue never drops or replaces a slot that TryNext() already committed to.
// Everything submitted while accepting is true is sent, in submission order, on
// the single reliable stream. Slots still queued when CompleteDroppingPending()
// runs (i.e. never got a chance to be submitted to the socket before stop_at) are
// the only ones recorded as unsent -- matching the magiconion backpressure pattern,
// just without the loss-tolerant overwrite path.
internal sealed class SendPipe
{
    private readonly ClientConnection connection;
    private readonly ClientWebSocket ws;
    private readonly uint originId;
    private readonly int payloadSizeLt;
    private readonly int payloadSizeMd;
    private readonly BenchMetrics metrics;
    private readonly object metricsGate;
    private readonly BenchAuthoritativeProgressTracker? progress;
    private readonly BenchPayloadValidation payloadValidation;
    private readonly object gate = new();
    private readonly Queue<BenchSlot> queue = new();
    private bool accepting = true;

    public SendPipe(
        ClientConnection connection,
        int payloadSizeLt,
        int payloadSizeMd,
        BenchMetrics metrics,
        object metricsGate,
        BenchAuthoritativeProgressTracker? progress,
        BenchPayloadValidation payloadValidation)
    {
        this.connection = connection;
        ws = connection.Ws;
        originId = connection.OriginId;
        this.payloadSizeLt = payloadSizeLt;
        this.payloadSizeMd = payloadSizeMd;
        this.metrics = metrics;
        this.metricsGate = metricsGate;
        this.progress = progress;
        this.payloadValidation = payloadValidation;
        Completion = Task.Run(RunAsync);
    }

    public Task Completion { get; }

    public void Submit(BenchSlot slot)
    {
        lock (gate)
        {
            if (accepting)
            {
                queue.Enqueue(slot);
                Monitor.Pulse(gate);
                return;
            }
        }

        RecordSlot(metrics, metricsGate, originId, slot, 0, false);
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
            while (queue.Count != 0)
            {
                dropped.Add(queue.Dequeue());
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
                while (accepting && queue.Count == 0)
                {
                    Monitor.Wait(gate);
                }

                if (queue.Count != 0)
                {
                    slot = queue.Dequeue();
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
            connection.RecordInputSeq(slot.Seq);
        }

        var submitted = false;
        try
        {
            // Raw WebSocket send: only one SendAsync at a time per socket. This is
            // the only writer for `ws` (server-originated bytes go through the
            // ReceiveLoopAsync side of the same connection, not this send path).
            await ws.SendAsync(new ArraySegment<byte>(payload), WebSocketMessageType.Binary, true, CancellationToken.None)
                .ConfigureAwait(false);
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
        progress?.RecordInputLastSent(connection.LocalIndex, header, submitted);
    }
}

internal static class BenchDescribeWs
{
    public static string Json()
    {
        var sb = new StringBuilder(1024);
        sb.Append("{\"transport\":\"websocket\",")
            .Append("\"class_mapping\":{\"loss_tolerant\":\"reliable-stream\",\"must_deliver\":\"reliable-stream\"},")
            .Append("\"coalescing\":\"none\",")
            .Append("\"cc_algo\":\"").Append(JsonEscape(CcAlgo())).Append("\",")
            .Append("\"thread_model\":\"async/task-based\",")
            .Append("\"encryption\":false,")
            .Append("\"max_payload_bytes\":").Append(BenchConstants.MaxPayloadBytes.ToString(CultureInfo.InvariantCulture)).Append(',')
            .Append("\"scenarios\":[\"environment_baseline\",\"authoritative_state\",\"room_relay\"],")
            .Append("\"tuning\":[")
            .Append("{\"knob\":\"ClientWebSocketOptions.KeepAliveInterval\",\"value\":\"TimeSpan.Zero (disabled; no ping/pong control frames on the measured path)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/dotnet/api/system.net.websockets.clientwebsocketoptions.keepaliveinterval\"},")
            .Append("{\"knob\":\"permessage-deflate compression\",\"value\":\"disabled (library default; ClientWebSocketOptions.DangerousDeflateOptions not set)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/dotnet/api/system.net.websockets.clientwebsocketoptions\"},")
            .Append("{\"knob\":\"Socket.NoDelay (via ClientWebSocket)\",\"value\":\"true (library default, unchanged; TCP_NODELAY on)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/dotnet/api/system.net.sockets.socket.nodelay\"}")
            .Append("]}");
        return sb.ToString();
    }

    private static string CcAlgo()
    {
        try
        {
            var value = File.ReadAllText("/proc/sys/net/ipv4/tcp_congestion_control").Trim();
            return value.Length == 0 ? "kernel-tcp(cubic)" : "kernel-tcp(" + value + ")";
        }
        catch
        {
            return "kernel-tcp(cubic)";
        }
    }

    private static string JsonEscape(string value) => value.Replace("\\", "\\\\").Replace("\"", "\\\"");
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
        var cfg = new MutableConfig();
        for (var i = 0; i < remaining.Length; i++)
        {
            if (remaining[i] == "--describe")
            {
                return new ClientConfig(true, true, "", 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0);
            }
            if (remaining[i] == "--host" && i + 1 < remaining.Length)
            {
                cfg.Host = remaining[++i];
                cfg.HaveHost = cfg.Host.Length != 0;
                continue;
            }
            if (remaining[i] == "--port" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Port) &&
                cfg.Port is > 0 and <= ushort.MaxValue)
            {
                cfg.HavePort = true;
                continue;
            }
            if (remaining[i] == "--conns" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Conns) &&
                cfg.Conns > 0)
            {
                cfg.HaveConns = true;
                continue;
            }
            if (remaining[i] == "--proc-index" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.ProcIndex) &&
                cfg.ProcIndex >= 0)
            {
                cfg.HaveProcIndex = true;
                continue;
            }
            if (remaining[i] == "--origin-base" && i + 1 < remaining.Length &&
                uint.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.OriginBase))
            {
                cfg.HaveOriginBase = true;
                continue;
            }
            if (remaining[i] == "--rate-lt" && i + 1 < remaining.Length &&
                double.TryParse(remaining[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateLt) &&
                double.IsFinite(cfg.RateLt) && cfg.RateLt >= 0)
            {
                cfg.HaveRateLt = true;
                continue;
            }
            if (remaining[i] == "--rate-md" && i + 1 < remaining.Length &&
                double.TryParse(remaining[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateMd) &&
                double.IsFinite(cfg.RateMd) && cfg.RateMd >= 0)
            {
                cfg.HaveRateMd = true;
                continue;
            }
            if (remaining[i] == "--broadcast-lt")
            {
                cfg.BroadcastLt = true;
                continue;
            }
            if (remaining[i] == "--broadcast-md")
            {
                cfg.BroadcastMd = true;
                continue;
            }
            if (remaining[i] == "--payload" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out var payloadValue))
            {
                cfg.PayloadLt = payloadValue;
                cfg.PayloadMd = payloadValue;
                cfg.HavePayload = true;
                continue;
            }
            if (remaining[i] == "--payload-lt" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadLt))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (remaining[i] == "--payload-md" && i + 1 < remaining.Length &&
                int.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadMd))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (remaining[i] == "--deadline-ns" && i + 1 < remaining.Length &&
                ulong.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.DeadlineNs))
            {
                cfg.HaveDeadline = true;
                continue;
            }
            if (remaining[i] == "--staleness-period-ns" && i + 1 < remaining.Length &&
                ulong.TryParse(remaining[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.StalenessPeriodNs) &&
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
