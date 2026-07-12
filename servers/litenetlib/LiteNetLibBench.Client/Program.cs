using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using BenchKit.CS;
using LiteNetLib;
using LiteNetLibBench;
using LiteNetLib.Utils;

var config = ClientConfig.Parse(args);

// 計測器(client farm)側: ThreadPool の hill-climbing 起動遅延が送信 pacing の
// stall(数百 ms 級)として観測されるため、最低スレッド数を先に確保する。
// client 専用の farm 設定であり server には入れない
System.Threading.ThreadPool.SetMinThreads(Environment.ProcessorCount * 4, Environment.ProcessorCount * 4);

if (config.Describe)
{
    Console.WriteLine(LnlDescribe.Json);
    return 0;
}
if (!config.Valid)
{
    Console.Error.WriteLine(
        "usage: LiteNetLibBench.Client --host HOST --port PORT --conns N --proc-index N " +
        "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES " +
        "[--payload-lt BYTES] [--payload-md BYTES] " +
        "--deadline-ns NS --staleness-period-ns NS");
    return 1;
}
var rampConfig = BenchRampConfig.FromEnvironment(config.Conns);

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

var maxOriginId = config.Scenario is { } configuredScenario
    ? checked((uint)configuredScenario.TotalConns + 1U)
    : checked(config.OriginBase + (uint)config.Conns);
var metrics = new BenchMetrics(new BenchMetricsConfig(
    maxOriginId == 0 ? 1 : maxOriginId,
    config.DeadlineNs,
    config.StalenessPeriodNs,
    config.Scenario is null ? 0U : (uint)config.Conns));
config.Scenario?.ConfigureMetrics(metrics);

// One NetManager per conn, each StartInManualMode (no internal threads).
//
// Why not a single NetManager for all conns (as an earlier draft of this
// task assumed): LiteNetLib.LiteNetManager.Connect() keys peers by remote
// IPEndPoint and, verified by reading LiteNetManager.cs, returns the
// *existing* peer/pending-request when Connect() is called again for the
// same remote endpoint from one manager:
//   if (TryGetPeer(target, out var peer)) { ... return peer; }
// Since every conn here targets the same server host:port, one manager could
// only ever represent ONE conn to it — this is a hard library constraint, not
// a style choice (v1's adapters/litenetlib/Program.cs hit the same wall and
// documents the identical per-conn-manager workaround).
//
// The thread-explosion risk that per-conn sockets normally carry (checklist
// §1.3/§6) is avoided by running every manager in manual mode: PollEvents()
// does the receive and ManualUpdate() drives retransmit/ping/timeout logic,
// both called from this process's single loop — so thread_model is "single"
// for the client despite N sockets, per --describe.
var managers = new NetManager[config.Conns];
var peers = new NetPeer[config.Conns];
var connected = new bool[config.Conns];
var plans = new BenchPlan?[config.Conns];
var originIds = new uint[config.Conns];
var lastInputSeq = new ulong[config.Conns];
var lastAppliedInputSeq = new ulong[config.Conns];
var payloadValidation = new BenchPayloadValidation();
var progress = config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState
    ? new BenchAuthoritativeProgressTracker(config.Conns, config.Scenario.TotalConns)
    : null;

