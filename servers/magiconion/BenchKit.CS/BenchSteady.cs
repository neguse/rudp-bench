namespace BenchKit.CS;

// 定常判定つき warmup(benchspec v2)の client ループヘルパ。
// rate の周期送信(250ms)と window の受信を1呼び出しにまとめる。
// sent/received は呼び出し側が自分の同期規約で読んだ累積生カウント
// (BenchMetrics.RawCounts の Submitted と RecvMeasured+RecvUnmeasured)。
// window 受信時は確定窓を返すので、呼び出し側は schedule を差し替えたうえで
// 自分の全 plan に SetWindow を適用すること。
// 窓確定後(window 受信 or 暫定 start_at 到達)は何もしない。
public sealed class BenchSteady
{
    // rate 報告の周期(benchspec v2 で固定)
    private const ulong RateIntervalNs = 250_000_000UL;

    private ulong nextRateNs;
    private bool windowFinal;

    public async ValueTask<BenchSchedule?> TickAsync(
        BenchControl control,
        ulong sent,
        ulong received,
        BenchSchedule schedule,
        ulong nowNs,
        CancellationToken cancellationToken)
    {
        if (windowFinal)
        {
            return null;
        }
        // 窓が開いたら以降 rate 報告も window 待ちも不要(暫定窓のまま計測)
        if (nowNs >= schedule.StartAtNs)
        {
            windowFinal = true;
            return null;
        }

        if (nextRateNs == 0)
        {
            nextRateNs = BenchClock.AddSaturating(nowNs, RateIntervalNs);
        }
        if (nowNs >= nextRateNs)
        {
            await control.SendRateAsync(sent, received, cancellationToken).ConfigureAwait(false);
            do
            {
                nextRateNs = BenchClock.AddSaturating(nextRateNs, RateIntervalNs);
            }
            while (nextRateNs <= nowNs);
        }

        var window = await control.PollWindowAsync(cancellationToken).ConfigureAwait(false);
        if (window is not null)
        {
            windowFinal = true;
        }

        return window;
    }
}
