using System.Collections.Concurrent;
using System.Globalization;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Channels;
using BenchKit.CS;
using Microsoft.AspNetCore.Server.Kestrel.Core;

var config = ServerConfig.Parse(args);
if (config.Describe)
{
    Console.WriteLine(BenchDescribeWs.Json());
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine("usage: WebSocketBench.Server --port PORT");
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

var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = args });
builder.Logging.ClearProviders();
builder.WebHost.ConfigureKestrel(options =>
{
    // ws:// over plain HTTP/1.1 (no TLS, no h2c needed for WebSockets on Kestrel).
    // NoDelay defaults to true in Kestrel (TCP_NODELAY on) -- left unchanged, disclosed
    // in --describe rather than set explicitly here.
    options.ListenAnyIP(config.Port, listen => listen.Protocols = HttpProtocols.Http1);
});

var stats = new ServerStats();
var connections = new ConcurrentDictionary<uint, ConnState>();
var rosterGate = new RosterGate();
var metricsGate = new object();
BenchMetrics? scenarioMetrics = null;
var progress = config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState
    ? new BenchAuthoritativeProgressTracker(0, config.Scenario.TotalConns)
    : null;
if (config.Scenario is { } scenario)
{
    scenarioMetrics = new BenchMetrics(new BenchMetricsConfig(
        checked((uint)scenario.TotalConns + 1U),
        0,
        config.StalenessPeriodNs,
        (uint)scenario.TotalConns));
    scenario.ConfigureMetrics(scenarioMetrics);
}

var app = builder.Build();
app.UseWebSockets(new WebSocketOptions
{
    // Disabled: avoid periodic ping/pong control frames on the measured path
    // during short benchmark windows. See servers/websocket/README.md.
    KeepAliveInterval = TimeSpan.Zero
});
app.Run(async context =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }

    if (!TryResolveConnectionId(context, config, out var connectionId))
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return;
    }
    if (config.Scenario is not null && rosterGate.Frozen)
    {
        context.Response.StatusCode = StatusCodes.Status409Conflict;
        return;
    }

    using var socket = await context.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
    await HandleConnectionAsync(
        socket,
        connectionId,
        stats,
        connections,
        config.Scenario,
        scenarioMetrics,
        metricsGate,
        stopCts.Token).ConfigureAwait(false);
});

await app.StartAsync(stopCts.Token).ConfigureAwait(false);

await using var control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
string reportJson;
try
{
    BenchSchedule? schedule = null;
    ConnState[]? devRoster = null;
    if (control is not null)
    {
        await control.HelloAsync("server", "websocket", 0, stopCts.Token).ConfigureAwait(false);
        await control.ReadyAsync(0, stopCts.Token).ConfigureAwait(false);
        schedule = await control.WaitScheduleAsync(stopCts.Token).ConfigureAwait(false);
    }
    else if (config.Scenario is not null)
    {
        devRoster = await FreezeRosterAsync(
            connections,
            config.Scenario.TotalConns,
            BenchClock.AddSaturating(BenchClock.NowNs(), ServerConfig.DevConnectTimeoutNs),
            stopCts.Token).ConfigureAwait(false);
        var now = BenchClock.NowNs();
        schedule = new BenchSchedule(
            BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs),
            BenchClock.AddSaturating(BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs), ServerConfig.DevDurationNs),
            BenchClock.AddSaturating(
                BenchClock.AddSaturating(BenchClock.AddSaturating(now, ServerConfig.DevWarmupNs), ServerConfig.DevDurationNs),
                ServerConfig.DevDrainNs));
    }

    if (schedule is { } activeSchedule && config.Scenario is { } activeScenario)
    {
        var roster = devRoster ?? await FreezeRosterAsync(
            connections,
            activeScenario.TotalConns,
            activeSchedule.StartAtNs,
            stopCts.Token).ConfigureAwait(false);
        rosterGate.Freeze();
        if (activeScenario.Kind == BenchScenarioKind.AuthoritativeState)
        {
            await RunAuthoritativeAsync(
                activeScenario,
                activeSchedule,
                roster,
                scenarioMetrics!,
                metricsGate,
                progress!,
                control,
                stopCts.Token).ConfigureAwait(false);
        }
        else
        {
            if (control is not null)
            {
                activeSchedule = await PollWindowUntilFinalAsync(control, activeSchedule, stopCts.Token)
                    .ConfigureAwait(false);
            }
            await DelayUntilNsAsync(activeSchedule.DrainUntilNs, stopCts.Token).ConfigureAwait(false);
        }
    }
    else if (schedule is { } legacySchedule)
    {
        legacySchedule = await PollWindowUntilFinalAsync(control!, legacySchedule, stopCts.Token).ConfigureAwait(false);
        await DelayUntilNsAsync(legacySchedule.DrainUntilNs, stopCts.Token).ConfigureAwait(false);
    }
    else
    {
        await Task.Delay(Timeout.InfiniteTimeSpan, stopCts.Token).ConfigureAwait(false);
    }
}
catch (OperationCanceledException)
{
}