BenchControl? control = null;
try
{
    control = await BenchControl.ConnectFromEnvironmentAsync(stopCts.Token).ConfigureAwait(false);
    if (control is not null)
    {
        await control.HelloAsync("client", "litenetlib", config.ProcIndex, stopCts.Token).ConfigureAwait(false);
    }

    if (rampConfig is { } ramp)
    {
        return await RunRampAsync(
            config,
            ramp,
            managers,
            peers,
            connected,
            plans,
            originIds,
            lastInputSeq,
            lastAppliedInputSeq,
            metrics,
            payloadValidation,
            progress,
            control,
            stopCts.Token).ConfigureAwait(false);
    }

    for (var i = 0; i < config.Conns; i++)
    {
        var localIndex = (uint)i;
        originIds[i] = config.OriginBase + localIndex;

        var listener = new EventBasedNetListener();
        listener.PeerConnectedEvent += _ => connected[localIndex] = true;
        listener.NetworkReceiveEvent += (_, reader, _, _) =>
        {
            var recvTsNs = BenchClock.NowNs();
            var span = new ReadOnlySpan<byte>(reader.RawData, reader.UserDataOffset, reader.UserDataSize);
            if (BenchPayload.TryRead(span, out var header))
            {
                if (config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState)
                {
                    var state = config.Scenario.ServerState!.Value;
                    if (header.OriginId != (uint)config.Scenario.TotalConns ||
                        header.TrafficId != state.TrafficId ||
                        (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                        BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ServerToClient ||
                        !state.Accepts(header.Flags, span.Length) ||
                        !BenchPayload.TryReadAppliedInputSeq(span, out var appliedInputSeq) ||
                        appliedInputSeq > lastInputSeq[localIndex] ||
                        !BenchPayload.ValidateTargetPad(span, originIds[localIndex]))
                    {
                        payloadValidation.Invalid();
                        reader.Recycle();
                        return;
                    }
                    BenchProgress.UpdateMax(ref lastAppliedInputSeq[localIndex], appliedInputSeq);
                    progress?.RecordStateReceived((int)localIndex, header, appliedInputSeq);
                }
                else if (!BenchPayload.ValidateBody(span, header))
                {
                    payloadValidation.Invalid();
                    reader.Recycle();
                    return;
                }
                // benchspec dedup key includes the receiving local conn
                // (broadcast fanout legitimately re-delivers the same
                // (origin, class, seq) to every local conn).
                metrics.OnRecv(localIndex, header, recvTsNs);
            }
            else
            {
                payloadValidation.Invalid();
            }

            reader.Recycle();
        };

        // tune(公式ノブ、--describe に開示): MtuDiscovery 有効(既定 off だと
        // MTU 1024 固定)、native socket、DisconnectTimeout 60s(manual pump が
        // 高負荷で遅れた際の切断猶予 — 既定 5s)
        var manager = new NetManager(listener)
        {
            MtuDiscovery = true,
            UseNativeSockets = true,
            DisconnectTimeout = 60000,
            PacketPoolSize = 4096,  // 既定 1000。枯渇すると毎 packet が GC alloc
        };
        if (!manager.StartInManualMode(0))
        {
            throw new InvalidOperationException($"NetManager.StartInManualMode failed for conn {i}");
        }

        // farm 側 kernel rcvbuf を 4MB へ。LiteNetLib の SocketBufferSize は
        // const 1MB で公開ノブが無く(NetConstants.cs:48)、broadcast fanout の
        // 受信で RcvbufErrors が発火する(ledger #5/#9 — wired c256 で
        // InErrors=207k を実測)。protected フィールドへの reflection が唯一の
        // 手段。計測器(farm)の十分性の話であり SUT 側は不変。
        SetReceiveBuffer(manager, 4 * 1024 * 1024);

        NetPeer? peer;
        if (config.Scenario is null)
        {
            peer = manager.Connect(config.Host, config.Port, LnlConstants.ConnectKey);
        }
        else
        {
            var connectData = new NetDataWriter();
            connectData.Put(LnlConstants.ConnectKey);
            connectData.Put(originIds[i]);
            peer = manager.Connect(config.Host, config.Port, connectData);
        }
        if (peer is null)
        {
            throw new InvalidOperationException($"NetManager.Connect returned null for conn {i}");
        }

        managers[i] = manager;
        peers[i] = peer;
    }

    void PumpAll(float dtMs)
    {
        foreach (var manager in managers)
        {
            manager.PollEvents();
            manager.ManualUpdate(dtMs);
        }
    }

    var lastPumpNs = BenchClock.NowNs();
    while (!Array.TrueForAll(connected, static c => c))
    {
        stopCts.Token.ThrowIfCancellationRequested();
        var nowNs = BenchClock.NowNs();
        PumpAll((float)(BenchClock.SaturatingSub(nowNs, lastPumpNs) / 1_000_000.0));
        lastPumpNs = nowNs;
        await Task.Delay(1, stopCts.Token).ConfigureAwait(false);
    }

    BenchSchedule schedule;
    if (control is not null)
    {
        await control.ReadyAsync(config.Conns, stopCts.Token).ConfigureAwait(false);

        // Poll+pump while awaiting the schedule so per-conn keepalive/ack
        // processing keeps running instead of stalling until start_at
        // (see Shared/LnlPump.cs).
        var scheduleTask = control.WaitScheduleAsync(stopCts.Token);
        schedule = await LnlPump.PumpWhileAwaitingAsync(scheduleTask, PumpAll, 5, stopCts.Token)
            .ConfigureAwait(false);
    }
    else
    {
        var now = BenchClock.NowNs();
        schedule = new BenchSchedule(
            BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs),
            BenchClock.AddSaturating(BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs), ClientConfig.DevDurationNs),
            BenchClock.AddSaturating(
                BenchClock.AddSaturating(BenchClock.AddSaturating(now, ClientConfig.DevWarmupNs), ClientConfig.DevDurationNs),
                ClientConfig.DevDrainNs));
    }

    var streams = BuildStreams(config);
    if (config.Scenario is { } activeScenario)
    {
        activeScenario.RegisterExpectedLatestFlows(
            metrics,
            (uint)config.Conns,
            config.OriginBase,
            schedule.StartAtNs);
    }
    var planStartNs = BenchClock.NowNs();
    for (var i = 0; i < config.Conns; i++)
    {
        plans[i] = new BenchPlan(streams, planStartNs, schedule.StartAtNs, schedule.StopAtNs);
    }

    var markedUnsent = false;
    var steady = new BenchSteady();
    lastPumpNs = BenchClock.NowNs();
    while (!stopCts.IsCancellationRequested && BenchClock.NowNs() < schedule.DrainUntilNs)
    {
        var now = BenchClock.NowNs();
        // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
        // window を受けたら全 conn の plan に計測窓を差し替える
        // (metrics はこのループのスレッドしか触らないので raw counts は素で読める)
        if (control is not null)
        {
            var raw = metrics.RawCounts();
            var window = await steady.TickAsync(
                control, raw.Submitted, raw.RecvMeasured + raw.RecvUnmeasured, schedule, now, stopCts.Token)
                .ConfigureAwait(false);
            if (window is { } w)
            {
                if (config.Scenario is { } windowScenario)
                {
                    windowScenario.RegisterExpectedLatestFlows(
                        metrics,
                        (uint)config.Conns,
                        config.OriginBase,
                        w.StartAtNs);
                }
                schedule = w;
                foreach (var plan in plans)
                {
                    plan!.SetWindow(w.StartAtNs, w.StopAtNs);
                }
            }
        }

        if (now >= schedule.StartAtNs && now < schedule.StopAtNs)
        {
            metrics.Tick(now);
        }

        if (now < schedule.StopAtNs)
        {
            for (var i = 0; i < config.Conns; i++)
            {
                while (plans[i]!.TryNext(now, out var slot))
                {
                    SendSlot(
                        peers[i],
                        originIds[i],
                        i,
                        slot,
                        config,
                        metrics,
                        ref lastInputSeq[i],
                        progress,
                        payloadValidation);
                }
            }
        }
        else if (!markedUnsent)
        {
            MarkUnsent(plans, originIds, schedule.StopAtNs, metrics);
            markedUnsent = true;
        }

        var nowNs = BenchClock.NowNs();
        PumpAll((float)(BenchClock.SaturatingSub(nowNs, lastPumpNs) / 1_000_000.0));
        lastPumpNs = nowNs;

        now = BenchClock.NowNs();
        var nextNs = schedule.DrainUntilNs;
        if (now < schedule.StopAtNs)
        {
            var due = NextPlanDue(plans);
            if (due < nextNs)
            {
                nextNs = due;
            }
        }

        // sleep 上限 1ms。manual mode の受信は PumpAll でしか進まないため、
        // sleep 粒度がそのまま farm の受信レイテンシ床になる。spin-pump は
        // farm proc が core を焼き、同居プロセス(local run では server)を
        // starve させて逆効果だったため不採用(farm 凍結構成も sleep 前提)
        if (now < nextNs)
        {
            var deltaNs = BenchClock.SaturatingSub(
                nextNs < schedule.DrainUntilNs ? nextNs : schedule.DrainUntilNs, now);
            if (deltaNs >= 1_000_000UL)
            {
                await Task.Delay(1, stopCts.Token).ConfigureAwait(false);
            }
            else
            {
                await Task.Yield();
            }
        }
    }

    if (!markedUnsent)
    {
        MarkUnsent(plans, originIds, schedule.StopAtNs, metrics);
    }

    // Final drain poll so echoes/broadcasts still in flight at drain_until
    // are captured before the metrics snapshot below.
    PumpAll(0f);

    var metricsPath = MetricsPathOrDefault();
    var metricsJson = metrics.ToJson();
    await File.WriteAllTextAsync(metricsPath, metricsJson + "\n", new UTF8Encoding(false), CancellationToken.None)
        .ConfigureAwait(false);

    var doneJson = metricsJson;
    if (progress is not null)
    {
        var snapshot = progress.ClientSnapshot();
        BenchProgress.WriteDiagnostics(snapshot, payloadValidation.Count);
        doneJson = BenchProgress.AttachToDoneStats(metricsJson, snapshot, payloadValidation.Count);
    }
    else if (config.Scenario is not null)
    {
        doneJson = BenchProgress.AttachValidationToDoneStats(metricsJson, payloadValidation.Count);
    }

    if (control is not null)
    {
        await control.DoneAsync(doneJson, CancellationToken.None).ConfigureAwait(false);
    }

    return 0;
}
catch (OperationCanceledException)
{
    return 130;
}
catch (Exception ex)
{
    Console.Error.WriteLine(ex);
    return 1;
}
finally
{
    if (control is not null)
    {
        await control.DisposeAsync().ConfigureAwait(false);
    }

    foreach (var manager in managers)
    {
        manager?.Stop();
    }
}

