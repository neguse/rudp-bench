using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using BenchKit.CS;
using LiteNetLib;
using LiteNetLibBench;

var config = ServerConfig.Parse(args);
if (config.Describe)
{
    Console.WriteLine(LnlDescribe.Json);
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine("usage: LiteNetLibBench.Server --port PORT");
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

var stats = new LnlServerStats();
var connectedPeers = new List<NetPeer>();

var listener = new EventBasedNetListener();
listener.ConnectionRequestEvent += req => req.AcceptIfKey(LnlConstants.ConnectKey);
listener.PeerConnectedEvent += peer =>
{
    connectedPeers.Add(peer);
    stats.Connected();
};
listener.PeerDisconnectedEvent += (peer, _) =>
{
    connectedPeers.Remove(peer);
    stats.Disconnected();
};
listener.NetworkReceiveEvent += (peer, reader, _, _) =>
{
    HandleReceive(peer, reader, connectedPeers, stats);
    reader.Recycle();
};

// Single NetManager for the whole server: every client connects from a
// distinct remote endpoint, so there is no per-conn socket/thread need here
// (unlike the client — see LiteNetLibBench.Client/Program.cs). Left in the
// library's default (non-manual) mode: NetManager.Start() runs its own
// receive+logic threads, which is what "thread_model":"internal_worker"
// (--describe) reports for the server role.
var manager = new NetManager(listener);
manager.Start(config.Port);

await using var control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
try
{
    if (control is not null)
    {
        await control.HelloAsync("server", "litenetlib", 0, stopCts.Token).ConfigureAwait(false);
        await control.ReadyAsync(0, stopCts.Token).ConfigureAwait(false);

        // Poll while awaiting the schedule so client handshakes/pings are
        // serviced instead of stalling until start_at (see Shared/LnlPump.cs).
        var scheduleTask = control.WaitScheduleAsync(stopCts.Token);
        var schedule = await LnlPump.PumpWhileAwaitingAsync(
            scheduleTask,
            _ => manager.PollEvents(),
            15,
            stopCts.Token).ConfigureAwait(false);

        await LnlPump.DrainUntilAsync(
            schedule.DrainUntilNs,
            _ => manager.PollEvents(),
            stopCts.Token).ConfigureAwait(false);
    }
    else
    {
        // Standalone / smoke-test mode: no control socket, just keep servicing
        // the manager until interrupted (mirrors magiconion's dev fallback).
        while (!stopCts.IsCancellationRequested)
        {
            manager.PollEvents();
            try
            {
                await Task.Delay(15, stopCts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
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

manager.Stop();
return 0;

static void HandleReceive(NetPeer origin, NetPacketReader reader, List<NetPeer> peers, LnlServerStats stats)
{
    var len = reader.UserDataSize;
    if (len < BenchConstants.HeaderSize)
    {
        stats.InvalidPayload();
        return;
    }

    var span = new ReadOnlySpan<byte>(reader.RawData, reader.UserDataOffset, len);
    if (!BenchPayload.TryRead(span, out var header))
    {
        stats.InvalidPayload();
        return;
    }

    stats.CountRecv(header.Flags);

    // class -> transport mapping (declared in --describe): must-deliver goes
    // out ReliableOrdered, loss-tolerant goes out Unreliable. Payload bytes
    // are forwarded byte-identical (benchspec: "payload の書き換えは禁止").
    var deliveryMethod = (header.Flags & BenchConstants.FlagMustDeliver) != 0
        ? DeliveryMethod.ReliableOrdered
        : DeliveryMethod.Unreliable;

    if ((header.Flags & BenchConstants.FlagBroadcast) == 0)
    {
        if (TrySend(origin, span, deliveryMethod))
        {
            stats.CountSubmit(header.Flags, 1, 0);
        }
        else
        {
            stats.CountSubmit(header.Flags, 0, 1);
        }

        return;
    }

    // broadcast: fan out unchanged to every currently connected peer,
    // including the origin (benchspec: "現在の全接続(origin 含む)へ").
    ulong ok = 0;
    ulong failed = 0;
    foreach (var target in peers)
    {
        if (TrySend(target, span, deliveryMethod))
        {
            ok++;
        }
        else
        {
            failed++;
        }
    }

    stats.CountSubmit(header.Flags, ok, failed);
}

static bool TrySend(NetPeer peer, ReadOnlySpan<byte> payload, DeliveryMethod method)
{
    if (peer.ConnectionState != ConnectionState.Connected)
    {
        return false;
    }

    try
    {
        peer.Send(payload, method);
        return true;
    }
    catch
    {
        return false;
    }
}

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

/// <summary>
/// Per-class recv/submit accounting, mirroring
/// servers/magiconion/MagicOnionBench.Server/Program.cs's ServerStats shape
/// exactly (same JSON keys) so both transports' server DONE stats are
/// structurally comparable.
/// </summary>
public sealed class LnlServerStats
{
    private const int DistEcho = 0;
    private const int DistBroadcast = 1;

    private readonly ulong[,] recv = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] recvMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submit = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] submitMeasured = new ulong[BenchConstants.ClassCount, 2];
    private readonly ulong[,] sendFailed = new ulong[BenchConstants.ClassCount, 2];
    private ulong invalidPayload;
    private int connections;

    // All counters are only ever touched from callbacks invoked inside
    // NetManager.PollEvents(), and only one thread (the server's main loop)
    // ever calls PollEvents() — so, unlike magiconion's ServerStats, no lock
    // is needed here (see Shared/LnlPump.cs and thread_model in --describe).
    public void Connected() => connections++;

    public void Disconnected() => connections--;

    public void InvalidPayload() => invalidPayload++;

    public void CountRecv(byte flags)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        recv[cls, dist]++;
        if ((flags & BenchConstants.FlagMeasure) != 0)
        {
            recvMeasured[cls, dist]++;
        }
    }

    public void CountSubmit(byte flags, ulong okCount, ulong failedCount)
    {
        var cls = BenchConstants.ClassIndexFromFlags(flags);
        var dist = DistFromFlags(flags);
        submit[cls, dist] += okCount;
        sendFailed[cls, dist] += failedCount;
        if ((flags & BenchConstants.FlagMeasure) != 0)
        {
            submitMeasured[cls, dist] += okCount;
        }
    }

    public string ToJson()
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

    private static int DistFromFlags(byte flags) =>
        (flags & BenchConstants.FlagBroadcast) != 0 ? DistBroadcast : DistEcho;
}
