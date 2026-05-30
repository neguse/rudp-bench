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

static void SampleProcIfDue(ProcSampler ps, ref long nextSampleTicks)
{
    long nowTicks = Stopwatch.GetTimestamp();
    if (nowTicks < nextSampleTicks) return;
    ps.SampleRss();
    ps.SampleCpu();  // M1
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
        else if (arg.StartsWith("--port="))
        {
            if (!TryParseUShort(arg["--port=".Length..], out cfg.Port)) return null;
        }
        else if (arg.StartsWith("--rate-r="))
        {
            if (!TryParseUInt(arg["--rate-r=".Length..], out cfg.RateR)) return null;
        }
        else if (arg.StartsWith("--rate-u="))
        {
            if (!TryParseUInt(arg["--rate-u=".Length..], out cfg.RateU)) return null;
        }
        else if (arg.StartsWith("--size="))
        {
            if (!TryParseUInt(arg["--size=".Length..], out cfg.SizeBytes)) return null;
        }
        else if (arg.StartsWith("--conns="))
        {
            if (!TryParseUInt(arg["--conns=".Length..], out cfg.Conns)) return null;
        }
        else if (arg.StartsWith("--duration="))
        {
            if (!TryParseUInt(arg["--duration=".Length..], out cfg.DurationS)) return null;
        }
        else if (arg.StartsWith("--warmup="))
        {
            if (!TryParseUInt(arg["--warmup=".Length..], out cfg.WarmupS)) return null;
        }
        else if (arg.StartsWith("--ramp-up-ms="))
        {
            if (!TryParseUInt(arg["--ramp-up-ms=".Length..], out cfg.RampUpMs)) return null;
        }
        else if (arg.StartsWith("--loss="))
        {
            if (!TryParseLossPct(arg["--loss=".Length..], out cfg.Loss)) return null;
        }
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
        else if (arg.StartsWith("--bins-r-out=")) cfg.BinsROut = arg["--bins-r-out=".Length..];
        else if (arg.StartsWith("--bins-u-out=")) cfg.BinsUOut = arg["--bins-u-out=".Length..];
        else { Console.Error.WriteLine($"unknown flag: {arg}"); return null; }
    }
    if (string.IsNullOrEmpty(cfg.Library)) return null;
    if (cfg.RateR == 0 && cfg.RateU == 0)
    {
        Console.Error.WriteLine("at least one of --rate-r / --rate-u must be > 0");
        return null;
    }
    if (cfg.Conns == 0)
    {
        Console.Error.WriteLine("--conns must be > 0");
        return null;
    }
    return cfg;
}

static bool TryParseUInt(string text, out uint value) =>
    uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);

static bool TryParseUShort(string text, out ushort value) =>
    ushort.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);