static async Task<int> RunRampAsync(
    ClientConfig config,
    BenchRampConfig ramp,
    NetManager[] managers,
    NetPeer[] peers,
    bool[] connected,
    BenchPlan?[] plans,
    uint[] originIds,
    ulong[] lastInputSeq,
    ulong[] lastAppliedInputSeq,
    BenchMetrics metrics,
    BenchPayloadValidation payloadValidation,
    BenchAuthoritativeProgressTracker? progress,
    BenchControl? control,
    CancellationToken cancellationToken)
{
    void PumpAll(float dtMs)
    {
        foreach (var manager in managers)
        {
            if (manager is null)
            {
                continue;
            }
            manager.PollEvents();
            manager.ManualUpdate(dtMs);
        }
    }

    int ActiveConns() => connected.Count(static value => value);

    async Task AddConnectionsAsync(int target, ulong deadlineNs, bool observeStop)
    {
        for (var i = 0; i < target; i++)
        {
            if (observeStop)
            {
                ramp.ThrowIfStopRequested();
            }
            if (BenchClock.NowNs() >= deadlineNs)
            {
                return;
            }
            if (managers[i] is not null)
            {
                continue;
            }
            var localIndex = i;
            var localIndexU32 = (uint)i;
            originIds[i] = config.OriginBase + localIndexU32;
            var listener = new EventBasedNetListener();
            listener.PeerConnectedEvent += _ => connected[localIndex] = true;
            listener.PeerDisconnectedEvent += (_, _) => connected[localIndex] = false;
            listener.NetworkReceiveEvent += (_, reader, _, _) =>
            {
                var recvTsNs = BenchClock.NowNs();
                var span = new ReadOnlySpan<byte>(reader.RawData, reader.UserDataOffset, reader.UserDataSize);
                if (BenchPayload.TryRead(span, out var header))
                {
                    if (config.Scenario?.Kind == BenchScenarioKind.AuthoritativeState)
                    {
                        var state = config.Scenario.ServerState!.Value;
                        if (header.OriginId != (uint)config.Scenario.TotalConns ||
                            header.TrafficId != state.TrafficId ||
                            (header.Flags & BenchConstants.FlagBroadcast) != 0 ||
                            BenchConstants.DirectionFromFlags(header.Flags) != BenchDirection.ServerToClient ||
                            !state.Accepts(header.Flags, span.Length) ||
                            !BenchPayload.TryReadAppliedInputSeq(span, out var appliedInputSeq) ||
                            appliedInputSeq > lastInputSeq[localIndex] ||
                            !BenchPayload.ValidateTargetPad(span, originIds[localIndex]))
                        {
                            payloadValidation.Invalid();
                            reader.Recycle();
                            return;
                        }
                        BenchProgress.UpdateMax(ref lastAppliedInputSeq[localIndex], appliedInputSeq);
                        progress?.RecordStateReceived(localIndex, header, appliedInputSeq);
                    }
                    else if (!BenchPayload.ValidateBody(span, header))
                    {
                        payloadValidation.Invalid();
                        reader.Recycle();
                        return;
                    }
                    metrics.OnRecv(localIndexU32, header, recvTsNs);
                }
                else
                {
                    payloadValidation.Invalid();
                }
                reader.Recycle();
            };

            var manager = new NetManager(listener)
            {
                MtuDiscovery = true,
                UseNativeSockets = true,
                DisconnectTimeout = 60000,
                PacketPoolSize = 4096,
            };
            if (!manager.StartInManualMode(0))
            {
                throw new InvalidOperationException($"NetManager.StartInManualMode failed for conn {i}");
            }
            SetReceiveBuffer(manager, 4 * 1024 * 1024);
            var connectData = new NetDataWriter();
            connectData.Put(LnlConstants.ConnectKey);
            connectData.Put(originIds[i]);
            var peer = manager.Connect(config.Host, config.Port, connectData) ??
                throw new InvalidOperationException($"NetManager.Connect returned null for conn {i}");
            managers[i] = manager;
            peers[i] = peer;
        }

        var lastPump = BenchClock.NowNs();
        while (ActiveConns() < target)
        {
            if (observeStop)
            {
                ramp.ThrowIfStopRequested();
            }
            if (BenchClock.NowNs() >= deadlineNs)
            {
                return;
            }
            cancellationToken.ThrowIfCancellationRequested();
            var now = BenchClock.NowNs();
            PumpAll((float)(BenchClock.SaturatingSub(now, lastPump) / 1_000_000.0));
            lastPump = now;
            await Task.Delay(1, cancellationToken).ConfigureAwait(false);
        }
    }

    await AddConnectionsAsync(ramp.StartConns, ulong.MaxValue, observeStop: false).ConfigureAwait(false);
    BenchSchedule schedule;
    if (control is not null)
    {
        await control.ReadyAsync(config.Conns, cancellationToken).ConfigureAwait(false);
        var scheduleTask = control.WaitScheduleAsync(cancellationToken);
        schedule = await LnlPump.PumpWhileAwaitingAsync(scheduleTask, PumpAll, 5, cancellationToken)
            .ConfigureAwait(false);
    }
    else
    {
        var now = BenchClock.NowNs();
        var duration = ramp.RequiredWindowNs(config.Conns);
        schedule = new BenchSchedule(now, BenchClock.AddSaturating(now, duration), BenchClock.AddSaturating(now, duration));
    }

    var streams = BuildStreams(config);
    for (var i = 0; i < ramp.StartConns; i++)
    {
        plans[i] = new BenchPlan(streams, schedule.StartAtNs, schedule.StartAtNs, schedule.StopAtNs);
    }
    var metricsPath = MetricsPathOrDefault();
    var phaseIndex = 0;
    try
    {
        foreach (var target in ramp.Targets(config.Conns))
        {
            var phaseStart = ramp.PhaseStartNs(schedule.StartAtNs, phaseIndex);
            var resetDeadline = BenchClock.AddSaturating(phaseStart, ramp.GuardNs);
            var sampleEnd = BenchClock.AddSaturating(resetDeadline, ramp.SampleNs);
            var phaseEnd = BenchClock.AddSaturating(sampleEnd, ramp.DrainNs);
            await DriveRampTrafficAsync(
                config, managers, peers, connected, plans, originIds, lastInputSeq, metrics,
                progress, payloadValidation, schedule, phaseStart, ramp, cancellationToken).ConfigureAwait(false);

            metrics.Reset();
            metrics.SetObservationWindow(resetDeadline, sampleEnd);
            config.Scenario?.RegisterExpectedLatestFlows(
                metrics, (uint)target, config.OriginBase, resetDeadline, (uint)target);

            await AddConnectionsAsync(target, resetDeadline, observeStop: true).ConfigureAwait(false);
            for (var i = 0; i < target; i++)
            {
                plans[i] ??= new BenchPlan(streams, BenchClock.NowNs(), schedule.StartAtNs, schedule.StopAtNs);
            }

            await DriveRampTrafficAsync(
                config, managers, peers, connected, plans, originIds, lastInputSeq, metrics,
                progress, payloadValidation, schedule, resetDeadline, ramp, cancellationToken).ConfigureAwait(false);

            await DriveRampTrafficAsync(
                config, managers, peers, connected, plans, originIds, lastInputSeq, metrics,
                progress, payloadValidation, schedule, phaseEnd, ramp, cancellationToken).ConfigureAwait(false);

            await File.WriteAllTextAsync(
                BenchRampConfig.SnapshotPath(metricsPath, phaseIndex, target),
                metrics.ToJson() + "\n",
                new UTF8Encoding(false),
                CancellationToken.None).ConfigureAwait(false);
            phaseIndex++;
            ramp.ThrowIfStopRequested();
        }
    }
    catch (BenchRampStopException)
    {
    }

    PumpAll(0f);
    // Ramp runs are scored from per-phase snapshots only; the cumulative
    // series spans multiple connection levels, so no plain metrics artifact
    // is written (matching the C endpoints).
    var metricsJson = metrics.ToJson();
    var doneJson = progress is null
        ? BenchProgress.AttachValidationToDoneStats(metricsJson, payloadValidation.Count)
        : BenchProgress.AttachToDoneStats(metricsJson, progress.ClientSnapshot(), payloadValidation.Count);
    if (control is not null)
    {
        await control.DoneAsync(doneJson, CancellationToken.None).ConfigureAwait(false);
    }
    return 0;
}

