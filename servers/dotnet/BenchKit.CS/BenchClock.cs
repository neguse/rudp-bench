using System.Runtime.InteropServices;

namespace BenchKit.CS;

public static class BenchClock
{
    private const int ClockMonotonic = 1;

    [StructLayout(LayoutKind.Sequential)]
    private struct Timespec
    {
        public long tv_sec;
        public long tv_nsec;
    }

    [DllImport("libc", EntryPoint = "clock_gettime", SetLastError = true)]
    private static extern int ClockGetTime(int clkId, out Timespec ts);

    public static ulong NowNs()
    {
        if (ClockGetTime(ClockMonotonic, out var ts) != 0)
        {
            return 0;
        }

        return checked((ulong)ts.tv_sec * 1_000_000_000UL + (ulong)ts.tv_nsec);
    }

    public static ulong AddSaturating(ulong a, ulong b) =>
        ulong.MaxValue - a < b ? ulong.MaxValue : a + b;

    public static ulong SaturatingSub(ulong a, ulong b) => a >= b ? a - b : 0;

    public static long DiffSaturatingI64(ulong a, ulong b)
    {
        if (a >= b)
        {
            var d = a - b;
            return d > long.MaxValue ? long.MaxValue : (long)d;
        }

        var neg = b - a;
        return neg > (ulong)long.MaxValue ? long.MinValue : -(long)neg;
    }
}