if (scenarioMetrics is not null)
{
    lock (metricsGate)
    {
        reportJson = scenarioMetrics.ToJson();
    }
}
else
{
    reportJson = stats.ToJson();
}
var metricsOut = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
if (!string.IsNullOrWhiteSpace(metricsOut))
{
    await File.WriteAllTextAsync(metricsOut, reportJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);
}

var doneJson = reportJson;
if (progress is not null)
{
    var snapshot = progress.ServerSnapshot();
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

// 計測は done で報告済み。loss 下では hung socket の graceful shutdown が
// orchestrator の ProcessExitTimeout(5s)を食い潰すため、停止は短く見切り、
// それでも返らなければ即時終了する(ledger #10 の server 側)
var stop = app.StopAsync(TimeSpan.FromSeconds(1));
if (await Task.WhenAny(stop, Task.Delay(2000)).ConfigureAwait(false) != stop)
{
    Environment.Exit(0);
}
return 0;

static bool TryResolveConnectionId(HttpContext context, ServerConfig config, out uint connectionId)
{
    if (config.Scenario is null)
    {
        connectionId = ConnIds.Next();
        return true;
    }

    return uint.TryParse(
               context.Request.Query["id"],
               NumberStyles.None,
               CultureInfo.InvariantCulture,
               out connectionId) &&
           connectionId < (uint)config.Scenario.TotalConns;
}

static async Task<ConnState[]> FreezeRosterAsync(
    ConcurrentDictionary<uint, ConnState> connections,
    int totalConns,
    ulong deadlineNs,
    CancellationToken cancellationToken)
{
    while (!cancellationToken.IsCancellationRequested && BenchClock.NowNs() < deadlineNs)
    {
        if (connections.Count == totalConns)
        {
            var roster = new ConnState[totalConns];
            var complete = true;
            for (var i = 0; i < totalConns; i++)
            {
                if (!connections.TryGetValue((uint)i, out roster[i]!))
                {
                    complete = false;
                    break;
                }
            }
            if (complete)
            {
                return roster;
            }
        }
        await Task.Delay(5, cancellationToken).ConfigureAwait(false);
    }
    throw new InvalidOperationException(
        $"connection roster incomplete: got {connections.Count}, want {totalConns}");
}

static async Task RunAuthoritativeAsync(
    BenchScenarioConfig scenario,
    BenchSchedule initialSchedule,
    ConnState[] roster,
    BenchMetrics metrics,
    object metricsGate,
    BenchAuthoritativeProgressTracker progress,
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
        lock (metricsGate)
        {
            foreach (var conn in roster)
            {
                metrics.ExpectLatest(
                    conn.GlobalId,
                    conn.GlobalId,
                    input.TrafficId,
                    BenchDirection.ClientToServer,
                    schedule.StartAtNs);
            }
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
            lock (metricsGate)
            {
                metrics.Tick(now);
            }
        }

        if (now < schedule.StopAtNs)
        {
            while (plan.TryNext(now, out var slot))
            {
                progress.RecordServerStateTick(slot);
                foreach (var target in roster)
                {
                    await SubmitStateAsync(target, state, slot, metrics, metricsGate, cancellationToken)
                        .ConfigureAwait(false);
                }
            }
        }
        else if (!markedUnsent)
        {
            MarkServerUnsent(plan, roster.Length, scenario.TotalConns, schedule.StopAtNs, metrics, metricsGate);
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
        MarkServerUnsent(plan, roster.Length, scenario.TotalConns, schedule.StopAtNs, metrics, metricsGate);
    }
}

static async Task SubmitStateAsync(
    ConnState target,
    BenchTrafficConfig state,
    BenchSlot slot,
    BenchMetrics metrics,
    object metricsGate,
    CancellationToken cancellationToken)
{
    var sendTsNs = BenchClock.NowNs();
    var header = new BenchHeader(
        slot.Seq,
        slot.SchedTsNs,
        sendTsNs,
        slot.Flags,
        target.ServerOriginId,
        slot.TrafficId);
    var payload = new byte[state.PayloadSize((slot.Flags & BenchConstants.FlagMustDeliver) != 0)];
    var encoded = BenchPayload.TryWrite(payload, header) &&
                  BenchPayload.TryWriteAppliedInputSeq(payload, target.ReadAppliedInputSeq()) &&
                  BenchPayload.TryFillTargetPad(payload, target.GlobalId);
    var submitted = encoded && await TryEnqueueAsync(target, payload, cancellationToken).ConfigureAwait(false);
    lock (metricsGate)
    {
        metrics.OnSlot(header, submitted);
    }
}

static void MarkServerUnsent(
    BenchPlan plan,
    int rosterSize,
    int totalConns,
    ulong stopAtNs,
    BenchMetrics metrics,
    object metricsGate)
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
        lock (metricsGate)
        {
            for (var i = 0; i < rosterSize; i++)
            {
                metrics.OnSlot(header, false);
            }
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

static async Task HandleConnectionAsync(
    WebSocket socket,
    uint id,
    ServerStats stats,
    ConcurrentDictionary<uint, ConnState> connections,
    BenchScenarioConfig? scenario,
    BenchMetrics? metrics,
    object metricsGate,
    CancellationToken ct)
{
    // Bounded, not unbounded: caps worst-case per-connection memory. FullMode=Wait
    // means a full queue applies backpressure (awaited by the writer/broadcaster)
    // rather than dropping -- app-level dropping would corrupt the must-deliver
    // guarantee this transport relies on the kernel TCP stack for. A slow reader's
    // queue filling up (and the resulting backpressure into other connections'
    // receive loops during broadcast) is exactly the HoL-delay cost this transport
    // is supposed to expose, not something to paper over.
    var outbound = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(ServerTuning.OutboundQueueCapacity)
    {
        FullMode = BoundedChannelFullMode.Wait,
        SingleReader = true,
        SingleWriter = false
    });
    var serverOriginId = scenario is null ? 0U : (uint)scenario.TotalConns;
    var conn = new ConnState(id, serverOriginId, socket, outbound);
    if (!connections.TryAdd(id, conn))
    {
        outbound.Writer.TryComplete();
        return;
    }
    stats.Connected();
    var writerTask = RunWriterAsync(conn, ct);
    var buffer = new byte[BenchConstants.MaxPayloadBytes];

    try
    {
        await ReceiveLoopAsync(
            socket,
            buffer,
            connections,
            conn,
            stats,
            scenario,
            metrics,
            metricsGate,
            ct).ConfigureAwait(false);
    }
    catch (OperationCanceledException)
    {
    }
    catch (WebSocketException)
    {
    }
    finally
    {
        connections.TryRemove(id, out _);
        stats.Disconnected();
        outbound.Writer.TryComplete();
        try
        {
            await writerTask.ConfigureAwait(false);
        }
        catch
        {
        }

        try
        {
            if (socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
            {
                await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None)
                    .ConfigureAwait(false);
            }
        }
        catch
        {
        }
    }
}

static async Task ReceiveLoopAsync(
    WebSocket socket,
    byte[] buffer,
    ConcurrentDictionary<uint, ConnState> connections,
    ConnState self,
    ServerStats stats,
    BenchScenarioConfig? scenario,
    BenchMetrics? metrics,
    object metricsGate,
    CancellationToken ct)
{
    while (true)
    {
        var offset = 0;
        WebSocketReceiveResult result;
        while (true)
        {
            if (offset >= buffer.Length)
            {
                // Oversized message (> max_payload_bytes): not a valid bench frame,
                // drop the connection rather than silently truncating/corrupting it.
                stats.InvalidPayload();
                return;
            }

            result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer, offset, buffer.Length - offset), ct)
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
            stats.InvalidPayload();
            continue;
        }

        if (!BenchPayload.TryRead(buffer.AsSpan(0, offset), out var header))
        {
            stats.InvalidPayload();
            continue;
        }
        if (!BenchPayload.ValidateBody(buffer.AsSpan(0, offset), header))
        {
            stats.InvalidPayload();
            continue;
        }

        stats.CountRecv(header.Flags);
        if (scenario?.Kind == BenchScenarioKind.AuthoritativeState)
        {
            var input = scenario.ClientInput!.Value;
            if (header.OriginId != self.GlobalId ||
                header.TrafficId != input.TrafficId ||
                (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ClientToServer ||
                !input.Accepts(header.Flags, offset))
            {
                stats.InvalidPayload();
                continue;
            }
            if ((header.Flags & BenchConstants.FlagMustDeliver) == 0)
            {
                self.ApplyInputSeq(header.Seq);
            }
            lock (metricsGate)
            {
                metrics!.OnRecv(self.GlobalId, header, BenchClock.NowNs());
            }
            continue;
        }
        // payload passthrough must be byte-identical; copy off the shared receive
        // buffer before handing it to (potentially several) outbound queues.
        var payload = buffer.AsSpan(0, offset).ToArray();
        if ((header.Flags & BenchConstants.FlagBroadcast) == 0)
        {
            var ok = await TryEnqueueAsync(self, payload, ct).ConfigureAwait(false);
            stats.CountSubmit(header.Flags, ok ? 1UL : 0UL, ok ? 0UL : 1UL);
        }
        else
        {
            // Snapshot of current connections (including origin) -- benchspec:
            // expected recv count for one broadcast message = connection count at
            // fanout time.
            ulong okCount = 0;
            ulong failedCount = 0;
            foreach (var target in connections.Values)
            {
                if (await TryEnqueueAsync(target, payload, ct).ConfigureAwait(false))
                {
                    okCount++;
                }
                else
                {
                    failedCount++;
                }
            }

            stats.CountSubmit(header.Flags, okCount, failedCount);
        }
    }
}