static async Task DriveRampTrafficAsync(
    ClientConfig config,
    NetManager[] managers,
    NetPeer[] peers,
    bool[] connected,
    BenchPlan?[] plans,
    uint[] originIds,
    ulong[] lastInputSeq,
    BenchMetrics metrics,
    BenchAuthoritativeProgressTracker? progress,
    BenchPayloadValidation payloadValidation,
    BenchSchedule schedule,
    ulong deadlineNs,
    BenchRampConfig ramp,
    CancellationToken cancellationToken)
{
    if (deadlineNs > schedule.StopAtNs)
    {
        throw new InvalidOperationException("ramp phases exceed the scheduled measurement window");
    }
    var lastPump = BenchClock.NowNs();
    while (BenchClock.NowNs() < deadlineNs)
    {
        ramp.ThrowIfStopRequested();
        cancellationToken.ThrowIfCancellationRequested();
        var now = BenchClock.NowNs();
        var sendThrough = now < deadlineNs ? now : deadlineNs - 1;
        metrics.Tick(now);
        for (var i = 0; i < plans.Length; i++)
        {
            if (!connected[i] || plans[i] is null)
            {
                continue;
            }
            while (plans[i]!.TryNext(sendThrough, out var slot))
            {
                SendSlot(
                    peers[i], originIds[i], i, slot, config, metrics,
                    ref lastInputSeq[i], progress, payloadValidation);
            }
        }
        var pumpNow = BenchClock.NowNs();
        foreach (var manager in managers)
        {
            if (manager is null)
            {
                continue;
            }
            manager.PollEvents();
            manager.ManualUpdate((float)(BenchClock.SaturatingSub(pumpNow, lastPump) / 1_000_000.0));
        }
        lastPump = pumpNow;

        var next = deadlineNs;
        foreach (var plan in plans)
        {
            if (plan is not null && plan.PeekNs() < next)
            {
                next = plan.PeekNs();
            }
        }
        now = BenchClock.NowNs();
        if (now < next && next - now >= 1_000_000UL)
        {
            await Task.Delay(1, cancellationToken).ConfigureAwait(false);
        }
        else
        {
            await Task.Yield();
        }
    }
}

