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
// UnsyncedReceiveEvent により受信ハンドラは受信スレッドから走る一方、
// connect/disconnect イベントは main ループの PollEvents から来る。
// fanout の読み手(受信スレッド)には copy-on-write の snapshot を渡す。
var peersBox = new PeersBox();

var listener = new EventBasedNetListener();
listener.ConnectionRequestEvent += req => req.AcceptIfKey(LnlConstants.ConnectKey);
listener.PeerConnectedEvent += peer =>
{
    connectedPeers.Add(peer);
    peersBox.Set(connectedPeers.ToArray());
    stats.Connected();
};
listener.PeerDisconnectedEvent += (peer, _) =>
{
    connectedPeers.Remove(peer);
    peersBox.Set(connectedPeers.ToArray());
    stats.Disconnected();
};
// broadcast fanout(1 受信 → 全 peer 送信)は受信スレッドで inline に行うと
// 受信そのものが滞る(受信イベントハンドラは O(1) を保つ)ため、専用の
// fanout スレッドへ逃がす。echo(O(1))は受信スレッド inline のまま。
var fanout = new FanoutWorker(peersBox, stats);

listener.NetworkReceiveEvent += (peer, reader, _, _) =>
{
    HandleReceive(peer, reader, fanout, stats);
    reader.Recycle();
};

// Single NetManager for the whole server: every client connects from a
// distinct remote endpoint, so there is no per-conn socket/thread need here
// (unlike the client — see LiteNetLibBench.Client/Program.cs). Left in the
// library's default (non-manual) mode: NetManager.Start() runs its own
// receive+logic threads, which is what "thread_model":"internal_worker"
// (--describe) reports for the server role.
//
// tune-to-plateau(全て upstream 公式ノブ。--describe の tuning に開示):
// - UnsyncedReceiveEvent: 既定では受信イベントは PollEvents までキューされ、
//   main ループの poll 粒度(≤15ms)がレイテンシと queue 成長の源泉になる。
//   受信スレッド直発火で echo/broadcast を inline 化(LiteNetManager.cs:159)
// - UpdateTime 1ms + 送信後 TriggerUpdate: Send はキューするだけで、実送信は
//   logic スレッドの tick(既定 15ms)まで出ない(LiteNetPeer.cs:1187-1213)。
//   TriggerUpdate は upstream が送信レイテンシ短縮用と明記する即時 wake
//   (LiteNetManager.cs:110-111,1307)
// - MtuDiscovery: 既定 off だと MTU 1024 固定(NetConstants.cs:89-107)。
//   有効化で 1432 まで交渉され、merge 効率と unreliable 上限が上がる
// - UseNativeSockets: recvfrom/sendto を P/Invoke 直呼びし managed Socket 層と
//   EndPoint alloc を回避(LiteNetManager.cs:252-255)
// - DisconnectTimeout 60s(既定 5s): 高負荷で相手の ping が滞った際の切断猶予
// - PacketPoolSize 16384(既定 1000): fanout の in-flight packet 数は
//   conns×burst で余裕で 1000 を超え、枯渇すると全 packet が
//   「contended pool lock + GC alloc」経路に落ちる(PoolGetPacket/
//   PoolRecycle、LiteNetManager.PacketPool.cs:42-80)。Release ビルドで
//   fanout が速くなると顕在化し c64 bcast が崩れるのを実測
var manager = new NetManager(listener)
{
    UnsyncedReceiveEvent = true,
    UpdateTime = 1,
    MtuDiscovery = true,
    UseNativeSockets = true,
    DisconnectTimeout = 60000,
    PacketPoolSize = 16384,
};
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

        // 定常判定つき warmup(benchspec v2): 確定窓(window)を受けたら
        // schedule を差し替える(drain 終端の前倒しに効く)。窓確定
        //(window 受信 or 暫定 start_at 到達)まで pump しながら poll する
        while (!stopCts.IsCancellationRequested && BenchClock.NowNs() < schedule.StartAtNs)
        {
            if (await control.PollWindowAsync(stopCts.Token).ConfigureAwait(false) is { } window)
            {
                schedule = window;
                break;
            }

            manager.PollEvents();
            await Task.Delay(15, stopCts.Token).ConfigureAwait(false);
        }

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