static async ValueTask<bool> TryEnqueueAsync(ConnState conn, byte[] payload, CancellationToken ct)
{
    try
    {
        await conn.Outbound.Writer.WriteAsync(payload, ct).ConfigureAwait(false);
        return true;
    }
    catch
    {
        return false;
    }
}

static async Task RunWriterAsync(ConnState conn, CancellationToken ct)
{
    try
    {
        await foreach (var payload in conn.Outbound.Reader.ReadAllAsync(ct).ConfigureAwait(false))
        {
            try
            {
                // One send at a time per WebSocket is required by System.Net.WebSockets;
                // this loop is the sole writer for `conn.Socket`.
                await conn.Socket.SendAsync(payload, WebSocketMessageType.Binary, true, ct).ConfigureAwait(false);
            }
            catch
            {
                conn.Outbound.Writer.TryComplete();
                break;
            }
        }
    }
    catch (OperationCanceledException)
    {
    }
    catch (ChannelClosedException)
    {
    }
}

internal static class ServerTuning
{
    // Bounded outbound queue capacity per connection (messages, not bytes). Large
    // enough that normal bench rates/durations never hit it; finite so a stalled
    // reader cannot grow memory without limit. See HandleConnectionAsync for the
    // no-drop (FullMode=Wait) policy this pairs with.
    public const int OutboundQueueCapacity = 4096;
}