static void SetReceiveBuffer(NetManager manager, int bytes)
{
    for (var t = manager.GetType(); t is not null; t = t.BaseType)
    {
        var field = t.GetField(
            "_udpSocketv4",
            System.Reflection.BindingFlags.Instance |
            System.Reflection.BindingFlags.NonPublic |
            System.Reflection.BindingFlags.DeclaredOnly);
        if (field?.GetValue(manager) is System.Net.Sockets.Socket socket)
        {
            socket.ReceiveBufferSize = bytes;
            return;
        }
    }

    Console.Error.WriteLine("warning: _udpSocketv4 not found; farm rcvbuf left at library default");
}

static IReadOnlyList<BenchStream> BuildStreams(ClientConfig config)
{
    if (config.Scenario is { Kind: BenchScenarioKind.AuthoritativeState, ClientInput: { } input })
    {
        return input.Streams(BenchDirection.ClientToServer);
    }
    if (config.Scenario is { Kind: BenchScenarioKind.EnvironmentBaseline, ClientInput: { } baseline })
    {
        return baseline.Streams(BenchDirection.RoomRelay);
    }
    if (config.Scenario?.RoomPublish is { } publish)
    {
        return publish.Streams(BenchDirection.RoomRelay, true);
    }

    var streams = new List<BenchStream>(2);
    if (config.RateLt > 0)
    {
        streams.Add(new BenchStream(false, config.BroadcastLt, IntervalFromRate(config.RateLt)));
    }
    if (config.RateMd > 0)
    {
        streams.Add(new BenchStream(true, config.BroadcastMd, IntervalFromRate(config.RateMd)));
    }

    return streams;
}

