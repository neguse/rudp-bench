using System.Globalization;

namespace BenchKit.CS;

// SUT の共通部分(benchkit/src/sut.c と同じ振る舞い)。負荷の生成、計測、
// orchestrator とのやり取りはここが持ち、ライブラリごとの層は ISutTransport を
// 実装するだけにする。
//
// 層は Service() の中から BenchSut.On* を呼んでイベントを返す。BenchSut は
// スレッド安全でないため、別スレッドで届くイベントは SutEventQueue で溜めて渡す。
public interface ISutTransport : IDisposable
{
    // orchestrator の run config の transport と同じ名前
    string Name { get; }

    // --describe の class_mapping, coalescing, cc_algo, thread_model,
    // encryption, tuning を並べた JSON の断片(外側の {} なし)
    string Describe { get; }

    int MaxPayload { get; }

    // server: port で待ち受ける。接続には 0..maxConns-1 の番号を振る。
    void Listen(BenchSut sut, ushort port, int maxConns);

    // client: conns 本の接続を始める。接続の番号は 0..conns-1。
    void Connect(BenchSut sut, string host, ushort port, int conns);

    // ライブラリに渡せたら true。data はこの呼び出しの間だけ有効。
    bool Send(int conn, bool mustDeliver, ReadOnlySpan<byte> data);

    // 1 tick 分の送信を渡し終えたときに呼ぶ。
    void Flush()
    {
    }

    // 送受信を進める。イベントがなければ最大 timeout 待つ。
    void Service(TimeSpan timeout);

    // 送信待ちの量。ライブラリの公開 API で取れなければ null。
    long? SendQueue(int conn) => null;

    string SendQueueUnit => "";
}

// 別スレッドで届いたイベントを溜め、Service() を呼んだスレッドで渡す。
public sealed class SutEventQueue
{
    private readonly Queue<(int Kind, int Conn, byte[]? Data)> events = new();
    private readonly SemaphoreSlim signal = new(0);

    public void Connected(int conn) => Post(0, conn, null);

    public void Received(int conn, byte[] data) => Post(1, conn, data);

    public void Disconnected(int conn) => Post(2, conn, null);

    public void Drain(BenchSut sut, TimeSpan timeout)
    {
        signal.Wait(timeout);
        while (true)
        {
            (int Kind, int Conn, byte[]? Data) e;
            lock (events)
            {
                if (!events.TryDequeue(out e))
                {
                    return;
                }
            }
            switch (e.Kind)
            {
                case 0:
                    sut.OnConnect(e.Conn);
                    break;
                case 1:
                    sut.OnReceive(e.Conn, e.Data);
                    break;
                default:
                    sut.OnDisconnect(e.Conn);
                    break;
            }
        }
    }

    private void Post(int kind, int conn, byte[]? data)
    {
        lock (events)
        {
            events.Enqueue((kind, conn, data));
        }
        signal.Release();
    }
}

public sealed class BenchSut
{
    private const int Lt = 0;
    private const int Md = 1;
    private const ulong ConnectTimeoutNs = 10_000_000_000UL;
    private static readonly TimeSpan MaxSleep = TimeSpan.FromMilliseconds(10);

    private sealed class Traffic
    {
        public byte TrafficId;
        public readonly double[] Rate = new double[2];
        public readonly int[] Payload = new int[2];
        public ulong DeadlineNs;
        public int Present;
    }

    private sealed class Conn
    {
        public bool Connected;
        // client: 自分の origin。server: 登録された origin(未登録は -1)
        public long Origin = -1;
        public BenchPlan? Plan;
        // client: 送った input(LT)の最大 seq。server: 受けた input(LT)の最大 seq
        public ulong InputSeq;
        public ulong InputSentMeasured;
        public ulong StateSeqMeasured;
        public ulong AppliedMeasured;
    }

    private readonly ISutTransport tr;
    private bool server;
    private string host = "";
    private ushort port;
    private int procIndex;
    private uint originBase;
    private uint totalConns;
    private readonly Traffic input = new();
    private readonly Traffic state = new();
    private ulong stalenessPeriodNs = BenchConstants.DefaultStalenessPeriodNs;