internal static class ConnIds
{
    private static int counter;
    public static uint Next() => (uint)Interlocked.Increment(ref counter);
}

internal sealed class ConnState(
    uint globalId,
    uint serverOriginId,
    WebSocket socket,
    Channel<byte[]> outbound)
{
    private ulong appliedInputSeq;

    public uint GlobalId { get; } = globalId;
    public uint ServerOriginId { get; } = serverOriginId;
    public WebSocket Socket { get; } = socket;
    public Channel<byte[]> Outbound { get; } = outbound;

    public void ApplyInputSeq(ulong seq) => BenchProgress.UpdateMax(ref appliedInputSeq, seq);

    public ulong ReadAppliedInputSeq() => Volatile.Read(ref appliedInputSeq);
}

internal sealed class RosterGate
{
    private int frozen;

    public bool Frozen => Volatile.Read(ref frozen) != 0;

    public void Freeze() => Interlocked.Exchange(ref frozen, 1);
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

internal static class BenchDescribeWs
{
    public static string Json()
    {
        var sb = new StringBuilder(1024);
        sb.Append("{\"transport\":\"websocket\",")
            .Append("\"class_mapping\":{")
            .Append("\"loss_tolerant\":{\"primitive\":\"reliable-stream\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"reliable_fallback\"},")
            .Append("\"must_deliver\":{\"primitive\":\"reliable-stream\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"native\"}},")
            .Append("\"coalescing\":\"none\",")
            .Append("\"cc_algo\":\"").Append(JsonEscape(CcAlgo())).Append("\",")
            .Append("\"thread_model\":\"async/task-based\",")
            .Append("\"encryption\":false,")
            .Append("\"max_payload_bytes\":").Append(BenchConstants.MaxPayloadBytes.ToString(CultureInfo.InvariantCulture)).Append(',')
            .Append("\"scenarios\":[\"environment_baseline\",\"authoritative_state\",\"room_relay\"],")
            .Append("\"tuning\":[")
            .Append("{\"knob\":\"Kestrel.ListenOptions.Protocols\",\"value\":\"Http1 (ws:// plaintext, no TLS)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/fundamentals/websockets\"},")
            .Append("{\"knob\":\"Kestrel.ListenOptions.NoDelay\",\"value\":\"true (library default, unchanged; TCP_NODELAY on)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/dotnet/api/microsoft.aspnetcore.server.kestrel.core.listenoptions.nodelay\"},")
            .Append("{\"knob\":\"WebSocketOptions.KeepAliveInterval\",\"value\":\"TimeSpan.Zero (disabled; no ping/pong control frames on the measured path)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/fundamentals/websockets#configure-the-middleware\"},")
            .Append("{\"knob\":\"permessage-deflate compression\",\"value\":\"disabled (library default; UseWebSockets does not enable DangerousDeflateOptions)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/fundamentals/websockets\"},")
            .Append("{\"knob\":\"outbound queue policy\",\"value\":\"per-connection bounded channel, capacity ")
            .Append(ServerTuning.OutboundQueueCapacity.ToString(CultureInfo.InvariantCulture))
            .Append(" messages, FullMode=Wait (backpressure, no app-level drops)\",")
            .Append("\"upstream_ref\":\"https://learn.microsoft.com/dotnet/api/system.threading.channels.boundedchannelfullmode\"}")
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

public sealed class ServerStats
{
    private const int DistEcho = 0;
    private const int DistBroadcast = 1;

    // カウンタは Interlocked: 受信ループは接続ごとの async タスク(複数
    // thread-pool スレッド)で並行に走るため、単一 lock だと全接続が
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
