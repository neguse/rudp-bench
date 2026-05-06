// LiteNetLib adapter for rudp-bench
// C# / .NET 8 独立バイナリ — harness/csv_writer.h の CSV カラム順を完全に踏襲する
//
// CSV column order (mirrors harness/csv_writer.h write_row exactly):
//   library,encryption,phase,reliable,size,conns,rate,loss,
//   throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,
//   delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s

using LiteNetLib;
using System;
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
        "--host= --port= --reliable=r|u --size= --conns= --rate= " +
        "--duration= --warmup= --loss= --out=");
    return 2;
}

CsvRow row = cfg.Role == "server" ? RunServer(cfg) : RunClient(cfg);

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
        else if (arg.StartsWith("--reliable="))
        {
            cfg.ReliableMode = arg["--reliable=".Length..];
            if (cfg.ReliableMode != "r" && cfg.ReliableMode != "u" && cfg.ReliableMode != "na") return null;
        }
        else if (arg.StartsWith("--size=")) cfg.SizeBytes = uint.Parse(arg["--size=".Length..]);
        else if (arg.StartsWith("--conns=")) cfg.Conns = uint.Parse(arg["--conns=".Length..]);
        else if (arg.StartsWith("--rate=")) cfg.Rate = uint.Parse(arg["--rate=".Length..]);
        else if (arg.StartsWith("--duration=")) cfg.DurationS = uint.Parse(arg["--duration=".Length..]);
        else if (arg.StartsWith("--warmup=")) cfg.WarmupS = uint.Parse(arg["--warmup=".Length..]);
        else if (arg.StartsWith("--loss=")) cfg.Loss = double.Parse(arg["--loss=".Length..], CultureInfo.InvariantCulture);
        else if (arg.StartsWith("--mode="))
        {
            cfg.Mode = arg["--mode=".Length..];
            if (cfg.Mode != "echo" && cfg.Mode != "broadcast") return null;
        }
        else if (arg.StartsWith("--out=")) cfg.OutPath = arg["--out=".Length..];
        else { Console.Error.WriteLine($"unknown flag: {arg}"); return null; }
    }
    if (string.IsNullOrEmpty(cfg.Library)) return null;
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
    var inbox = new List<(NetPeer peer, byte[] data)>();

    listener.ConnectionRequestEvent += req => req.AcceptIfKey("rudpbench");
    listener.NetworkReceiveEvent += (peer, reader, _, _) =>
    {
        inbox.Add((peer, reader.GetRemainingBytes()));
    };

    manager.Start(cfg.Port);

    var ps = new ProcSampler();
    ps.Begin();

    var deliveryMethod = cfg.ReliableMode == "r"
        ? DeliveryMethod.ReliableOrdered
        : DeliveryMethod.Unreliable;

    // クライアントの warmup + duration + tail_drain を収容するため +5秒余裕
    long deadlineTicks = Stopwatch.GetTimestamp() +
        (long)((cfg.WarmupS + cfg.DurationS + 5) * Stopwatch.Frequency);

    var knownPeers = new HashSet<NetPeer>();
    bool broadcast = cfg.Mode == "broadcast";

    while (Stopwatch.GetTimestamp() < deadlineTicks)
    {
        manager.PollEvents();

        foreach (var (peer, data) in inbox)
        {
            knownPeers.Add(peer);
            if (broadcast)
            {
                foreach (var target in knownPeers)
                    target.Send(data, deliveryMethod);
            }
            else
            {
                peer.Send(data, deliveryMethod);
            }
        }
        inbox.Clear();

        Thread.Sleep(0);
    }

    ps.End();
    manager.Stop();

    return new CsvRow
    {
        Library = cfg.Library,
        Encryption = "off",
        Phase = 1,
        Reliable = cfg.ReliableMode,
        Size = cfg.SizeBytes,
        Conns = cfg.Conns,
        Rate = cfg.Rate,
        Loss = cfg.Loss,
        DurationS = cfg.DurationS,
        CpuPct = ps.CpuPct(),
        RssMb = ps.RssMbMax,
        Mode = cfg.Mode,
    };
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

