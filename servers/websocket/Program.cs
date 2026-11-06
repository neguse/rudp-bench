// WebSocket(ASP.NET Core と ClientWebSocket)の層。LT も MD も 1 本の TCP の上の
// binary message で送るため、LT は reliable_fallback と開示する。設定は既定のまま。
using System.Net.WebSockets;
using BenchKit.CS;

return BenchSut.Main(args, new WebSocketTransport());

internal sealed class WebSocketTransport : ISutTransport
{
    private readonly SutEventQueue events = new();
    private BenchSut sut = null!;
    private WebSocket?[] sockets = [];
    private SerialSender?[] senders = [];
    private WebApplication? app;
    private int accepted;

    public string Name => "websocket";

    public string Describe =>
        "\"class_mapping\":{" +
        "\"loss_tolerant\":{\"primitive\":\"websocket binary message\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"reliable_fallback\"}," +
        "\"must_deliver\":{\"primitive\":\"websocket binary message\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"native\"}}," +
        "\"coalescing\":\"none\",\"cc_algo\":\"tcp (kernel default)\",\"thread_model\":\"async\",\"encryption\":false,\"tuning\":[]";

    public int MaxPayload => 65_536;

    public void Listen(BenchSut sut, ushort port, int maxConns)
    {
        this.sut = sut;
        sockets = new WebSocket?[maxConns];
        senders = new SerialSender?[maxConns];
        var builder = WebApplication.CreateBuilder();
        builder.Logging.ClearProviders();
        builder.WebHost.ConfigureKestrel(o => o.ListenAnyIP(port));
        app = builder.Build();
        app.UseWebSockets();
        app.Run(async context =>
        {
            if (!context.WebSockets.IsWebSocketRequest)
            {
                context.Response.StatusCode = 400;
                return;
            }
            var ws = await context.WebSockets.AcceptWebSocketAsync().ConfigureAwait(false);
            var i = Interlocked.Increment(ref accepted) - 1;
            if (i >= maxConns)
            {
                await ws.CloseAsync(WebSocketCloseStatus.PolicyViolation, null, CancellationToken.None).ConfigureAwait(false);
                return;
            }
            await RunAsync(i, ws).ConfigureAwait(false);
        });
        app.StartAsync().GetAwaiter().GetResult();
    }

    public void Connect(BenchSut sut, string host, ushort port, int conns)
    {
        this.sut = sut;
        sockets = new WebSocket?[conns];
        senders = new SerialSender?[conns];
        for (var i = 0; i < conns; ++i)
        {
            var index = i;
            _ = Task.Run(async () =>
            {
                var ws = new ClientWebSocket();
                try
                {
                    await ws.ConnectAsync(new Uri($"ws://{host}:{port}/"), CancellationToken.None).ConfigureAwait(false);
                }
                catch (Exception e)
                {
                    Console.Error.WriteLine($"websocket: connect failed: {e.Message}");
                    events.Disconnected(index);
                    return;
                }
                await RunAsync(index, ws).ConfigureAwait(false);
            });
        }
    }

    private async Task RunAsync(int i, WebSocket ws)
    {
        var sender = new SerialSender();
        Volatile.Write(ref senders[i], sender);
        sockets[i] = ws;
        events.Connected(i);
        var writer = sender.RunAsync(p => ws.SendAsync(p.AsMemory(), WebSocketMessageType.Binary, true, CancellationToken.None).AsTask());
        try
        {
            var buffer = new byte[MaxPayload];
            while (true)
            {
                var len = 0;
                ValueWebSocketReceiveResult r;
                do
                {
                    r = await ws.ReceiveAsync(buffer.AsMemory(len), CancellationToken.None).ConfigureAwait(false);
                    len += r.Count;
                }
                while (!r.EndOfMessage && r.MessageType != WebSocketMessageType.Close && len < buffer.Length);
                if (r.MessageType == WebSocketMessageType.Close)
                {
                    break;
                }
                events.Received(i, buffer.AsSpan(0, len).ToArray());
            }
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"websocket: conn {i}: {e.Message}");
        }
        sender.Complete();
        events.Disconnected(i);
        try
        {
            await writer.ConfigureAwait(false);
        }
        catch
        {
            // 切断後の送信の失敗は Disconnected で伝わっている
        }
    }

    public bool Send(int conn, bool mustDeliver, ReadOnlySpan<byte> data) =>
        Volatile.Read(ref senders[conn]) is SerialSender s && s.Enqueue(data);

    public void Service(TimeSpan timeout) => events.Drain(sut, timeout);

    public void Dispose()
    {
        foreach (var ws in sockets)
        {
            ws?.Abort();
        }
        app?.StopAsync().GetAwaiter().GetResult();
    }
}
