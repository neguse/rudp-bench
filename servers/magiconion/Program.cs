// MagicOnion(StreamingHub、gRPC の HTTP/2 の上)の層。LT も MD も 1 本の TCP の上の
// hub の呼び出しで送るため、LT は reliable_fallback と開示する。設定は既定のまま。
// client からの送信は応答を待たない FireAndForget() で呼ぶ(呼び出しごとに往復を待つと、
// 送る間隔より往復が長い回線で送信が詰まる)。
// client は接続ごとに GrpcChannel を作る(1 接続 = 1 本の HTTP/2 接続)。
using BenchKit.CS;
using Grpc.Net.Client;
using MagicOnion;
using MagicOnion.Client;
using MagicOnion.Server.Hubs;
using Microsoft.AspNetCore.Server.Kestrel.Core;

return BenchSut.Main(args, new MagicOnionTransport());

public interface IBenchHub : IStreamingHub<IBenchHub, IBenchHubReceiver>
{
    ValueTask SendAsync(byte[] payload);
}

public interface IBenchHubReceiver
{
    void OnPayload(byte[] payload);
}

public sealed class BenchHub(MagicOnionTransport transport)
    : StreamingHubBase<IBenchHub, IBenchHubReceiver>, IBenchHub
{
    private int index = -1;

    protected override ValueTask OnConnected()
    {
        index = transport.Accept(Client);
        return ValueTask.CompletedTask;
    }

    public ValueTask SendAsync(byte[] payload)
    {
        if (index >= 0)
        {
            transport.Events.Received(index, payload);
        }
        return ValueTask.CompletedTask;
    }

    protected override ValueTask OnDisconnected()
    {
        if (index >= 0)
        {
            transport.Events.Disconnected(index);
        }
        return ValueTask.CompletedTask;
    }
}

public sealed class MagicOnionTransport : ISutTransport
{
    private sealed class Receiver(SutEventQueue events, int index) : IBenchHubReceiver
    {
        public void OnPayload(byte[] payload) => events.Received(index, payload);
    }

    private BenchSut sut = null!;
    private IBenchHubReceiver?[] serverPeers = [];
    private IBenchHub?[] senders = [];
    private readonly List<IBenchHub> owned = [];
    private WebApplication? app;
    private int accepted;

    public SutEventQueue Events { get; } = new();

    public string Name => "magiconion";

    public string Describe =>
        "\"class_mapping\":{" +
        "\"loss_tolerant\":{\"primitive\":\"streaming hub call\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"reliable_fallback\"}," +
        "\"must_deliver\":{\"primitive\":\"streaming hub call\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"native\"}}," +
        "\"coalescing\":\"none\",\"cc_algo\":\"tcp (kernel default)\",\"thread_model\":\"async\",\"encryption\":false,\"tuning\":[]";

    public int MaxPayload => 65_536;

    public int Accept(IBenchHubReceiver client)
    {
        var i = Interlocked.Increment(ref accepted) - 1;
        if (i >= serverPeers.Length)
        {
            return -1;
        }
        Volatile.Write(ref serverPeers[i], client);
        Events.Connected(i);
        return i;
    }

    public void Listen(BenchSut sut, ushort port, int maxConns)
    {
        this.sut = sut;
        serverPeers = new IBenchHubReceiver?[maxConns];
        var builder = WebApplication.CreateBuilder();
        builder.Logging.ClearProviders();
        // gRPC は TLS なしでは HTTP/2 を明示しないと使えない
        builder.WebHost.ConfigureKestrel(o => o.ListenAnyIP(port, l => l.Protocols = HttpProtocols.Http2));
        builder.Services.AddSingleton(this);
        builder.Services.AddMagicOnion();
        app = builder.Build();
        app.MapMagicOnionService();
        app.StartAsync().GetAwaiter().GetResult();
    }

    public void Connect(BenchSut sut, string host, ushort port, int conns)
    {
        this.sut = sut;
        senders = new IBenchHub?[conns];
        for (var i = 0; i < conns; ++i)
        {
            var index = i;
            _ = Task.Run(async () =>
            {
                try
                {
                    var channel = GrpcChannel.ForAddress($"http://{host}:{port}");
                    var hub = await StreamingHubClient
                        .ConnectAsync<IBenchHub, IBenchHubReceiver>(channel, new Receiver(Events, index))
                        .ConfigureAwait(false);
                    lock (owned)
                    {
                        owned.Add(hub);
                    }
                    Volatile.Write(ref senders[index], hub.FireAndForget());
                    Events.Connected(index);
                    await hub.WaitForDisconnect().ConfigureAwait(false);
                    Volatile.Write(ref senders[index], null);
                    Events.Disconnected(index);
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine($"magiconion: conn {index}: {e.Message}");
                    Events.Disconnected(index);
                }
            });
        }
    }

    public bool Send(int conn, bool mustDeliver, ReadOnlySpan<byte> data)
    {
        if (app is not null)
        {
            if (Volatile.Read(ref serverPeers[conn]) is not IBenchHubReceiver peer)
            {
                return false;
            }
            peer.OnPayload(data.ToArray());
            return true;
        }
        if (Volatile.Read(ref senders[conn]) is not IBenchHub hub)
        {
            return false;
        }
        var sent = hub.SendAsync(data.ToArray());
        if (!sent.IsCompleted)
        {
            sent.AsTask().ContinueWith(
                t => Console.Error.WriteLine($"magiconion: send failed: {t.Exception?.GetBaseException().Message}"),
                TaskContinuationOptions.OnlyOnFaulted);
        }
        return !sent.IsFaulted;
    }

    public void Service(TimeSpan timeout) => Events.Drain(sut, timeout);

    public void Dispose()
    {
        lock (owned)
        {
            foreach (var hub in owned)
            {
                hub.DisposeAsync().Wait(TimeSpan.FromSeconds(1));
            }
        }
        app?.StopAsync().GetAwaiter().GetResult();
    }
}
