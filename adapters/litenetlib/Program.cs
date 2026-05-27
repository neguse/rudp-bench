// LiteNetLib adapter for rudp-bench
// C# / .NET 8 独立バイナリ — harness/csv_writer.h の CSV カラム順を完全に踏襲する
//
// CSV column order (mirrors harness/csv_writer.h write_row exactly):
//   library,encryption,phase,rate_r,rate_u,size,conns,loss,
//   throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,
//   delivered,accepted,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,
//   mode,idle_policy,flush_policy,...
//
// Payload layout: bytes [0..8) seq, [8..16) ts_ns, [16] reliable flag (0/1),
// [17..size) padding. Server reads the flag and echoes back on the same channel
// so mixed reliable+unreliable runs exercise per-channel HoL behavior.

using LiteNetLib;
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;

// インバリアントカルチャを強制（小数点がコンマになる環境での誤動作防止）
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;

var cfg = ParseArgs(args);
if (cfg == null)
{
    Console.Error.WriteLine("usage: litenetlib_adapter --library=<name> --role=server|client " +
        "--host= --port= --rate-r= --rate-u= --size= --conns= " +
        "--duration= --warmup= --loss= --idle=spin|adaptive --out=");
    return 2;
}

CsvRow row = PayloadSupported(cfg) ? (cfg.Role == "server" ? RunServer(cfg) : RunClient(cfg))
                                   : UnsupportedPayloadRow(cfg);

string header = CsvHeader();
string line = FormatRow(row);
if (!string.IsNullOrEmpty(cfg.OutPath))
{
    File.WriteAllText(cfg.OutPath, header + "\n" + line + "\n");
}
else
{
    Console.WriteLine(header);
    Console.WriteLine(line);
}
return 0;

static bool PayloadSupported(Config cfg) =>
    cfg.SizeBytes >= Config.MinPayloadBytes && cfg.SizeBytes <= Config.MaxPayloadBytes;

static void SampleRssIfDue(ProcSampler ps, ref long nextSampleTicks)
{
    long nowTicks = Stopwatch.GetTimestamp();
    if (nowTicks < nextSampleTicks) return;
    ps.SampleRss();
    nextSampleTicks = nowTicks + ProcSampler.RssSampleIntervalTicks;
}

static CsvRow UnsupportedPayloadRow(Config cfg) => new()
{
    Library = cfg.Library,
    Encryption = "off",
    Phase = 1,
    RateR = cfg.RateR,
    RateU = cfg.RateU,
    Size = cfg.SizeBytes,
    Conns = cfg.Conns,
    Loss = cfg.Loss,
    DurationS = cfg.DurationS,
    Mode = cfg.Mode,
    IdlePolicy = cfg.IdlePolicy,
    FlushPolicy = FlushPolicy(cfg),
};

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

