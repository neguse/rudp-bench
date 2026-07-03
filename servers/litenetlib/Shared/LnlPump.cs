using BenchKit.CS;

namespace LiteNetLibBench;

/// <summary>
/// LiteNetLib only moves data when the app calls PollEvents() (and, in manual
/// mode, ManualUpdate()) — there is no OS-callback push. Both the server and
/// client must keep pumping while they `await` the control-channel schedule,
/// otherwise: (a) the server's PeerConnected/NetworkReceive events never fire
/// so no client can finish its handshake before start_at (the "deadlock
/// landmine" called out in the task brief — docs/checklist.md §6, §10.1), and
/// (b) in the client's per-conn manual-mode managers, nothing sends the
/// periodic ping/ack traffic that keeps a peer under DisconnectTimeout, so a
/// long schedule wait could silently disconnect peers before the run starts.
/// This helper interleaves pumping with an arbitrary awaited Task so neither
/// binary ever blocks its poll loop.
/// </summary>
public static class LnlPump
{
    public static async Task<T> PumpWhileAwaitingAsync<T>(
        Task<T> task,
        Action<float> pump,
        int intervalMs,
        CancellationToken cancellationToken)
    {
        var lastNs = BenchClock.NowNs();
        while (!task.IsCompleted)
        {
            var nowNs = BenchClock.NowNs();
            var elapsedMs = (float)(BenchClock.SaturatingSub(nowNs, lastNs) / 1_000_000.0);
            lastNs = nowNs;
            pump(elapsedMs);

            var delay = Task.Delay(intervalMs, cancellationToken);
            await Task.WhenAny(task, delay).ConfigureAwait(false);
        }

        return await task.ConfigureAwait(false);
    }

    /// <summary>
    /// Keeps pumping until drainUntilNs, used by the server after it has the
    /// schedule (no async result to await here, just a deadline).
    /// </summary>
    public static async Task DrainUntilAsync(
        ulong drainUntilNs,
        Action<float> pump,
        CancellationToken cancellationToken)
    {
        var lastNs = BenchClock.NowNs();
        while (true)
        {
            var nowNs = BenchClock.NowNs();
            var elapsedMs = (float)(BenchClock.SaturatingSub(nowNs, lastNs) / 1_000_000.0);
            lastNs = nowNs;
            pump(elapsedMs);

            nowNs = BenchClock.NowNs();
            if (nowNs >= drainUntilNs || cancellationToken.IsCancellationRequested)
            {
                break;
            }

            var remainNs = drainUntilNs - nowNs;
            var delayMs = (int)Math.Clamp(remainNs / 1_000_000UL, 1UL, 15UL);
            try
            {
                await Task.Delay(delayMs, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }

        pump(0f);
    }
}