static bool TryParseLossPct(string text, out double value)
{
    if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value))
        return false;
    return double.IsFinite(value) && value >= 0.0 && value <= 100.0;
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
    // M2: exclude warmup from the server CPU window (mirrors run_server in
    // harness/runner.cc — the server has no explicit warmup gate, so re-baseline
    // once warmup elapses).
    long serverMeasureBeginTicks = Stopwatch.GetTimestamp() + (long)(cfg.WarmupS * Stopwatch.Frequency);
    bool measureStarted = cfg.WarmupS == 0;

    // クライアントの warmup + duration + tail_drain を収容するため +2秒余裕
    long deadlineTicks = Stopwatch.GetTimestamp() +
        (long)((cfg.WarmupS + cfg.DurationS + 2) * Stopwatch.Frequency);

    var knownPeers = new HashSet<NetPeer>();
    bool broadcast = cfg.Mode == "broadcast";

    while (Stopwatch.GetTimestamp() < deadlineTicks)
    {
        if (!measureStarted && Stopwatch.GetTimestamp() >= serverMeasureBeginTicks)
        {
            ps.MarkMeasureBegin();
            measureStarted = true;
        }
        SampleProcIfDue(ps, ref nextRssSampleTicks);
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
    long tCloseBegin = Stopwatch.GetTimestamp();  // L6: time teardown
    manager.Stop();
    ulong closeMs = (ulong)((Stopwatch.GetTimestamp() - tCloseBegin) * 1000L / Stopwatch.Frequency);

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
        CpuPctPeak = ps.CpuPctPeak(),
        RssMb = ps.RssMbMax,
        CloseMs = closeMs,
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

    // 全コネクション発行(必要なら ramp-up で等間隔分散)
    long tConnectBeginTicks = Stopwatch.GetTimestamp();
    long rampIntervalTicks = (cfg.RampUpMs > 0 && cfg.Conns > 0)
        ? (long)cfg.RampUpMs * Stopwatch.Frequency / 1000L / cfg.Conns
        : 0L;
    for (uint i = 0; i < cfg.Conns; i++)
    {
        peers[i] = managers[i].Connect(cfg.Host, cfg.Port, "rudpbench");
        if (rampIntervalTicks > 0 && i + 1 < cfg.Conns)
        {
            long dueTicks = tConnectBeginTicks + rampIntervalTicks * (i + 1);
            while (Stopwatch.GetTimestamp() < dueTicks)
            {
                for (uint j = 0; j <= i; j++) managers[j].PollEvents();
                Thread.Sleep(1);
            }
        }
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
    bool measureStarted = cfg.WarmupS == 0;  // M2

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
        // D1: count accepted only when the peer is actually connected, matching
        // the C++ harness which marks accepted only on a.send()==0. NetPeer.Send
        // is void (cannot fail-return), so the connected state is the closest
        // "the transport accepted this send" signal. In steady state every peer
        // is connected (we waited for all before measuring), so this normally
        // agrees with the old unconditional count — it only differs if a peer
        // drops mid-run, which should not inflate accepted.
        if (inActive && peers[i].ConnectionState == ConnectionState.Connected)
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
        if (!measureStarted && nowTicks >= warmupEndTicks)
        {
            ps.MarkMeasureBegin();  // M2
            measureStarted = true;
        }
        SampleProcIfDue(ps, ref nextRssSampleTicks);

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
    long tCloseBegin = Stopwatch.GetTimestamp();  // L6: time teardown
    for (uint i = 0; i < cfg.Conns; i++) managers[i].Stop();
    ulong closeMs = (ulong)((Stopwatch.GetTimestamp() - tCloseBegin) * 1000L / Stopwatch.Frequency);

    // Per-channel RTT histogram sidecars (same binary layout as the C++
    // LatencyHist::write_binary) so combine_clients.py can merge a multi-proc
    // litenetlib client farm by summing bins and recomputing percentiles.
    if (!string.IsNullOrEmpty(cfg.BinsROut)) rttR.WriteBinary(cfg.BinsROut);
    if (!string.IsNullOrEmpty(cfg.BinsUOut)) rttU.WriteBinary(cfg.BinsUOut);

    ulong tickGapP99Us = tick.TickGapP99Us;
    ulong pacingLagP99Us = tick.PacingLagP99Us;
    double attemptedRatio = targetAttempted > 0 ? (double)tick.Attempted / targetAttempted : 1.0;
    double acceptedRatio = tick.Attempted > 0 ? (double)tick.Accepted / tick.Attempted : 0.0;
    // Validity = functional correctness only (attempted/accepted ratios).
    // Per-tick pacing lag (client_tick_gap_p99_us) is a diagnostic, NOT a
    // pass/fail gate -- mirrors harness/runner.cc: gating on tick_gap falsely
    // invalidated high-conn runs that emitted and accepted 100% of the load.
    bool tickOk = acceptedRatio >= 0.99;
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
        CpuPctPeak = ps.CpuPctPeak(),
        RssMb = ps.RssMbMax,
        CloseMs = closeMs,
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
    "client_outstanding_max,client_tick_ok,delivery_dedup_policy," +
    "cpu_pct_peak,close_ms";

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
    $"{r.ClientOutstandingMax},{r.ClientTickOk},{r.DeliveryDedupPolicy}," +
    $"{r.CpuPctPeak.ToString("F2", CultureInfo.InvariantCulture)},{r.CloseMs}";

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
    public uint RampUpMs = 0; // connect を等間隔に分散する時間。0 で即時(従来挙動)
    public double Loss = 0.0;
    public string Mode = "echo";  // "echo" / "broadcast"
    public string IdlePolicy = "spin"; // "spin" / "adaptive"
    public string OutPath = "";
    public string BinsROut = ""; // reliable RTT histogram sidecar (LatencyHist binary)
    public string BinsUOut = ""; // unreliable RTT histogram sidecar
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
    public double CpuPctPeak;  // M1
    public ulong CloseMs;      // L6
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

    // Dense u64 little-endian dump mirroring harness/metrics.cc
    // LatencyHist::write_binary so scripts/combine_clients.py can read it:
    //   u32 magic='LHST'(0x5453484c), u32 version=1,
    //   u64 count, overflow, max, bin_count, then u64 bins[bin_count].
    // BinaryWriter writes little-endian on all platforms.
    public void WriteBinary(string path)
    {
        using var fs = new FileStream(path, FileMode.Create, FileAccess.Write);
        using var w = new BinaryWriter(fs);
        w.Write((uint)0x5453484C);
        w.Write((uint)1);
        w.Write(count_);
        w.Write(overflow_);
        w.Write(max_);
        w.Write((ulong)BinCount);
        for (int i = 0; i < BinCount; i++) w.Write(bins_[i]);
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

    // D6: TotalProcessorTime is the .NET equivalent of getrusage(RUSAGE_SELF)
    // used by the C++ harness — both sum CPU across ALL threads of the process,
    // so the two are measuring the same quantity (whole-process CPU). The
    // sampling shape is also kept identical to harness/proc_sampler.cc: a 2-point
    // mean plus a periodic per-interval peak (M1), re-baselined at warmup_end so
    // warmup is excluded (M2).
    private TimeSpan cpuBefore_;
    private TimeSpan cpuAfter_;
    private long wallBeforeTicks_;
    private long wallAfterTicks_;
    private TimeSpan cpuLast_;
    private long cpuLastTicks_;
    private double cpuPctPeak_;
    private ulong cpuSamples_;
    private ulong rssMbMax_;
    private ulong rssSamples_;

    public void Begin()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuBefore_ = proc.TotalProcessorTime;
        wallBeforeTicks_ = Stopwatch.GetTimestamp();
        cpuLast_ = cpuBefore_;
        cpuLastTicks_ = wallBeforeTicks_;
        cpuPctPeak_ = 0.0;
        cpuSamples_ = 0;
        rssMbMax_ = 0;
        rssSamples_ = 0;
        SampleRss();
    }

    // M2: re-baseline the CPU/wall window to "now" so warmup is not counted.
    public void MarkMeasureBegin()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuBefore_ = proc.TotalProcessorTime;
        wallBeforeTicks_ = Stopwatch.GetTimestamp();
        cpuLast_ = cpuBefore_;
        cpuLastTicks_ = wallBeforeTicks_;
        cpuPctPeak_ = 0.0;
    }

    public void SampleRss()
    {
        ulong now = ReadRssMb();
        if (now > rssMbMax_) rssMbMax_ = now;
        rssSamples_++;
    }

    // M1: per-interval CPU% so short spikes surface as the peak.
    public void SampleCpu()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        TimeSpan nowCpu = proc.TotalProcessorTime;
        long nowTicks = Stopwatch.GetTimestamp();
        double wallS = (double)(nowTicks - cpuLastTicks_) / Stopwatch.Frequency;
        if (wallS > 0)
        {
            double pct = (nowCpu - cpuLast_).TotalSeconds / wallS * 100.0;
            if (pct > cpuPctPeak_) cpuPctPeak_ = pct;
            cpuSamples_++;
        }
        cpuLast_ = nowCpu;
        cpuLastTicks_ = nowTicks;
    }

    public void End()
    {
        SampleCpu();
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

    public double CpuPctPeak() => cpuPctPeak_ > 0.0 ? cpuPctPeak_ : CpuPct();

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