static Config? ParseArgs(string[] args)
{
    var cfg = new Config();
    foreach (var arg in args)
    {
        if (arg.StartsWith("--library=")) cfg.Library = arg["--library=".Length..];
        else if (arg.StartsWith("--role="))
        {
            cfg.Role = arg["--role=".Length..];
            if (cfg.Role != "server" && cfg.Role != "client") return null;
        }
        else if (arg.StartsWith("--host=")) cfg.Host = arg["--host=".Length..];
        else if (arg.StartsWith("--port=")) cfg.Port = ushort.Parse(arg["--port=".Length..]);
        else if (arg.StartsWith("--rate-r=")) cfg.RateR = uint.Parse(arg["--rate-r=".Length..]);
        else if (arg.StartsWith("--rate-u=")) cfg.RateU = uint.Parse(arg["--rate-u=".Length..]);
        else if (arg.StartsWith("--size=")) cfg.SizeBytes = uint.Parse(arg["--size=".Length..]);
        else if (arg.StartsWith("--conns=")) cfg.Conns = uint.Parse(arg["--conns=".Length..]);
        else if (arg.StartsWith("--duration=")) cfg.DurationS = uint.Parse(arg["--duration=".Length..]);
        else if (arg.StartsWith("--warmup=")) cfg.WarmupS = uint.Parse(arg["--warmup=".Length..]);
        else if (arg.StartsWith("--loss=")) cfg.Loss = double.Parse(arg["--loss=".Length..], CultureInfo.InvariantCulture);
        else if (arg.StartsWith("--mode="))
        {
            cfg.Mode = arg["--mode=".Length..];
            if (cfg.Mode != "echo" && cfg.Mode != "broadcast") return null;
        }
        else if (arg.StartsWith("--idle="))
        {
            cfg.IdlePolicy = arg["--idle=".Length..];
            if (cfg.IdlePolicy != "spin" && cfg.IdlePolicy != "adaptive") return null;
        }
        else if (arg.StartsWith("--out=")) cfg.OutPath = arg["--out=".Length..];
        else { Console.Error.WriteLine($"unknown flag: {arg}"); return null; }
    }
    if (string.IsNullOrEmpty(cfg.Library)) return null;
    if (cfg.RateR == 0 && cfg.RateU == 0)
    {
        Console.Error.WriteLine("at least one of --rate-r / --rate-u must be > 0");
        return null;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

static CsvRow RunServer(Config cfg)
{
    var listener = new EventBasedNetListener();
    var manager = new NetManager(listener) { AutoRecycle = true };

    // 受信メッセージキュー（PollEvents()内で同期的にpushされる）
    var inbox = new PooledInbox<NetPeer>();

    listener.ConnectionRequestEvent += req => req.AcceptIfKey("rudpbench");
    listener.NetworkReceiveEvent += (peer, reader, _, _) =>
    {
        inbox.Add(peer, reader);
    };

    manager.Start(cfg.Port);

    var ps = new ProcSampler();
    ps.Begin();
    long nextRssSampleTicks = Stopwatch.GetTimestamp() + ProcSampler.RssSampleIntervalTicks;

    // クライアントの warmup + duration + tail_drain を収容するため +2秒余裕
    long deadlineTicks = Stopwatch.GetTimestamp() +
        (long)((cfg.WarmupS + cfg.DurationS + 2) * Stopwatch.Frequency);

    var knownPeers = new HashSet<NetPeer>();
    bool broadcast = cfg.Mode == "broadcast";

    while (Stopwatch.GetTimestamp() < deadlineTicks)
    {
        SampleRssIfDue(ps, ref nextRssSampleTicks);
        manager.PollEvents();
        bool didWork = inbox.Count > 0;

        foreach (var msg in inbox.Entries)
        {
            knownPeers.Add(msg.Conn);
            if (msg.Length < Config.MinPayloadBytes) continue;
            // Echo on the same channel the message arrived on; client tagged
            // byte 16 with the reliable flag.
            var deliveryMethod = msg.Buffer[16] != 0
                ? DeliveryMethod.ReliableOrdered
                : DeliveryMethod.Unreliable;
            if (broadcast)
            {
                foreach (var target in knownPeers)
                    target.Send(new ReadOnlySpan<byte>(msg.Buffer, 0, msg.Length), 0, deliveryMethod);
            }
            else
            {
                msg.Conn.Send(new ReadOnlySpan<byte>(msg.Buffer, 0, msg.Length), 0, deliveryMethod);
            }
        }
        inbox.Clear();

        if (!didWork && cfg.IdlePolicy == "adaptive") Thread.Sleep(0);
    }

    ps.End();
    manager.Stop();

    return new CsvRow
    {
        Library = cfg.Library,
        Encryption = "off",
        Phase = 1,
        RateR = cfg.RateR,
        RateU = cfg.RateU,
        Size = cfg.SizeBytes,
        Conns = cfg.Conns,
        Loss = cfg.Loss,
        DurationS = cfg.DurationS,
        CpuPct = ps.CpuPct(),
        RssMb = ps.RssMbMax,
        Mode = cfg.Mode,
        IdlePolicy = cfg.IdlePolicy,
        FlushPolicy = FlushPolicy(cfg),
    };
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static CsvRow RunClient(Config cfg)
{
    // LiteNetLib の NetManager は内部で単一 UDP ソケットを持つ。
    // 同一 manager から複数 Connect すると全 peer が同一 source endpoint
    // を共有し、サーバ側で同一エンドポイント扱いになって接続待ちがハングする。
    // 解決策は conn ごとに独立した NetManager を持つこと（各自エフェメラル
    // ポートを取得 → サーバから別 peer として認識される）。
    var managers = new NetManager[cfg.Conns];
    var peers = new NetPeer[cfg.Conns];
    var connected = new bool[cfg.Conns];

    // 受信メッセージキュー（各 listener から conn_id 付きで push）
    var inbox = new PooledInbox<uint>();

    for (uint i = 0; i < cfg.Conns; i++)
    {
        uint connId = i; // closure capture
        var listener = new EventBasedNetListener();
        listener.PeerConnectedEvent += _ => connected[connId] = true;
        listener.NetworkReceiveEvent += (_, reader, _, _) =>
            inbox.Add(connId, reader);

        var manager = new NetManager(listener) { AutoRecycle = true };
        if (!manager.Start())
        {
            Console.Error.WriteLine($"NetManager.Start() failed for conn {connId}");
            Environment.Exit(2);
        }
        managers[i] = manager;
    }

    // 全コネクション発行
    long tConnectBeginTicks = Stopwatch.GetTimestamp();
    for (uint i = 0; i < cfg.Conns; i++)
    {
        peers[i] = managers[i].Connect(cfg.Host, cfg.Port, "rudpbench");
    }

    // 全コネクションが ready になるまで poll
    while (!connected.All(c => c))
    {
        for (uint i = 0; i < cfg.Conns; i++) managers[i].PollEvents();
        Thread.Sleep(1);
    }
    long tConnectEndTicks = Stopwatch.GetTimestamp();
    ulong connectMs = (ulong)((tConnectEndTicks - tConnectBeginTicks) * 1000L / Stopwatch.Frequency);

    var ps = new ProcSampler();
    ps.Begin();
    long nextRssSampleTicks = Stopwatch.GetTimestamp() + ProcSampler.RssSampleIntervalTicks;

    // タイミングは Stopwatch ベース（DateTime.UtcNow より高精度）
    long nowTicks = Stopwatch.GetTimestamp();
    long warmupEndTicks = nowTicks + (long)(cfg.WarmupS * Stopwatch.Frequency);
    long runEndTicks = warmupEndTicks + (long)(cfg.DurationS * Stopwatch.Frequency);
    long tailUntilTicks = runEndTicks + Stopwatch.Frequency / 2; // +500ms tail drain

    var rttR = new LatencyHist();
    var rttU = new LatencyHist();
    var throughput = new ThroughputCounter();
    var delivery = new DeliveryTracker();
    var tick = new ClientTickStats();

    // ペーシング: 2 系統(reliable/unreliable)の per-conn 次回送信時刻
    var schedR = new ChannelSched(cfg.RateR, true, cfg.Conns, Stopwatch.GetTimestamp());
    var schedU = new ChannelSched(cfg.RateU, false, cfg.Conns, Stopwatch.GetTimestamp());
    ulong combinedRate = (ulong)cfg.RateR + cfg.RateU;
    ulong missedBudgetUs = TickUtil.PacingBudgetUs(Math.Max(cfg.RateR, cfg.RateU));
    uint expectedPerSend = cfg.Mode == "broadcast" ? cfg.Conns : 1u;
    ulong targetAttempted = combinedRate * cfg.Conns * cfg.DurationS * expectedPerSend;
    ulong outstanding = 0;

    var payload = new byte[Math.Max(cfg.SizeBytes, Config.MinPayloadBytes)];
    Array.Fill(payload, (byte)0xAB);
    var seqCounters = new ulong[cfg.Conns];
    for (int i = 0; i < cfg.Conns; i++) seqCounters[i] = 1;

    void TrySendOn(ChannelSched sched, uint i, long now, ref bool didWork, bool inActive)
    {
        if (!sched.Active || now < sched.NextSend[i]) return;
        didWork = true;
        ulong lagUs = 0;
        if (now > sched.NextSend[i])
            lagUs = TickUtil.TicksToUs(now - sched.NextSend[i]);
        if (inActive) tick.RecordSendDue(expectedPerSend, lagUs, missedBudgetUs);

        ulong localSeq = seqCounters[i]++;
        ulong globalSeq = ((ulong)i << 32) | localSeq;
        ulong tsNs = (ulong)(Stopwatch.GetTimestamp() * 1_000_000_000L / Stopwatch.Frequency);
        BitConverter.TryWriteBytes(payload.AsSpan(0, 8), globalSeq);
        BitConverter.TryWriteBytes(payload.AsSpan(8, 8), tsNs);
        payload[16] = sched.Reliable ? (byte)1 : (byte)0;

        var method = sched.Reliable
            ? DeliveryMethod.ReliableOrdered
            : DeliveryMethod.Unreliable;
        peers[i].Send(payload, method);
        if (inActive)
        {
            tick.RecordAccepted(expectedPerSend);
            for (uint k = 0; k < expectedPerSend; k++) delivery.MarkAccepted(globalSeq, i);
            outstanding += expectedPerSend;
            tick.RecordOutstanding(outstanding);
        }

        sched.NextSend[i] += sched.IntervalTicks;
        if (sched.NextSend[i] < now) sched.NextSend[i] = now;
    }

    while (true)
    {
        nowTicks = Stopwatch.GetTimestamp();
        if (nowTicks >= tailUntilTicks) break;
        SampleRssIfDue(ps, ref nextRssSampleTicks);

        bool inDiag = nowTicks >= warmupEndTicks;
        bool inActiveSend = nowTicks >= warmupEndTicks && nowTicks < runEndTicks;
        bool didWork = false;
        tick.RecordTick(nowTicks, inDiag);

        if (nowTicks < runEndTicks)
        {
            for (uint i = 0; i < cfg.Conns; i++)
            {
                TrySendOn(schedR, i, nowTicks, ref didWork, inActiveSend);
                TrySendOn(schedU, i, nowTicks, ref didWork, inActiveSend);
            }
        }

        for (uint i = 0; i < cfg.Conns; i++) managers[i].PollEvents();

        ulong drainedThisTick = 0;
        foreach (var msg in inbox.Entries)
        {
            drainedThisTick++;
            nowTicks = Stopwatch.GetTimestamp();
            bool inMeasureNow = nowTicks >= warmupEndTicks;

            if (msg.Length < Config.MinPayloadBytes) continue;

            ulong seq = BitConverter.ToUInt64(msg.Buffer, 0);
            ulong tsNs = BitConverter.ToUInt64(msg.Buffer, 8);
            bool reliableMsg = msg.Buffer[16] != 0;
            ulong nowNs = (ulong)(nowTicks * 1_000_000_000L / Stopwatch.Frequency);
            ulong rttUs = (nowNs > tsNs) ? (nowNs - tsNs) / 1000 : 0;

            if (inMeasureNow)
            {
                (reliableMsg ? rttR : rttU).RecordUs(rttUs);
                throughput.Record((ulong)msg.Length);
                if (delivery.MarkReceived(seq, msg.Conn) && outstanding > 0)
                    outstanding--;
            }
        }
        if (inDiag && drainedThisTick > 0) tick.RecordRecvDrained(drainedThisTick);
        if (drainedThisTick > 0) didWork = true;
        inbox.Clear();
        if (!didWork && outstanding == 0 && cfg.IdlePolicy == "adaptive")
            Thread.Sleep(0);
    }

    ps.End();
    for (uint i = 0; i < cfg.Conns; i++) managers[i].Stop();

    ulong tickGapP99Us = tick.TickGapP99Us;
    ulong pacingLagP99Us = tick.PacingLagP99Us;
    double attemptedRatio = targetAttempted > 0 ? (double)tick.Attempted / targetAttempted : 1.0;
    double acceptedRatio = tick.Attempted > 0 ? (double)tick.Accepted / tick.Attempted : 0.0;
    // See harness/runner.cc tick_ok comment: pacing_lag stays diagnostic-only
    // because attempted/accepted ratios are what actually catch broken runs.
    // tick_gap budget scales with rate so heavy-poll libraries can still
    // pass at low send rates.
    ulong tickGapBudgetUs = combinedRate > 0
        ? Math.Max(250UL, 1_000_000UL / combinedRate / 4)
        : 250UL;
    bool tickOk = tick.TickSamples > 0 &&
        tickGapP99Us <= tickGapBudgetUs &&
        acceptedRatio >= 0.99;
    if (combinedRate > 0)
    {
        tickOk = tickOk && attemptedRatio >= 0.99;
    }

    return new CsvRow
    {
        Library = cfg.Library,
        Encryption = "off",
        Phase = 1,
        RateR = cfg.RateR,
        RateU = cfg.RateU,
        Size = cfg.SizeBytes,
        Conns = cfg.Conns,
        Loss = cfg.Loss,
        ThroughputMbps = throughput.Bytes * 8.0 / (cfg.DurationS * 1_000_000.0),
        MsgPerSec = throughput.Messages / Math.Max(1u, cfg.DurationS),
        RttRP50Us = rttR.PercentileUs(0.50),
        RttRP95Us = rttR.PercentileUs(0.95),
        RttRP99Us = rttR.PercentileUs(0.99),
        RttUP50Us = rttU.PercentileUs(0.50),
        RttUP95Us = rttU.PercentileUs(0.95),
        RttUP99Us = rttU.PercentileUs(0.99),
        Delivered = delivery.Received,
        Accepted = delivery.Accepted,
        DeliveryRatio = delivery.DeliveryRatio,
        CpuPct = ps.CpuPct(),
        RssMb = ps.RssMbMax,
        ConnectMs = connectMs,
        DurationS = cfg.DurationS,
        Mode = cfg.Mode,
        IdlePolicy = cfg.IdlePolicy,
        FlushPolicy = FlushPolicy(cfg),
        ClientTickGapP99Us = tickGapP99Us,
        ClientTickGapMaxUs = tick.TickGapMaxUs,
        ClientPacingLagP99Us = pacingLagP99Us,
        ClientPacingLagMaxUs = tick.PacingLagMaxUs,
        ClientMissedPacing = tick.MissedPacing,
        ClientAttempted = tick.Attempted,
        ClientAccepted = tick.Accepted,
        ClientAttemptedRatio = attemptedRatio,
        ClientAcceptedRatio = acceptedRatio,
        ClientRecvDrainedP99 = tick.RecvDrainedP99,
        ClientRecvDrainedMax = tick.RecvDrainedMax,
        ClientOutstandingMax = tick.OutstandingMax,
        ClientTickOk = tickOk ? 1u : 0u,
    };
}

// ---------------------------------------------------------------------------
// CSV — カラム順は harness/csv_writer.h の write_row と完全一致させること
// ---------------------------------------------------------------------------

static string FlushPolicy(Config _) => "library_internal";

static string CsvHeader() =>
    "library,encryption,phase,rate_r,rate_u,size,conns,loss," +
    "throughput_mbps,msg_per_sec," +
    "rtt_r_p50_us,rtt_r_p95_us,rtt_r_p99_us," +
    "rtt_u_p50_us,rtt_u_p95_us,rtt_u_p99_us," +
    "delivered,accepted,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s," +
    "mode,idle_policy,flush_policy,client_tick_gap_p99_us," +
    "client_tick_gap_max_us," +
    "client_pacing_lag_p99_us,client_pacing_lag_max_us," +
    "client_missed_pacing,client_attempted,client_accepted," +
    "client_attempted_ratio,client_accepted_ratio," +
    "client_recv_drained_p99,client_recv_drained_max," +
    "client_outstanding_max,client_tick_ok,delivery_dedup_policy";

static string FormatRow(CsvRow r) =>
    $"{r.Library},{r.Encryption},{r.Phase}," +
    $"{r.RateR},{r.RateU}," +
    $"{r.Size},{r.Conns}," +
    $"{r.Loss.ToString("F3", CultureInfo.InvariantCulture)}," +
    $"{r.ThroughputMbps.ToString("F3", CultureInfo.InvariantCulture)}," +
    $"{r.MsgPerSec}," +
    $"{r.RttRP50Us},{r.RttRP95Us},{r.RttRP99Us}," +
    $"{r.RttUP50Us},{r.RttUP95Us},{r.RttUP99Us}," +
    $"{r.Delivered},{r.Accepted}," +
    $"{r.DeliveryRatio.ToString("F4", CultureInfo.InvariantCulture)}," +
    $"{r.CpuPct.ToString("F2", CultureInfo.InvariantCulture)}," +
    $"{r.RssMb},{r.ConnectMs},{r.DurationS},{r.Mode},{r.IdlePolicy},{r.FlushPolicy}," +
    $"{r.ClientTickGapP99Us},{r.ClientTickGapMaxUs}," +
    $"{r.ClientPacingLagP99Us},{r.ClientPacingLagMaxUs}," +
    $"{r.ClientMissedPacing},{r.ClientAttempted},{r.ClientAccepted}," +
    $"{r.ClientAttemptedRatio.ToString("F4", CultureInfo.InvariantCulture)}," +
    $"{r.ClientAcceptedRatio.ToString("F4", CultureInfo.InvariantCulture)}," +
    $"{r.ClientRecvDrainedP99},{r.ClientRecvDrainedMax}," +
    $"{r.ClientOutstandingMax},{r.ClientTickOk},{r.DeliveryDedupPolicy}";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

class Config
{
    public const uint MinPayloadBytes = 17;  // 8B seq + 8B ts + 1B reliable flag
    public const uint MaxPayloadBytes = 1000;

    public string Library = "litenetlib";
    public string Role = "client";
    public string Host = "127.0.0.1";
    public ushort Port = 9000;
    public uint RateR;       // reliable msg/s/conn (0 = no reliable traffic)
    public uint RateU;       // unreliable msg/s/conn (0 = no unreliable traffic)
    public uint SizeBytes = 64;
    public uint Conns = 1;
    public uint DurationS = 30;
    public uint WarmupS = 5; // spec: JIT/GC ウォームアップのため 5 秒デフォルト
    public double Loss = 0.0;
    public string Mode = "echo";  // "echo" / "broadcast"
    public string IdlePolicy = "spin"; // "spin" / "adaptive"
    public string OutPath = "";
}

// ---------------------------------------------------------------------------
// CsvRow
// ---------------------------------------------------------------------------

class CsvRow
{
    public string Library = "";
    public string Encryption = "off";
    public int Phase = 1;
    public uint RateR;
    public uint RateU;
    public uint Size;
    public uint Conns;
    public double Loss;
    public double ThroughputMbps;
    public ulong MsgPerSec;
    public ulong RttRP50Us;
    public ulong RttRP95Us;
    public ulong RttRP99Us;
    public ulong RttUP50Us;
    public ulong RttUP95Us;
    public ulong RttUP99Us;
    public ulong Delivered;
    public ulong Accepted;
    public double DeliveryRatio;
    public double CpuPct;
    public ulong RssMb;
    public ulong ConnectMs;
    public uint DurationS;
    public string Mode = "echo";
    public string IdlePolicy = "spin";
    public string FlushPolicy = "library_internal";
    public ulong ClientTickGapP99Us;
    public ulong ClientTickGapMaxUs;
    public ulong ClientPacingLagP99Us;
    public ulong ClientPacingLagMaxUs;
    public ulong ClientMissedPacing;
    public ulong ClientAttempted;
    public ulong ClientAccepted;
    public double ClientAttemptedRatio;
    public double ClientAcceptedRatio;
    public ulong ClientRecvDrainedP99;
    public ulong ClientRecvDrainedMax;
    public ulong ClientOutstandingMax;
    public uint ClientTickOk;
    public string DeliveryDedupPolicy = DeliveryTracker.DedupPolicy;
}

sealed class PooledInbox<TConn>
{
    public readonly struct Entry
    {
        public Entry(TConn conn, byte[] buffer, int length)
        {
            Conn = conn;
            Buffer = buffer;
            Length = length;
        }

        public TConn Conn { get; }
        public byte[] Buffer { get; }
        public int Length { get; }
    }

    private readonly List<Entry> entries_ = new();

    public IReadOnlyList<Entry> Entries => entries_;
    public int Count => entries_.Count;

    public void Add(TConn conn, NetPacketReader reader)
    {
        int len = reader.AvailableBytes;
        byte[] buf = ArrayPool<byte>.Shared.Rent(len);
        reader.GetBytes(buf, len);
        entries_.Add(new Entry(conn, buf, len));
    }

    public void Clear()
    {
        foreach (var entry in entries_)
            ArrayPool<byte>.Shared.Return(entry.Buffer);
        entries_.Clear();
    }
}

static class TickUtil
{
    public static ulong TicksToUs(long ticks) =>
        ticks <= 0 ? 0UL : (ulong)(ticks * 1_000_000L / Stopwatch.Frequency);

    public static ulong PacingBudgetUs(uint ratePerConn)
    {
        if (ratePerConn == 0) return 0;
        ulong intervalUs = 1_000_000UL / ratePerConn;
        // Mirrors harness/runner.cc pacing_budget_us: 10% of interval with
        // a 100us floor so high-rate runs still flag starvation while
        // low-rate runs do not fail over single-digit microsecond overruns.
        return Math.Max(100UL, intervalUs / 10UL);
    }
}

sealed class ChannelSched
{
    public uint Rate { get; }
    public bool Reliable { get; }
    public long IntervalTicks { get; }
    public long[] NextSend { get; }

    public bool Active => Rate > 0;

    public ChannelSched(uint rate, bool reliable, uint conns, long t0)
    {
        Rate = rate;
        Reliable = reliable;
        IntervalTicks = rate > 0 ? Stopwatch.Frequency / (long)rate : 0;
        NextSend = new long[conns];
        for (int i = 0; i < conns; i++) NextSend[i] = t0;
    }
}

class BoundedHistogram
{
    private readonly ulong[] bins_;
    private ulong count_;
    private ulong max_;
    private ulong overflow_;

    public BoundedHistogram(int maxExact)
    {
        bins_ = new ulong[maxExact + 1];
    }

    public void Record(ulong value)
    {
        count_++;
        if (value > max_) max_ = value;
        if (value < (ulong)bins_.Length)
            bins_[value]++;
        else
            overflow_++;
    }

    public ulong PercentilePerMille(uint perMille)
    {
        if (count_ == 0) return 0;
        ulong target = (count_ * perMille + 999) / 1000;
        ulong seen = 0;
        for (int i = 0; i < bins_.Length; i++)
        {
            seen += bins_[i];
            if (seen >= target) return (ulong)i;
        }
        _ = overflow_;
        return max_;
    }

    public ulong Max => max_;
    public ulong Count => count_;
}

class ClientTickStats
{
    private readonly BoundedHistogram tickGapUs_ = new(10_000);
    private readonly BoundedHistogram pacingLagUs_ = new(10_000);
    private readonly BoundedHistogram recvDrained_ = new(10_000);
    private bool haveLastTick_;
    private long lastTick_;

    public ulong MissedPacing { get; private set; }
    public ulong Attempted { get; private set; }
    public ulong Accepted { get; private set; }
    public ulong OutstandingMax { get; private set; }

    public void RecordTick(long nowTicks, bool inDiag)
    {
        if (haveLastTick_ && inDiag)
            tickGapUs_.Record(TickUtil.TicksToUs(nowTicks - lastTick_));
        lastTick_ = nowTicks;
        haveLastTick_ = true;
    }

    public void RecordSendDue(ulong expectedDeliveries, ulong lagUs, ulong missedBudgetUs)
    {
        Attempted += expectedDeliveries;
        pacingLagUs_.Record(lagUs);
        if (lagUs > missedBudgetUs) MissedPacing++;
    }

    public void RecordAccepted(ulong expectedDeliveries) => Accepted += expectedDeliveries;

    public void RecordRecvDrained(ulong n) => recvDrained_.Record(n);

    public void RecordOutstanding(ulong n)
    {
        if (n > OutstandingMax) OutstandingMax = n;
    }

    public ulong TickSamples => tickGapUs_.Count;
    public ulong TickGapP99Us => tickGapUs_.PercentilePerMille(990);
    public ulong TickGapMaxUs => tickGapUs_.Max;
    public ulong PacingLagP99Us => pacingLagUs_.PercentilePerMille(990);
    public ulong PacingLagMaxUs => pacingLagUs_.Max;
    public ulong RecvDrainedP99 => recvDrained_.PercentilePerMille(990);
    public ulong RecvDrainedMax => recvDrained_.Max;
}

// ---------------------------------------------------------------------------
// Metrics — harness/metrics.cc の LatencyHist / ThroughputCounter / DeliveryTracker を踏襲
// ---------------------------------------------------------------------------

class LatencyHist
{
    private const ulong ExactMaxUs = 10_000UL;
    private const ulong FineMaxUs = 1_000_000UL;
    private const ulong CoarseMaxUs = 60_000_000UL;
    private const ulong FineBinUs = 100UL;
    private const ulong CoarseBinUs = 1_000UL;
    private const int ExactBins = 10_001;
    private const int FineBins = 9_900;
    private const int CoarseBins = 59_000;
    private const int BinCount = ExactBins + FineBins + CoarseBins;

    private readonly ulong[] bins_ = new ulong[BinCount];
    private ulong count_;
    private ulong overflow_;
    private ulong max_;

    public void RecordUs(ulong us)
    {
        count_++;
        if (us > max_) max_ = us;
        int index = BinIndex(us);
        if (index < 0)
        {
            overflow_++;
            return;
        }
        bins_[index]++;
    }

    public ulong PercentileUs(double p)
    {
        if (count_ == 0) return 0;
        double q = Math.Clamp(p, 0.0, 1.0);
        ulong target = (ulong)(q * (double)(count_ - 1)) + 1;
        ulong seen = 0;
        for (int i = 0; i < bins_.Length; i++)
        {
            seen += bins_[i];
            if (seen >= target) return BinUpperBoundUs(i);
        }
        _ = overflow_;
        return max_;
    }

    private static int BinIndex(ulong us)
    {
        if (us <= ExactMaxUs) return (int)us;
        if (us <= FineMaxUs)
            return ExactBins + (int)((us - ExactMaxUs - 1) / FineBinUs);
        if (us <= CoarseMaxUs)
            return ExactBins + FineBins + (int)((us - FineMaxUs - 1) / CoarseBinUs);
        return -1;
    }

    private static ulong BinUpperBoundUs(int index)
    {
        if (index < ExactBins) return (ulong)index;
        if (index < ExactBins + FineBins)
            return ExactMaxUs + ((ulong)(index - ExactBins) + 1) * FineBinUs;
        return FineMaxUs + ((ulong)(index - ExactBins - FineBins) + 1) * CoarseBinUs;
    }
}

class ThroughputCounter
{
    private ulong bytes_;
    private ulong messages_;
    public void Record(ulong n) { bytes_ += n; messages_++; }
    public ulong Bytes => bytes_;
    public ulong Messages => messages_;
}

class DeliveryTracker
{
    public const string DedupPolicy = "sliding_window_65536_per_conn";
    private const int DedupWindowPerConn = 65_536;
    private const ulong SeqMask = 0x0000FFFFFFFFFFFFul;

    private sealed class ConnDedupWindow
    {
        public readonly Queue<ulong> Order = new();
        public readonly HashSet<ulong> Keys = new();
    }

    private ulong acceptedCount_;
    private ulong receivedCount_;
    private readonly Dictionary<uint, ConnDedupWindow> receivedByConn_ = new();

    public void MarkAccepted(ulong seq, uint connId)
    {
        acceptedCount_++;
        _ = seq;
        _ = connId;
    }
    public bool MarkReceived(ulong seq, uint connId)
    {
        if (!receivedByConn_.TryGetValue(connId, out var window))
        {
            window = new ConnDedupWindow();
            receivedByConn_[connId] = window;
        }
        ulong key = seq & SeqMask;
        if (!window.Keys.Add(key)) return false;
        window.Order.Enqueue(key);
        if (window.Order.Count > DedupWindowPerConn)
            window.Keys.Remove(window.Order.Dequeue());
        receivedCount_++;
        return true;
    }

    public ulong Accepted => acceptedCount_;
    public ulong Received => receivedCount_;
    public double DeliveryRatio => acceptedCount_ > 0 ? (double)receivedCount_ / acceptedCount_ : 0.0;
}

// ---------------------------------------------------------------------------
// ProcSampler — harness/proc_sampler.cc の cpu_pct / rss_max_mb を踏襲
// ---------------------------------------------------------------------------

class ProcSampler
{
    public static readonly long RssSampleIntervalTicks = Math.Max(1L, Stopwatch.Frequency / 10);

    private TimeSpan cpuBefore_;
    private TimeSpan cpuAfter_;
    private long wallBeforeTicks_;
    private long wallAfterTicks_;
    private ulong rssMbMax_;
    private ulong rssSamples_;

    public void Begin()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuBefore_ = proc.TotalProcessorTime;
        wallBeforeTicks_ = Stopwatch.GetTimestamp();
        rssMbMax_ = 0;
        rssSamples_ = 0;
        SampleRss();
    }

    public void SampleRss()
    {
        ulong now = ReadRssMb();
        if (now > rssMbMax_) rssMbMax_ = now;
        rssSamples_++;
    }

    public void End()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuAfter_ = proc.TotalProcessorTime;
        wallAfterTicks_ = Stopwatch.GetTimestamp();
        SampleRss();
    }

    public double CpuPct()
    {
        double cpuS = (cpuAfter_ - cpuBefore_).TotalSeconds;
        double wallS = (double)(wallAfterTicks_ - wallBeforeTicks_) / Stopwatch.Frequency;
        return wallS > 0 ? cpuS / wallS * 100.0 : 0.0;
    }

    public ulong RssMbMax => rssMbMax_;
    public ulong RssSamples => rssSamples_;

    // /proc/self/status の VmRSS 行から RSS を読む (harness/proc_sampler.cc と同じアプローチ)
    private static ulong ReadRssMb()
    {
        try
        {
            foreach (var line in File.ReadLines("/proc/self/status"))
            {
                if (!line.StartsWith("VmRSS:", StringComparison.Ordinal)) continue;
                ulong kb = 0;
                foreach (char c in line)
                    if (c >= '0' && c <= '9') kb = kb * 10 + (ulong)(c - '0');
                return kb / 1024;
            }
        }
        catch { /* /proc 未対応環境では 0 を返す */ }
        return 0;
    }
}