static ulong IntervalFromRate(double rateHz)
{
    var interval = 1_000_000_000.0 / rateHz;
    if (!double.IsFinite(interval) || interval < 1.0 || interval > ulong.MaxValue)
    {
        throw new ArgumentOutOfRangeException(nameof(rateHz), "invalid rate");
    }

    var ns = (ulong)(interval + 0.5);
    return ns == 0 ? 1 : ns;
}

// Sends are synchronous, non-blocking library calls (unlike magiconion's
// gRPC streaming call, which needs an async pipe to avoid RTT-bound
// coalescing) so there is no send queue and no app-level coalescing here —
// matches --describe's "coalescing":"none". A slot that cannot be sent right
// now (peer not connected, or the header fails to encode) is recorded as
// unsent rather than retried, per benchspec.
static void SendSlot(
    NetPeer peer,
    uint originId,
    int localIndex,
    BenchSlot slot,
    ClientConfig config,
    BenchMetrics metrics,
    ref ulong lastInputSeq,
    BenchAuthoritativeProgressTracker? progress,
    BenchPayloadValidation payloadValidation)
{
    var sendTsNs = BenchClock.NowNs();
    var header = new BenchHeader(slot.Seq, slot.SchedTsNs, sendTsNs, slot.Flags, originId, slot.TrafficId);
    var mustDeliver = (slot.Flags & BenchConstants.FlagMustDeliver) != 0;
    var payloadSize = mustDeliver ? config.PayloadMd : config.PayloadLt;
    var submitted = false;

    if (peer.ConnectionState == ConnectionState.Connected)
    {
        var method = mustDeliver ? DeliveryMethod.ReliableOrdered : DeliveryMethod.Unreliable;
        if (!mustDeliver &&
            BenchConstants.DirectionFromFlags(header.Flags) == BenchDirection.ClientToServer)
        {
            BenchProgress.UpdateMax(ref lastInputSeq, header.Seq);
        }
        try
        {
            // CreatePacketFromPool + 直書き = per-send の managed alloc 0・
            // copy 0(通常の Send は毎回 pool packet へ CopyTo する)。
            // header 32B 以降は受信側で読まれない契約なので未初期化のまま。
            // MTU(1 packet)に収まらない md はライブラリの fragment 経路
            // (通常 Send)へフォールバック。
            var pooled = peer.CreatePacketFromPool(method);
            if (payloadSize <= pooled.MaxUserDataSize)
            {
                var span = pooled.Data.AsSpan(pooled.UserDataOffset, payloadSize);
                if (!BenchPayload.TryWrite(span, header) || !BenchPayload.TryFillBody(span, header))
                {
                    payloadValidation.Invalid();
                    throw new InvalidOperationException("failed to encode benchmark payload");
                }
                peer.SendPooledPacket(pooled, payloadSize);
            }
            else
            {
                var payload = new byte[payloadSize];
                if (!BenchPayload.TryWrite(payload, header) || !BenchPayload.TryFillBody(payload, header))
                {
                    payloadValidation.Invalid();
                    throw new InvalidOperationException("failed to encode benchmark payload");
                }
                peer.Send(payload, method);
            }
            submitted = peer.ConnectionState == ConnectionState.Connected;
        }
        catch
        {
            submitted = false;
        }
    }

    metrics.OnSlot(header, submitted);
    progress?.RecordInputLastSent(localIndex, header, submitted);
}