    private BenchMetrics metrics = null!;
    private Conn[] conns = [];
    private long[] roster = [];
    private int connected;
    private uint registered;
    private bool frozen;
    private bool failed;
    private ulong invalidPayload;
    private ulong peerDisconnects;
    private ulong stateTicks;
    private long sendQueueMax;
    private byte[] buf = [];

    private BenchSut(ISutTransport transport)
    {
        tr = transport;
    }

    // args[0] が server / client / --describe。戻り値は終了コード。
    public static int Main(string[] args, ISutTransport transport)
    {
        if (args.Length == 1 && args[0] == "--describe")
        {
            Console.WriteLine("{\"transport\":\"" + transport.Name + "\"," + transport.Describe +
                ",\"payload_pattern\":\"splitmix64-v1\",\"wire_compression\":\"none\"," +
                "\"max_payload_bytes\":" + transport.MaxPayload.ToString(CultureInfo.InvariantCulture) +
                ",\"scenarios\":[\"authoritative_state\"]}");
            return 0;
        }
        var sut = new BenchSut(transport);
        if (!sut.ParseArgs(args))
        {
            Console.Error.WriteLine(
                "usage: server --port PORT <scenario args>\n" +
                "       client --host HOST --port PORT --conns N --proc-index N --origin-base N <scenario args>\n" +
                "       --describe");
            return 2;
        }
        try
        {
            return sut.RunAsync().GetAwaiter().GetResult() ? 0 : 1;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"{transport.Name}: {e}");
            return 1;
        }
        finally
        {
            transport.Dispose();
        }
    }

    // ---- 引数 ----------------------------------------------------------------

    private static bool ParseTraffic(string arg, string value, string prefix, Traffic t, out bool matched)
    {
        matched = false;
        if (!arg.StartsWith("--" + prefix + "-", StringComparison.Ordinal))
        {
            return true;
        }
        matched = true;
        var field = arg[(prefix.Length + 3)..];
        var inv = CultureInfo.InvariantCulture;
        switch (field)
        {
            case "traffic-id":
                if (!byte.TryParse(value, inv, out t.TrafficId) || t.TrafficId == 0)
                {
                    return false;
                }
                t.Present |= 1;
                return true;
            case "rate-lt":
            case "rate-md":
                var c = field == "rate-lt" ? Lt : Md;
                if (!double.TryParse(value, NumberStyles.Float, inv, out t.Rate[c]) || !(t.Rate[c] >= 0))
                {
                    return false;
                }
                t.Present |= c == Lt ? 2 : 4;
                return true;
            case "payload-lt":
            case "payload-md":
                var p = field == "payload-lt" ? Lt : Md;
                if (!int.TryParse(value, inv, out t.Payload[p]))
                {
                    return false;
                }
                t.Present |= p == Lt ? 8 : 16;
                return true;
            case "deadline-ns":
                if (!ulong.TryParse(value, inv, out t.DeadlineNs))
                {
                    return false;
                }
                t.Present |= 32;
                return true;
            default:
                matched = false;
                return true;
        }
    }

    private static bool TrafficValid(Traffic t, int minPayload, int maxPayload)
    {
        if (t.Present != 63 || (t.Rate[Lt] == 0 && t.Rate[Md] == 0))
        {
            return false;
        }
        for (var c = Lt; c <= Md; ++c)
        {
            if (t.Rate[c] > 0 && (t.Payload[c] < minPayload || t.Payload[c] > maxPayload))
            {
                return false;
            }
        }
        return true;
    }

    private bool ParseArgs(string[] args)
    {
        if (args.Length < 1 || (args[0] != "server" && args[0] != "client"))
        {
            return false;
        }
        server = args[0] == "server";
        var inv = CultureInfo.InvariantCulture;
        bool scenario = false, havePort = false, haveConns = false, haveOriginBase = false;
        var nConns = 0;
        for (var i = 1; i < args.Length; i += 2)
        {
            if (i + 1 >= args.Length)
            {
                return false;
            }
            var arg = args[i];
            var value = args[i + 1];
            if (!ParseTraffic(arg, value, "input", input, out var matched) ||
                (!matched && !ParseTraffic(arg, value, "state", state, out matched)))
            {
                return false;
            }
            if (matched)
            {
                continue;
            }
            bool ok;
            switch (arg)
            {
                case "--scenario":
                    ok = scenario = value == "authoritative_state";
                    break;
                case "--total-conns":
                    ok = uint.TryParse(value, inv, out totalConns) && totalConns > 0 && totalConns <= int.MaxValue;
                    break;
                case "--staleness-period-ns":
                    ok = ulong.TryParse(value, inv, out stalenessPeriodNs) && stalenessPeriodNs > 0;
                    break;
                case "--host":
                    host = value;
                    ok = true;
                    break;
                case "--port":
                    ok = havePort = ushort.TryParse(value, inv, out port) && port != 0;
                    break;
                case "--conns":
                    ok = haveConns = int.TryParse(value, inv, out nConns) && nConns > 0;
                    break;
                case "--proc-index":
                    ok = int.TryParse(value, inv, out procIndex) && procIndex >= 0;
                    break;
                case "--origin-base":
                    ok = haveOriginBase = uint.TryParse(value, inv, out originBase);
                    break;
                default:
                    ok = false;
                    break;
            }
            if (!ok)
            {
                return false;
            }
        }
        if (!scenario || totalConns == 0 || !havePort || input.TrafficId == state.TrafficId ||
            !TrafficValid(input, BenchConstants.MinPayloadBytes, tr.MaxPayload) ||
            !TrafficValid(state, BenchPayload.AuthoritativeStateMinPayloadBytes, tr.MaxPayload))
        {
            return false;
        }
        if (!server && (host == "" || !haveConns || !haveOriginBase || (ulong)originBase + (ulong)nConns > totalConns))
        {
            return false;
        }
        conns = new Conn[server ? (int)totalConns : nConns];
        for (var i = 0; i < conns.Length; ++i)
        {
            conns[i] = new Conn();
        }
        return true;
    }

    // ---- 共通 ----------------------------------------------------------------

    private static List<BenchStream> BuildStreams(Traffic t, BenchDirection direction)
    {
        var streams = new List<BenchStream>();
        for (var c = Lt; c <= Md; ++c)
        {
            if (t.Rate[c] > 0)
            {
                streams.Add(new BenchStream(c == Md, false, (ulong)(1e9 / t.Rate[c] + 0.5), t.TrafficId, direction));
            }
        }
        return streams;
    }

    private static bool IsRegistration(BenchHeader h, int len) =>
        len == BenchConstants.HeaderSize && h.Seq == 0 && h.SchedTsNs == 0 && h.SendTsNs != 0 &&
        h.Flags == 0 && h.TrafficId == 0;

    private static bool HeaderMatches(BenchHeader h, int len, Traffic t, BenchDirection direction)
    {
        var c = (h.Flags & BenchConstants.FlagMustDeliver) != 0 ? Md : Lt;
        return h.Seq != 0 && h.SchedTsNs != 0 && h.SendTsNs != 0 && h.TrafficId == t.TrafficId &&
            BenchConstants.DirectionFromFlags(h.Flags) == direction &&
            (h.Flags & BenchConstants.FlagBroadcast) == 0 && t.Rate[c] > 0 && len == t.Payload[c];
    }

    private bool ValidConn(int conn) => conn >= 0 && conn < conns.Length;

    private void SampleSendQueue()
    {
        for (var i = 0; i < conns.Length; ++i)
        {
            if (conns[i].Connected && tr.SendQueue(i) is long q && q > sendQueueMax)
            {
                sendQueueMax = q;
            }
        }
    }

    private void ServiceUntil(ulong now, ulong next)
    {
        var waitNs = next > now ? next - now : 0;
        var wait = TimeSpan.FromTicks((long)(waitNs / 100));
        tr.Service(wait < MaxSleep ? wait : MaxSleep);
        if (failed)
        {
            throw new InvalidOperationException("transport reported a failure");
        }
    }

    // schedule を待つ間も送受信を進める(止めると相手側の接続処理が止まる)。
    private async Task<BenchSchedule> WaitScheduleAsync(BenchControl control)
    {
        var task = control.WaitScheduleAsync(CancellationToken.None);
        while (!task.IsCompleted)
        {
            var now = BenchClock.NowNs();
            ServiceUntil(now, now + 10_000_000UL);
        }
        return await task.ConfigureAwait(false);
    }

    private async Task<bool> FinishAsync(BenchControl control, string progress)
    {
        var path = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
        if (string.IsNullOrEmpty(path))
        {
            Console.Error.WriteLine($"{tr.Name}: BENCH_METRICS_OUT is not set");
            return false;
        }
        metrics.DumpJson(path);
        var queue = tr.SendQueueUnit == ""
            ? ""
            : $",\"send_queue_max\":{sendQueueMax},\"send_queue_unit\":\"{tr.SendQueueUnit}\"";
        await control.DoneAsync(
            $"{{\"invalid_payload\":{invalidPayload},\"peer_disconnects\":{peerDisconnects}{queue},{progress}}}",
            CancellationToken.None).ConfigureAwait(false);
        return true;
    }

    private static string Progress(string role, uint localConns, uint rosterConns, ulong[] lo, ulong[] hi, ulong ticks) =>
        $"\"authoritative_progress\":{{\"role\":\"{role}\",\"local_conns\":{localConns}," +
        $"\"roster_conns\":{rosterConns},\"input_last_sent_min\":{lo[0]},\"input_last_sent_max\":{hi[0]}," +
        $"\"state_header_seq_recv_min\":{lo[1]},\"state_header_seq_recv_max\":{hi[1]}," +
        $"\"state_applied_input_seq_recv_min\":{lo[2]},\"state_applied_input_seq_recv_max\":{hi[2]}," +
        $"\"server_state_ticks\":{ticks}}}";

    // ---- イベント ------------------------------------------------------------

    public void OnConnect(int conn)
    {
        if (!ValidConn(conn) || conns[conn].Connected)
        {
            return;
        }
        conns[conn].Connected = true;
        connected++;
        if (!server)
        {
            var h = new BenchHeader(0, 0, BenchClock.NowNs(), 0, (uint)conns[conn].Origin);
            var reg = buf.AsSpan(0, BenchConstants.HeaderSize);
            if (!BenchPayload.TryWrite(reg, h) || !tr.Send(conn, true, reg))
            {
                Console.Error.WriteLine($"{tr.Name}: registration send failed");
                failed = true;
            }
        }
    }

    public void OnDisconnect(int conn)
    {
        if (!ValidConn(conn))
        {
            return;
        }
        var c = conns[conn];
        if (!c.Connected)
        {
            // 一度もつながらなかった接続(client の接続失敗)
            failed |= !server;
            return;
        }
        c.Connected = false;
        connected--;
        peerDisconnects++;
        Console.Error.WriteLine($"{tr.Name}: conn {conn} disconnected (continuing)");
        if (server && c.Origin >= 0)
        {
            roster[c.Origin] = -1;
            if (!frozen)
            {
                registered--;
                c.Origin = -1;
            }
        }
    }

    private bool RegisterOrigin(int conn, uint origin)
    {
        var c = conns[conn];
        if (origin >= totalConns)
        {
            return false;
        }
        if (c.Origin >= 0)
        {
            return c.Origin == origin;
        }
        if (frozen || roster[origin] >= 0)
        {
            return false;
        }
        c.Origin = origin;
        roster[origin] = conn;
        registered++;
        return true;
    }

    private bool ServerReceive(int conn, ReadOnlySpan<byte> data, BenchHeader h)
    {
        if (IsRegistration(h, data.Length))
        {
            return RegisterOrigin(conn, h.OriginId);
        }
        if (!HeaderMatches(h, data.Length, input, BenchDirection.ClientToServer) ||
            !BenchPayload.ValidateBody(data, h) || !RegisterOrigin(conn, h.OriginId))
        {
            return false;
        }
        var c = conns[conn];
        if ((h.Flags & BenchConstants.FlagMustDeliver) == 0)
        {
            c.InputSeq = Math.Max(c.InputSeq, h.Seq);
        }
        metrics.OnRecv(h.OriginId, h, BenchClock.NowNs());
        return true;
    }

    private bool ClientReceive(int conn, ReadOnlySpan<byte> data, BenchHeader h)
    {
        var c = conns[conn];
        if (h.OriginId != totalConns ||
            !HeaderMatches(h, data.Length, state, BenchDirection.ServerToClient) ||
            !BenchPayload.TryReadAppliedInputSeq(data, out var applied) ||
            applied > c.InputSeq ||
            !BenchPayload.ValidateTargetPad(data, (uint)c.Origin))
        {
            return false;
        }
        if ((h.Flags & (BenchConstants.FlagMustDeliver | BenchConstants.FlagMeasure)) == BenchConstants.FlagMeasure)
        {
            c.StateSeqMeasured = Math.Max(c.StateSeqMeasured, h.Seq);
            c.AppliedMeasured = Math.Max(c.AppliedMeasured, applied);
        }
        metrics.OnRecv((uint)conn, h, BenchClock.NowNs());
        return true;
    }

    public void OnReceive(int conn, ReadOnlySpan<byte> data)
    {
        var ok = ValidConn(conn) && BenchPayload.TryRead(data, out var h) &&
            (server ? ServerReceive(conn, data, h) : ClientReceive(conn, data, h));
        if (!ok)
        {
            invalidPayload++;
        }
    }

    // ---- 実行 ----------------------------------------------------------------

    private async Task<bool> RunAsync()
    {
        metrics = new BenchMetrics(new BenchMetricsConfig(totalConns + 1, input.DeadlineNs, stalenessPeriodNs, (uint)conns.Length));
        metrics.SetTrafficDeadline(input.TrafficId, BenchDirection.ClientToServer, input.DeadlineNs);
        metrics.SetTrafficDeadline(state.TrafficId, BenchDirection.ServerToClient, state.DeadlineNs);
        roster = Enumerable.Repeat(-1L, (int)totalConns).ToArray();
        buf = new byte[tr.MaxPayload];
        await using var control = await BenchControl.ConnectFromEnvironmentAsync(CancellationToken.None).ConfigureAwait(false)
            ?? throw new InvalidOperationException("BENCH_CONTROL_SOCK is not set");
        return server ? await RunServerAsync(control).ConfigureAwait(false) : await RunClientAsync(control).ConfigureAwait(false);
    }

    private void ClientSendSlot(int conn, BenchSlot slot)
    {
        var c = conns[conn];
        var md = (slot.Flags & BenchConstants.FlagMustDeliver) != 0;
        var span = buf.AsSpan(0, input.Payload[md ? Md : Lt]);
        var h = new BenchHeader(slot.Seq, slot.SchedTsNs, BenchClock.NowNs(), slot.Flags, (uint)c.Origin, slot.TrafficId);
        var ok = BenchPayload.TryWrite(span, h) && BenchPayload.TryFillBody(span, h) && tr.Send(conn, md, span);
        metrics.OnSlot(h, ok);
        if (ok && !md)
        {
            c.InputSeq = Math.Max(c.InputSeq, h.Seq);
            if ((h.Flags & BenchConstants.FlagMeasure) != 0)
            {
                c.InputSentMeasured = Math.Max(c.InputSentMeasured, h.Seq);
            }
        }
    }

    private async Task<bool> RunClientAsync(BenchControl control)
    {
        await control.HelloAsync("client", tr.Name, procIndex, CancellationToken.None).ConfigureAwait(false);
        for (var i = 0; i < conns.Length; ++i)
        {
            conns[i].Origin = originBase + (uint)i;
        }
        tr.Connect(this, host, port, conns.Length);
        var deadline = BenchClock.NowNs() + ConnectTimeoutNs;
        while (connected < conns.Length)
        {
            var now = BenchClock.NowNs();
            if (now >= deadline)
            {
                Console.Error.WriteLine($"{tr.Name}: connect timed out ({connected}/{conns.Length})");
                return false;
            }
            ServiceUntil(now, now + 10_000_000UL);
        }
        await control.ReadyAsync(conns.Length, CancellationToken.None).ConfigureAwait(false);
        var schedule = await WaitScheduleAsync(control).ConfigureAwait(false);
        var streams = BuildStreams(input, BenchDirection.ClientToServer);
        var start = BenchClock.NowNs();
        for (var i = 0; i < conns.Length; ++i)
        {
            conns[i].Plan = new BenchPlan(streams, start, schedule.StartAtNs, schedule.StopAtNs);
            if (state.Rate[Lt] > 0 &&
                !metrics.ExpectLatest((uint)i, totalConns, state.TrafficId, BenchDirection.ServerToClient, schedule.StartAtNs))
            {
                return false;
            }
        }

        var steady = new BenchSteady();
        while (true)
        {
            var now = BenchClock.NowNs();
            if (now >= schedule.DrainUntilNs)
            {
                break;
            }
            var raw = metrics.RawCounts();
            if (await steady.TickAsync(control, raw.Submitted, raw.RecvMeasured + raw.RecvUnmeasured, schedule, now,
                    CancellationToken.None).ConfigureAwait(false) is BenchSchedule window)
            {
                schedule = window;
                foreach (var c in conns)
                {
                    c.Plan!.SetWindow(schedule.StartAtNs, schedule.StopAtNs);
                }
            }
            if (now >= schedule.StartAtNs && now < schedule.StopAtNs)
            {
                metrics.Tick(now);
                SampleSendQueue();
            }
            // 計測窓の終わりより前に予定された分は、遅れても送る(遅延は予定時刻から測る)
            var cutoff = now < schedule.StopAtNs ? now : schedule.StopAtNs - 1;
            var next = schedule.DrainUntilNs;
            var sent = false;
            for (var i = 0; i < conns.Length; ++i)
            {
                var c = conns[i];
                if (!c.Connected)
                {
                    continue;
                }
                while (c.Plan!.TryNext(cutoff, out var slot))
                {
                    ClientSendSlot(i, slot);
                    sent = true;
                }
                var due = c.Plan.PeekNs();
                if (due < schedule.StopAtNs && due < next)
                {
                    next = due;
                }
            }
            if (sent)
            {
                tr.Flush();
            }
            ServiceUntil(BenchClock.NowNs(), next);
        }
        foreach (var c in conns)
        {
            while (c.Plan!.TryNext(schedule.StopAtNs - 1, out var slot))
            {
                metrics.OnSlot(new BenchHeader(slot.Seq, slot.SchedTsNs, 0, slot.Flags, (uint)c.Origin, slot.TrafficId), false);
            }
        }

        var lo = new[] { ulong.MaxValue, ulong.MaxValue, ulong.MaxValue };
        var hi = new ulong[3];
        foreach (var c in conns)
        {
            var v = new[] { c.InputSentMeasured, c.StateSeqMeasured, c.AppliedMeasured };
            for (var k = 0; k < 3; ++k)
            {
                lo[k] = Math.Min(lo[k], v[k]);
                hi[k] = Math.Max(hi[k], v[k]);
            }
        }
        return await FinishAsync(control, Progress("client", (uint)conns.Length, totalConns, lo, hi, 0)).ConfigureAwait(false);
    }

    private void ServerSendSlot(BenchSlot slot, bool submit)
    {
        var md = (slot.Flags & BenchConstants.FlagMustDeliver) != 0;
        var span = buf.AsSpan(0, state.Payload[md ? Md : Lt]);
        if ((slot.Flags & (BenchConstants.FlagMustDeliver | BenchConstants.FlagMeasure)) == BenchConstants.FlagMeasure)
        {
            stateTicks++;
        }
        for (uint target = 0; target < totalConns; ++target)
        {
            var h = new BenchHeader(slot.Seq, slot.SchedTsNs, 0, slot.Flags, totalConns, slot.TrafficId);
            var conn = roster[target];
            var ok = false;
            if (submit && conn >= 0 && conns[conn].Connected)
            {
                h = h with { SendTsNs = BenchClock.NowNs() };
                ok = BenchPayload.TryWrite(span, h) &&
                    BenchPayload.TryWriteAppliedInputSeq(span, conns[conn].InputSeq) &&
                    BenchPayload.TryFillTargetPad(span, target) &&
                    tr.Send((int)conn, md, span);
            }
            metrics.OnSlot(h, ok);
        }
    }

    private async Task<bool> RunServerAsync(BenchControl control)
    {
        tr.Listen(this, port, conns.Length);
        await control.HelloAsync("server", tr.Name, 0, CancellationToken.None).ConfigureAwait(false);
        await control.ReadyAsync(0, CancellationToken.None).ConfigureAwait(false);
        var schedule = await WaitScheduleAsync(control).ConfigureAwait(false);
        var streams = BuildStreams(state, BenchDirection.ServerToClient);
        BenchPlan? plan = null;
        var windowFinal = false;
        while (true)
        {
            var now = BenchClock.NowNs();
            if (now >= schedule.DrainUntilNs)
            {
                break;
            }
            if (!frozen)
            {
                if (registered == totalConns)
                {
                    plan = new BenchPlan(streams, now, schedule.StartAtNs, schedule.StopAtNs);
                    for (uint o = 0; o < totalConns; ++o)
                    {
                        if (input.Rate[Lt] > 0 &&
                            !metrics.ExpectLatest(o, o, input.TrafficId, BenchDirection.ClientToServer, schedule.StartAtNs))
                        {
                            return false;
                        }
                    }
                    frozen = true;
                }
                else if (now >= schedule.StartAtNs)
                {
                    Console.Error.WriteLine($"{tr.Name}: only {registered}/{totalConns} clients registered before start");
                    return false;
                }
            }
            if (!windowFinal)
            {
                if (now >= schedule.StartAtNs)
                {
                    windowFinal = true;
                }
                else if (await control.PollWindowAsync(CancellationToken.None).ConfigureAwait(false) is BenchSchedule window)
                {
                    windowFinal = true;
                    schedule = window;
                    plan?.SetWindow(schedule.StartAtNs, schedule.StopAtNs);
                }
            }
            if (now >= schedule.StartAtNs && now < schedule.StopAtNs)
            {
                metrics.Tick(now);
                SampleSendQueue();
            }
            var next = schedule.DrainUntilNs;
            if (plan is not null)
            {
                var cutoff = now < schedule.StopAtNs ? now : schedule.StopAtNs - 1;
                var sent = false;
                while (plan.TryNext(cutoff, out var slot))
                {
                    ServerSendSlot(slot, true);
                    sent = true;
                }
                if (sent)
                {
                    tr.Flush();
                }
                var due = plan.PeekNs();
                if (due < schedule.StopAtNs)
                {
                    next = due;
                }
            }
            ServiceUntil(BenchClock.NowNs(), next);
        }
        if (plan is not null)
        {
            while (plan.TryNext(schedule.StopAtNs - 1, out var slot))
            {
                ServerSendSlot(slot, false);
            }
        }
        var zero = new ulong[3];
        return await FinishAsync(control, Progress("server", 0, registered, zero, zero, stateTicks)).ConfigureAwait(false);
    }
}

// 同時に 1 つしか送れない API(WebSocket.SendAsync など)のために、送信を溜めて順に渡す。
public sealed class SerialSender
{
    private readonly System.Threading.Channels.Channel<byte[]> queue =
        System.Threading.Channels.Channel.CreateUnbounded<byte[]>(
            new System.Threading.Channels.UnboundedChannelOptions { SingleReader = true });

    public bool Enqueue(ReadOnlySpan<byte> data) => queue.Writer.TryWrite(data.ToArray());

    public void Complete() => queue.Writer.TryComplete();

    public async Task RunAsync(Func<byte[], Task> send)
    {
        await foreach (var payload in queue.Reader.ReadAllAsync().ConfigureAwait(false))
        {
            await send(payload).ConfigureAwait(false);
        }
    }
}
