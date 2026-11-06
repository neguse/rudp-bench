namespace BenchKit.CS;

public static class BenchConstants
{
    public const int HeaderSize = 32;
    public const int MinPayloadBytes = HeaderSize;
    public const int MaxPayloadBytes = 65_536;

    public const byte FlagMustDeliver = 1 << 0;
    public const byte FlagMeasure = 1 << 1;
    public const byte FlagBroadcast = 1 << 2;
    public const int DirectionShift = 3;
    public const byte DirectionMask = 0x18;
    public const byte ReservedFlagsMask = 0xe0;

    public const int ClassLossTolerant = 0;
    public const int ClassMustDeliver = 1;
    public const int ClassCount = 2;

    public const ulong DefaultStalenessPeriodNs = 10_000_000UL;
    public const ulong HistMinNs = 1_000UL;
    public const ulong HistMaxNs = 100_000_000_000UL;
    public const int HistSubBins = 16;
    public const int HistMajorBins = 28;
    public const int HistBins = HistSubBins * HistMajorBins;

    public static int ClassIndexFromFlags(byte flags) =>
        (flags & FlagMustDeliver) != 0 ? ClassMustDeliver : ClassLossTolerant;

    public static BenchDirection DirectionFromFlags(byte flags) =>
        (BenchDirection)((flags & DirectionMask) >> DirectionShift);

    public static byte DirectionFlags(BenchDirection direction) =>
        (byte)((byte)direction << DirectionShift);
}

public enum BenchDirection : byte
{
    RoomRelay = 0,
    ClientToServer = 1,
    ServerToClient = 2,
}