static void MarkUnsent(BenchPlan?[] plans, uint[] originIds, ulong stopAtNs, BenchMetrics metrics)
{
    var cutoff = stopAtNs == 0 ? 0 : stopAtNs - 1;
    for (var i = 0; i < plans.Length; i++)
    {
        while (plans[i]!.TryNext(cutoff, out var slot))
        {
            var header = new BenchHeader(slot.Seq, slot.SchedTsNs, 0, slot.Flags, originIds[i], slot.TrafficId);
            metrics.OnSlot(header, false);
        }
    }
}

static ulong NextPlanDue(BenchPlan?[] plans)
{
    var next = ulong.MaxValue;
    foreach (var plan in plans)
    {
        var due = plan!.PeekNs();
        if (due < next)
        {
            next = due;
        }
    }

    return next;
}

static string MetricsPathOrDefault()
{
    var path = Environment.GetEnvironmentVariable("BENCH_METRICS_OUT");
    if (!string.IsNullOrWhiteSpace(path))
    {
        return path;
    }

    return "/tmp/rudp-bench-litenetlib-client-" +
           Environment.ProcessId.ToString(CultureInfo.InvariantCulture) + ".json";
}

internal readonly record struct ClientConfig(
    bool Valid,
    bool Describe,
    string Host,
    int Port,
    int Conns,
    int ProcIndex,
    uint OriginBase,
    double RateLt,
    double RateMd,
    bool BroadcastLt,
    bool BroadcastMd,
    int PayloadLt,
    int PayloadMd,
    ulong DeadlineNs,
    ulong StalenessPeriodNs,
    BenchScenarioConfig? Scenario = null)
{
    public const ulong DevWarmupNs = 200_000_000UL;
    public const ulong DevDurationNs = 2_000_000_000UL;
    public const ulong DevDrainNs = 500_000_000UL;

    // loss-tolerant rides DeliveryMethod.Unreliable, which LiteNetLib caps at
    // a single UDP datagram (no fragmentation) — see Shared/BenchDescribeLnl.cs.
    // must-deliver rides ReliableOrdered, which fragments, so the shared
    // BenchKit.CS.BenchConstants.MaxPayloadBytes ceiling (also used by
    // magiconion) applies there instead.
    private static readonly int LtMaxPayloadBytes = LiteNetLib.NetConstants.MaxUnreliableDataSize;

    public static ClientConfig Parse(string[] args)
    {
        if (!BenchScenarioConfig.TryParseArguments(args, out var scenario, out var remaining))
        {
            return Invalid();
        }
        args = remaining;
        var cfg = new MutableConfig();
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--describe")
            {
                return new ClientConfig(true, true, "", 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0);
            }
            if (args[i] == "--host" && i + 1 < args.Length)
            {
                cfg.Host = args[++i];
                cfg.HaveHost = cfg.Host.Length != 0;
                continue;
            }
            if (args[i] == "--port" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Port) &&
                cfg.Port is > 0 and <= ushort.MaxValue)
            {
                cfg.HavePort = true;
                continue;
            }
            if (args[i] == "--conns" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.Conns) &&
                cfg.Conns > 0)
            {
                cfg.HaveConns = true;
                continue;
            }
            if (args[i] == "--proc-index" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.ProcIndex) &&
                cfg.ProcIndex >= 0)
            {
                cfg.HaveProcIndex = true;
                continue;
            }
            if (args[i] == "--origin-base" && i + 1 < args.Length &&
                uint.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.OriginBase))
            {
                cfg.HaveOriginBase = true;
                continue;
            }
            if (args[i] == "--rate-lt" && i + 1 < args.Length &&
                double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateLt) &&
                double.IsFinite(cfg.RateLt) && cfg.RateLt >= 0)
            {
                cfg.HaveRateLt = true;
                continue;
            }
            if (args[i] == "--rate-md" && i + 1 < args.Length &&
                double.TryParse(args[++i], NumberStyles.Float, CultureInfo.InvariantCulture, out cfg.RateMd) &&
                double.IsFinite(cfg.RateMd) && cfg.RateMd >= 0)
            {
                cfg.HaveRateMd = true;
                continue;
            }
            if (args[i] == "--broadcast-lt")
            {
                cfg.BroadcastLt = true;
                continue;
            }
            if (args[i] == "--broadcast-md")
            {
                cfg.BroadcastMd = true;
                continue;
            }
            if (args[i] == "--payload" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out var payloadValue))
            {
                cfg.PayloadLt = payloadValue;
                cfg.PayloadMd = payloadValue;
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--payload-lt" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadLt))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--payload-md" && i + 1 < args.Length &&
                int.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.PayloadMd))
            {
                cfg.HavePayload = true;
                continue;
            }
            if (args[i] == "--deadline-ns" && i + 1 < args.Length &&
                ulong.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.DeadlineNs))
            {
                cfg.HaveDeadline = true;
                continue;
            }
            if (args[i] == "--staleness-period-ns" && i + 1 < args.Length &&
                ulong.TryParse(args[++i], NumberStyles.None, CultureInfo.InvariantCulture, out cfg.StalenessPeriodNs) &&
                cfg.StalenessPeriodNs > 0)
            {
                cfg.HaveStaleness = true;
                continue;
            }

            return Invalid();
        }

        if (scenario is { } activeScenario)
        {
            if (!cfg.BaseComplete)
            {
                return Invalid();
            }
            var activeTraffic = activeScenario.ClientInput ?? activeScenario.RoomPublish;
            if (activeTraffic is not { } source)
            {
                return Invalid();
            }
            cfg.RateLt = source.RateLt;
            cfg.RateMd = source.RateMd;
            cfg.PayloadLt = source.PayloadLt;
            cfg.PayloadMd = source.PayloadMd;
            cfg.BroadcastLt = activeScenario.Kind == BenchScenarioKind.RoomRelay;
            cfg.BroadcastMd = activeScenario.Kind == BenchScenarioKind.RoomRelay;
            if (!cfg.HaveDeadline)
            {
                cfg.DeadlineNs = source.DeadlineNs;
            }
            if (!cfg.HaveStaleness)
            {
                cfg.StalenessPeriodNs = BenchConstants.DefaultStalenessPeriodNs;
            }
        }
        else if (!cfg.Complete || (cfg.RateLt == 0 && cfg.RateMd == 0))
        {
            return Invalid();
        }
        // Loss-tolerant payload must fit LiteNetLib's real Unreliable limit
        // (a single UDP datagram — LiteNetLib throws TooBigPacketException
        // above this, it does not fragment unreliable sends).
        if (cfg.RateLt > 0 &&
            (cfg.PayloadLt < BenchConstants.MinPayloadBytes || cfg.PayloadLt > LtMaxPayloadBytes))
        {
            return Invalid();
        }
        if (cfg.RateMd > 0 &&
            (cfg.PayloadMd < BenchConstants.MinPayloadBytes || cfg.PayloadMd > BenchConstants.MaxPayloadBytes))
        {
            return Invalid();
        }
        var endOrigin = (ulong)cfg.OriginBase + (ulong)cfg.Conns;
        if (endOrigin > uint.MaxValue ||
            (scenario is not null && endOrigin > (ulong)scenario.TotalConns))
        {
            return Invalid();
        }

        return new ClientConfig(
            true,
            false,
            cfg.Host,
            cfg.Port,
            cfg.Conns,
            cfg.ProcIndex,
            cfg.OriginBase,
            cfg.RateLt,
            cfg.RateMd,
            cfg.BroadcastLt,
            cfg.BroadcastMd,
            cfg.PayloadLt,
            cfg.PayloadMd,
            cfg.DeadlineNs,
            cfg.StalenessPeriodNs,
            scenario);
    }

    private static ClientConfig Invalid() => new(false, false, "", 0, 0, 0, 0, 0, 0, false, false, 0, 0, 0, 0);

    private sealed class MutableConfig
    {
        public string Host = "";
        public int Port;
        public int Conns;
        public int ProcIndex;
        public uint OriginBase;
        public double RateLt;
        public double RateMd;
        public bool BroadcastLt;
        public bool BroadcastMd;
        public int PayloadLt;
        public int PayloadMd;
        public ulong DeadlineNs;
        public ulong StalenessPeriodNs;
        public bool HaveHost;
        public bool HavePort;
        public bool HaveConns;
        public bool HaveProcIndex;
        public bool HaveOriginBase;
        public bool HaveRateLt;
        public bool HaveRateMd;
        public bool HavePayload;
        public bool HaveDeadline;
        public bool HaveStaleness;

        public bool Complete =>
            BaseComplete &&
            HaveRateLt && HaveRateMd && HavePayload && HaveDeadline && HaveStaleness;

        public bool BaseComplete => HaveHost && HavePort && HaveConns && HaveProcIndex && HaveOriginBase;
    }
}
