using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
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
builder.Services.AddMagicOnion();

var app = builder.Build();
app.MapMagicOnionService();
await app.StartAsync(stopCts.Token).ConfigureAwait(false);

var stats = app.Services.GetRequiredService<ServerStats>();
await using var control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
try
{
    if (control is not null)
    {
        await control.HelloAsync("server", "magiconion", 0, stopCts.Token).ConfigureAwait(false);
        await control.ReadyAsync(0, stopCts.Token).ConfigureAwait(false);
        var schedule = await control.WaitScheduleAsync(stopCts.Token).ConfigureAwait(false);
        // 定常判定つき warmup(benchspec v2): 確定窓(window)を受けたら
        // schedule を差し替える(drain 終端の前倒しに効く)
        schedule = await PollWindowUntilFinalAsync(control, schedule, stopCts.Token).ConfigureAwait(false);
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

public sealed class BenchHub(ServerStats stats) : StreamingHubBase<IBenchHub, IBenchHubReceiver>, IBenchHub
{
    private const string GroupName = "bench";
    private IGroup<IBenchHubReceiver>? room;
    private bool joined;

    public async ValueTask JoinAsync()
    {
        if (joined)
        {
            return;
        }

        room = await Group.AddAsync(GroupName).ConfigureAwait(false);
        joined = true;
        stats.Connected();
    }

    public ValueTask SendPayloadAsync(byte[] payload)
    {
        if (!BenchPayload.TryRead(payload, out var header))
        {
            stats.InvalidPayload();
            return ValueTask.CompletedTask;
        }

        stats.CountRecv(header.Flags);
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
            stats.Disconnected();
        }

        return ValueTask.CompletedTask;
    }
}
