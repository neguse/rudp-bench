// LiteNetLib の層。LT は Sequenced、MD は ReliableOrdered で送る(DeliveryMethod の
// 説明で、Sequenced は「落ちてよいが順に届く」、ReliableOrdered は「落ちず順に届く」)。
// 設定は既定のまま。client は接続ごとに NetManager を持つ(同じ相手への接続は
// NetManager ごとに 1 本のため)。
//
// LITENETLIB_TIMEOUT30 を定義すると(timeout30/LiteNetLibBenchTimeout30.csproj)、切断までの時間(NetManager.DisconnectTimeout)を既定の 5 秒から 30 秒にする。
// トンネルやエレベーター、Wi-Fi と携帯回線の切り替えで回線が十数秒切れても、接続を保ちたいゲームで使う。
using BenchKit.CS;
using LiteNetLib;

return BenchSut.Main(args, new LiteNetLibTransport());

internal sealed class LiteNetLibTransport : ISutTransport
{
    private const string Key = "rudp-bench";

    private readonly List<NetManager> managers = [];
    private NetPeer?[] peers = [];
    private BenchSut sut = null!;
    private bool gotEvent;

#if LITENETLIB_TIMEOUT30
    public string Name => "litenetlib-timeout30";
    private const string Tuning = "[{\"knob\":\"NetManager.DisconnectTimeout\",\"value\":\"30000\",\"reason\":\"keep the session through outages of a few tens of seconds\",\"upstream_ref\":\"LiteNetLib/LiteNetManager.cs\"}]";
#else
    public string Name => "litenetlib";
    private const string Tuning = "[]";
#endif

    public string Describe =>
        "\"class_mapping\":{" +
        "\"loss_tolerant\":{\"primitive\":\"Sequenced\",\"delivery\":\"best_effort\",\"ordering\":\"ordered\",\"realization\":\"native\"}," +
        "\"must_deliver\":{\"primitive\":\"ReliableOrdered\",\"delivery\":\"reliable\",\"ordering\":\"ordered\",\"realization\":\"native\"}}," +
        "\"coalescing\":\"none\",\"cc_algo\":\"none\",\"thread_model\":\"internal_worker\",\"encryption\":false,\"tuning\":" + Tuning;

    public int MaxPayload => NetConstants.MaxUnreliableDataSize;

    public string SendQueueUnit => "packets";

    public void Listen(BenchSut sut, ushort port, int maxConns)
    {
        this.sut = sut;
        peers = new NetPeer?[maxConns];
        var listener = new EventBasedNetListener();
        listener.ConnectionRequestEvent += req => req.AcceptIfKey(Key);
        listener.PeerConnectedEvent += peer =>
        {
            var i = Array.IndexOf(peers, null);
            if (i < 0)
            {
                peer.Disconnect();
                return;
            }
            peers[i] = peer;
            peer.Tag = i;
            Event(() => sut.OnConnect(i));
        };
        Wire(listener);
        var manager = NewManager(listener);
        if (!manager.Start(port))
        {
            throw new InvalidOperationException($"NetManager.Start({port}) failed");
        }
        managers.Add(manager);
    }

    private static NetManager NewManager(EventBasedNetListener listener)
    {
        var manager = new NetManager(listener);
#if LITENETLIB_TIMEOUT30
        manager.DisconnectTimeout = 30000;
#endif
        return manager;
    }

    public void Connect(BenchSut sut, string host, ushort port, int conns)
    {
        this.sut = sut;
        peers = new NetPeer?[conns];
        for (var i = 0; i < conns; ++i)
        {
            var index = i;
            var listener = new EventBasedNetListener();
            listener.PeerConnectedEvent += peer =>
            {
                peer.Tag = index;
                Event(() => sut.OnConnect(index));
            };
            Wire(listener);
            var manager = NewManager(listener);
            if (!manager.Start())
            {
                throw new InvalidOperationException("NetManager.Start failed");
            }
            managers.Add(manager);
            peers[i] = manager.Connect(host, port, Key);
        }
    }

    private void Wire(EventBasedNetListener listener)
    {
        listener.PeerDisconnectedEvent += (peer, info) =>
        {
            Console.Error.WriteLine($"litenetlib: disconnected: {info.Reason}");
            if (peer.Tag is int i)
            {
                Event(() => sut.OnDisconnect(i));
            }
        };
        listener.NetworkReceiveEvent += (peer, reader, _, _) =>
        {
            if (peer.Tag is int i)
            {
                var data = new ReadOnlySpan<byte>(reader.RawData, reader.UserDataOffset, reader.UserDataSize);
                gotEvent = true;
                sut.OnReceive(i, data);
            }
            reader.Recycle();
        };
    }

    private void Event(Action action)
    {
        gotEvent = true;
        action();
    }

    public bool Send(int conn, bool mustDeliver, ReadOnlySpan<byte> data)
    {
        var peer = peers[conn];
        if (peer is null || peer.ConnectionState != ConnectionState.Connected)
        {
            return false;
        }
        peer.Send(data, mustDeliver ? DeliveryMethod.ReliableOrdered : DeliveryMethod.Sequenced);
        return true;
    }

    // LiteNetLib は待つ API を持たないので(PollEvents は溜まったイベントを渡すだけ)、1 ms ごとに見に行く
    public void Service(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            gotEvent = false;
            foreach (var m in managers)
            {
                m.PollEvents();
            }
            var now = DateTime.UtcNow;
            if (gotEvent || now >= deadline)
            {
                return;
            }
            Thread.Sleep(TimeSpan.FromMilliseconds(Math.Min(1, (deadline - now).TotalMilliseconds)));
        }
    }

    public long? SendQueue(int conn) =>
        peers[conn] is NetPeer p ? p.GetPacketsCountInReliableQueue(0, true) : null;

    public void Dispose()
    {
        foreach (var m in managers)
        {
            m.Stop();
        }
    }
}