static void HandleReceive(NetPeer origin, NetPacketReader reader, FanoutWorker fanout, LnlServerStats stats)
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

        // Send はキューするだけ。logic スレッドを即 wake して flush する
        origin.NetManager.TriggerUpdate();
        return;
    }

    // broadcast: fanout スレッドへ委譲(受信スレッドを O(1) に保つ)
    fanout.Enqueue(origin.NetManager, span, header.Flags, deliveryMethod);
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

    // 受信系カウンタ(CountRecv/CountSubmit/InvalidPayload)は受信スレッド
    // (UnsyncedReceiveEvent)だけが書き、Connected/Disconnected は main ループ
    // (PollEvents)だけが書く。フィールドが分かれた single-writer 構成なので
    // ロック不要。ToJson は全 peer 停止後にのみ呼ぶ。
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

/// <summary>
/// connectedPeers の copy-on-write snapshot。書き手は main ループ
/// (PollEvents の connect/disconnect イベント)のみ、読み手は受信スレッド
/// (echo)と fanout スレッド(broadcast)。
/// </summary>
public sealed class PeersBox
{
    private NetPeer[] peers = Array.Empty<NetPeer>();

    public void Set(NetPeer[] next) => Volatile.Write(ref peers, next);

    public NetPeer[] Get() => Volatile.Read(ref peers);
}

/// <summary>
/// broadcast fanout 専用スレッド。受信スレッド(UnsyncedReceiveEvent)は
/// payload を pool バッファへ 1 copy して積むだけ(O(1))で戻り、
/// N peer への Send はこのスレッドが行う。submit 系カウンタ(broadcast 列)は
/// このスレッドだけが書く(single-writer 維持)。
/// キューは LiteNetLib の unreliable channel と同様に上限なし —
/// 過負荷時は破棄でなく遅延として現れる(staleness gate が検出する)。
/// </summary>
public sealed class FanoutWorker
{
    private readonly System.Collections.Concurrent.ConcurrentQueue<(LiteNetManager Manager, byte[] Buf, int Len, byte Flags, DeliveryMethod Method)> queue = new();
    private readonly AutoResetEvent wake = new(false);
    private readonly PeersBox peersBox;
    private readonly LnlServerStats stats;

    public FanoutWorker(PeersBox peersBox, LnlServerStats stats)
    {
        this.peersBox = peersBox;
        this.stats = stats;
        var thread = new Thread(Loop) { IsBackground = true, Name = "lnl-fanout" };
        thread.Start();
    }

    public void Enqueue(LiteNetManager manager, ReadOnlySpan<byte> payload, byte flags, DeliveryMethod method)
    {
        var buf = System.Buffers.ArrayPool<byte>.Shared.Rent(payload.Length);
        payload.CopyTo(buf);
        queue.Enqueue((manager, buf, payload.Length, flags, method));
        wake.Set();
    }

    private void Loop()
    {
        while (true)
        {
            wake.WaitOne(1);
            LiteNetManager? manager = null;
            while (queue.TryDequeue(out var item))
            {
                manager = item.Manager;
                var peers = peersBox.Get();
                var span = new ReadOnlySpan<byte>(item.Buf, 0, item.Len);
                ulong ok = 0;
                ulong failed = 0;
                foreach (var target in peers)
                {
                    if (target.ConnectionState == ConnectionState.Connected)
                    {
                        try
                        {
                            target.Send(span, item.Method);
                            ok++;
                        }
                        catch
                        {
                            failed++;
                        }
                    }
                    else
                    {
                        failed++;
                    }
                }

                stats.CountSubmit(item.Flags, ok, failed);
                System.Buffers.ArrayPool<byte>.Shared.Return(item.Buf);
            }

            manager?.TriggerUpdate();
        }
    }
}