static CsvRow RunClient(Config cfg)
{
    var listener = new EventBasedNetListener();
    var manager = new NetManager(listener) { AutoRecycle = true };

    // peer → conn_id マッピング
    var peerToId = new Dictionary<NetPeer, uint>(32);
    var peers = new NetPeer[cfg.Conns];
    var connected = new bool[cfg.Conns];

    // 受信メッセージキュー（PollEvents()内で同期的にpush）
    var inbox = new List<(uint connId, byte[] data)>();

    listener.PeerConnectedEvent += peer =>
    {
        if (peerToId.TryGetValue(peer, out uint id))
            connected[id] = true;
    };

    listener.NetworkReceiveEvent += (peer, reader, _, _) =>
    {
        if (peerToId.TryGetValue(peer, out uint id))
            inbox.Add((id, reader.GetRemainingBytes()));
    };

    manager.Start();

    // 全コネクション発行
    long tConnectBeginTicks = Stopwatch.GetTimestamp();
    for (uint i = 0; i < cfg.Conns; i++)
    {
        var peer = manager.Connect(cfg.Host, cfg.Port, "rudpbench");
        peers[i] = peer;
        peerToId[peer] = i;
    }

    // 全コネクションが ready になるまで poll
    while (!connected.All(c => c))
    {
        manager.PollEvents();
        Thread.Sleep(1);
    }
    long tConnectEndTicks = Stopwatch.GetTimestamp();
    ulong connectMs = (ulong)((tConnectEndTicks - tConnectBeginTicks) * 1000L / Stopwatch.Frequency);

    var ps = new ProcSampler();
    ps.Begin();

    // タイミングは Stopwatch ベース（DateTime.UtcNow より高精度）
    long nowTicks = Stopwatch.GetTimestamp();
    long warmupEndTicks = nowTicks + (long)(cfg.WarmupS * Stopwatch.Frequency);
    long runEndTicks = warmupEndTicks + (long)(cfg.DurationS * Stopwatch.Frequency);
    long tailUntilTicks = runEndTicks + Stopwatch.Frequency / 2; // +500ms tail drain

    var rtt = new LatencyHist();
    var throughput = new ThroughputCounter();
    var delivery = new DeliveryTracker();

    // ペーシング: コネクションごとの次回送信時刻(ticks)
    long intervalTicks = cfg.Rate > 0 ? Stopwatch.Frequency / (long)cfg.Rate : 0;
    var nextSendTicks = new long[cfg.Conns];
    long t0 = Stopwatch.GetTimestamp();
    for (int i = 0; i < cfg.Conns; i++) nextSendTicks[i] = t0;

    var payload = new byte[Math.Max(cfg.SizeBytes, 16)];
    Array.Fill(payload, (byte)0xAB);
    var seqCounters = new ulong[cfg.Conns];
    for (int i = 0; i < cfg.Conns; i++) seqCounters[i] = 1;

    var deliveryMethod = cfg.ReliableMode == "r"
        ? DeliveryMethod.ReliableOrdered
        : DeliveryMethod.Unreliable;

    while (true)
    {
        nowTicks = Stopwatch.GetTimestamp();
        if (nowTicks >= tailUntilTicks) break;

        bool inMeasure = nowTicks >= warmupEndTicks;

        if (nowTicks < runEndTicks)
        {
            for (uint i = 0; i < cfg.Conns; i++)
            {
                if (nowTicks < nextSendTicks[i]) continue;

                ulong localSeq = seqCounters[i]++;
                // src_idx を上位 32bit に乗せて global seq に
                // (broadcast 時の dedup key が src 違いで衝突しないように)
                ulong globalSeq = ((ulong)i << 32) | localSeq;
                // 単調クロックのナノ秒タイムスタンプ（C++ runner.cc と同形式）
                ulong tsNs = (ulong)(Stopwatch.GetTimestamp() * 1_000_000_000L / Stopwatch.Frequency);
                BitConverter.TryWriteBytes(payload.AsSpan(0, 8), globalSeq);
                BitConverter.TryWriteBytes(payload.AsSpan(8, 8), tsNs);

                // LiteNetLib 2.x: Send() は void を返す。送信失敗の検出は省略し常に MarkSent
                peers[i].Send(payload, deliveryMethod);
                if (inMeasure)
                {
                    uint expected = cfg.Mode == "broadcast" ? cfg.Conns : 1u;
                    for (uint k = 0; k < expected; k++) delivery.MarkSent(globalSeq, i);
                }

                if (intervalTicks > 0)
                {
                    nextSendTicks[i] += intervalTicks;
                    if (nextSendTicks[i] < nowTicks) nextSendTicks[i] = nowTicks;
                }
            }
        }

        manager.PollEvents();

        foreach (var (connId, data) in inbox)
        {
            nowTicks = Stopwatch.GetTimestamp();
            bool inMeasureNow = nowTicks >= warmupEndTicks;

            if (data.Length < 16) continue;

            ulong seq = BitConverter.ToUInt64(data, 0);
            ulong tsNs = BitConverter.ToUInt64(data, 8);
            ulong nowNs = (ulong)(nowTicks * 1_000_000_000L / Stopwatch.Frequency);
            ulong rttUs = (nowNs > tsNs) ? (nowNs - tsNs) / 1000 : 0;

            if (inMeasureNow)
            {
                rtt.RecordUs(rttUs);
                throughput.Record((ulong)data.Length);
                delivery.MarkReceived(seq, connId);
            }
        }
        inbox.Clear();
    }

    ps.End();
    manager.Stop();

    return new CsvRow
    {
        Library = cfg.Library,
        Encryption = "off",
        Phase = 1,
        Reliable = cfg.ReliableMode,
        Size = cfg.SizeBytes,
        Conns = cfg.Conns,
        Rate = cfg.Rate,
        Loss = cfg.Loss,
        ThroughputMbps = throughput.Bytes * 8.0 / (cfg.DurationS * 1_000_000.0),
        MsgPerSec = throughput.Messages / Math.Max(1u, cfg.DurationS),
        RttP50Us = rtt.PercentileUs(0.50),
        RttP95Us = rtt.PercentileUs(0.95),
        RttP99Us = rtt.PercentileUs(0.99),
        Delivered = delivery.Received,
        Sent = delivery.Sent,
        DeliveryRatio = delivery.DeliveryRatio,
        CpuPct = ps.CpuPct(),
        RssMb = ps.RssMbMax,
        ConnectMs = connectMs,
        DurationS = cfg.DurationS,
        Mode = cfg.Mode,
    };
}

