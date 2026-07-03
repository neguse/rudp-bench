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
var connections = new ConcurrentDictionary<long, ConnState>();

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

    using var socket = await context.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
    await HandleConnectionAsync(socket, stats, connections, stopCts.Token).ConfigureAwait(false);
});

await app.StartAsync(stopCts.Token).ConfigureAwait(false);

await using var control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
try
{
    if (control is not null)
    {
        await control.HelloAsync("server", "websocket", 0, stopCts.Token).ConfigureAwait(false);
        await control.ReadyAsync(0, stopCts.Token).ConfigureAwait(false);
        var schedule = await control.WaitScheduleAsync(stopCts.Token).ConfigureAwait(false);
        await DelayUntilNsAsync(schedule.DrainUntilNs, stopCts.Token).ConfigureAwait(false);
    }
    else
    {
        await Task.Delay(Timeout.InfiniteTimeSpan, stopCts.Token).ConfigureAwait(false);
    }
}
catch (OperationCanceledException)
{
}

var statsJson = stats.ToJson();
var metricsOut = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
if (!string.IsNullOrWhiteSpace(metricsOut))
{
    await File.WriteAllTextAsync(metricsOut, statsJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);
}

if (control is not null)
{
    await control.DoneAsync(statsJson, CancellationToken.None).ConfigureAwait(false);
}

await app.StopAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
return 0;

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
    ServerStats stats,
    ConcurrentDictionary<long, ConnState> connections,
    CancellationToken ct)
{
    var id = ConnIds.Next();
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
    var conn = new ConnState(socket, outbound);
    connections[id] = conn;
    stats.Connected();
    var writerTask = RunWriterAsync(conn, ct);
    var buffer = new byte[BenchConstants.MaxPayloadBytes];

    try
    {
        await ReceiveLoopAsync(socket, buffer, connections, conn, stats, ct).ConfigureAwait(false);
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
    ConcurrentDictionary<long, ConnState> connections,
    ConnState self,
    ServerStats stats,
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

        if (!BenchPayload.TryRead(buffer.AsSpan(0, offset), out var header))
        {
            stats.InvalidPayload();
            continue;
        }

        stats.CountRecv(header.Flags);
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
    private static long counter;
    public static long Next() => Interlocked.Increment(ref counter);
}

internal sealed record ConnState(WebSocket Socket, Channel<byte[]> Outbound);

internal readonly record struct ServerConfig(bool Valid, bool Describe, int Port)
{
    public static ServerConfig Parse(string[] args)
    {
        var port = 0;
        var havePort = false;
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--describe")
            {
                return new ServerConfig(true, true, 0);
            }
            if (args[i] == "--port" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out port) &&
                port is > 0 and <= ushort.MaxValue)
            {
                havePort = true;
                continue;
            }

            return new ServerConfig(false, false, 0);
        }

        return new ServerConfig(havePort, false, port);
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

    private readonly object gate = new();
    private readonly ulong[,] recv = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] recvMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submit = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submitMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] sendFailed = new ulong[BenchConstants.ClassCount, 2];
    private ulong invalidPayload;
    private int connections;

    public int ConnectionCount => Volatile.Read(ref connections);

    public void Connected() => Interlocked.Increment(ref connections);

    public void Disconnected() => Interlocked.Decrement(ref connections);

    public void InvalidPayload()
    {
        lock (gate)
        {
            invalidPayload++;
        }
    }

    public void CountRecv(byte flags)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        var measured = (flags & BenchConstants.FlagMeasure) != 0;
        lock (gate)
        {
            recv[cls, dist]++;
            if (measured)
            {
                recvMeasured[cls, dist]++;
            }
        }
    }

    public void CountSubmit(byte flags, ulong okCount, ulong failedCount)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        var measured = (flags & BenchConstants.FlagMeasure) != 0;
        lock (gate)
        {
            submit[cls, dist] += okCount;
            sendFailed[cls, dist] += failedCount;
            if (measured)
            {
                submitMeasured[cls, dist] += okCount;
            }
        }
    }

    public string ToJson()
    {
        lock (gate)
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