// ---------------------------------------------------------------------------
// CSV — カラム順は harness/csv_writer.h の write_row と完全一致させること
// ---------------------------------------------------------------------------

static string CsvHeader() =>
    "library,encryption,phase,reliable,size,conns,rate,loss," +
    "throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us," +
    "delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,mode";

static string FormatRow(CsvRow r) =>
    $"{r.Library},{r.Encryption},{r.Phase},{r.Reliable}," +
    $"{r.Size},{r.Conns},{r.Rate}," +
    $"{r.Loss.ToString("F3", CultureInfo.InvariantCulture)}," +
    $"{r.ThroughputMbps.ToString("F3", CultureInfo.InvariantCulture)}," +
    $"{r.MsgPerSec}," +
    $"{r.RttP50Us},{r.RttP95Us},{r.RttP99Us}," +
    $"{r.Delivered},{r.Sent}," +
    $"{r.DeliveryRatio.ToString("F4", CultureInfo.InvariantCulture)}," +
    $"{r.CpuPct.ToString("F2", CultureInfo.InvariantCulture)}," +
    $"{r.RssMb},{r.ConnectMs},{r.DurationS},{r.Mode}";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

class Config
{
    public string Library = "litenetlib";
    public string Role = "client";
    public string Host = "127.0.0.1";
    public ushort Port = 9000;
    public string ReliableMode = "u"; // "r"/"u"/"na"
    public uint SizeBytes = 64;
    public uint Conns = 1;
    public uint Rate = 0;    // 0 = unbounded
    public uint DurationS = 30;
    public uint WarmupS = 5; // spec: JIT/GC ウォームアップのため 5 秒デフォルト
    public double Loss = 0.0;
    public string Mode = "echo";  // "echo" / "broadcast"
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
    public string Reliable = "u";
    public uint Size;
    public uint Conns;
    public uint Rate;
    public double Loss;
    public double ThroughputMbps;
    public ulong MsgPerSec;
    public ulong RttP50Us;
    public ulong RttP95Us;
    public ulong RttP99Us;
    public ulong Delivered;
    public ulong Sent;
    public double DeliveryRatio;
    public double CpuPct;
    public ulong RssMb;
    public ulong ConnectMs;
    public uint DurationS;
    public string Mode = "echo";
}

// ---------------------------------------------------------------------------
// Metrics — harness/metrics.cc の LatencyHist / ThroughputCounter / DeliveryTracker を踏襲
// ---------------------------------------------------------------------------

class LatencyHist
{
    private readonly List<ulong> samples_ = new();
    private bool sorted_ = false;

    public void RecordUs(ulong us) { samples_.Add(us); sorted_ = false; }

    public ulong PercentileUs(double p)
    {
        if (samples_.Count == 0) return 0;
        if (!sorted_) { samples_.Sort(); sorted_ = true; }
        int idx = (int)(p * (samples_.Count - 1));
        return samples_[idx];
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
    private ulong sentCount_;
    private ulong receivedCount_;
    private readonly HashSet<ulong> receivedKeys_ = new();

    // pack(seq, connId) は harness/metrics.cc と同じビット割り当て
    private static ulong Pack(ulong seq, uint connId) =>
        ((ulong)connId << 48) | (seq & 0x0000FFFFFFFFFFFFul);

    public void MarkSent(ulong seq, uint connId) { sentCount_++; _ = Pack(seq, connId); }
    public void MarkReceived(ulong seq, uint connId)
    {
        if (receivedKeys_.Add(Pack(seq, connId))) receivedCount_++;
    }

    public ulong Sent => sentCount_;
    public ulong Received => receivedCount_;
    public double DeliveryRatio => sentCount_ > 0 ? (double)receivedCount_ / sentCount_ : 0.0;
}

// ---------------------------------------------------------------------------
// ProcSampler — harness/proc_sampler.cc の cpu_pct / rss_max_mb を踏襲
// ---------------------------------------------------------------------------

class ProcSampler
{
    private TimeSpan cpuBefore_;
    private TimeSpan cpuAfter_;
    private long wallBeforeTicks_;
    private long wallAfterTicks_;
    private ulong rssMbMax_;

    public void Begin()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuBefore_ = proc.TotalProcessorTime;
        wallBeforeTicks_ = Stopwatch.GetTimestamp();
        rssMbMax_ = ReadRssMb();
    }

    public void End()
    {
        var proc = Process.GetCurrentProcess();
        proc.Refresh();
        cpuAfter_ = proc.TotalProcessorTime;
        wallAfterTicks_ = Stopwatch.GetTimestamp();
        ulong now = ReadRssMb();
        if (now > rssMbMax_) rssMbMax_ = now;
    }

    public double CpuPct()
    {
        double cpuS = (cpuAfter_ - cpuBefore_).TotalSeconds;
        double wallS = (double)(wallAfterTicks_ - wallBeforeTicks_) / Stopwatch.Frequency;
        return wallS > 0 ? cpuS / wallS * 100.0 : 0.0;
    }

    public ulong RssMbMax => rssMbMax_;

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
