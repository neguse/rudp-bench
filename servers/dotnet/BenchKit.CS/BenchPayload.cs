using System.Buffers.Binary;

namespace BenchKit.CS;

public readonly record struct BenchHeader(
    ulong Seq,
    ulong SchedTsNs,
    ulong SendTsNs,
    byte Flags,
    uint OriginId,
    byte TrafficId = 0);

public static class BenchPayload
{
    public const int AuthoritativeStateMinPayloadBytes = BenchConstants.HeaderSize + sizeof(ulong);

    public static bool TryWrite(Span<byte> buffer, BenchHeader header)
    {
        if (buffer.Length < BenchConstants.HeaderSize ||
            (header.Flags & BenchConstants.ReservedFlagsMask) != 0 ||
            BenchConstants.DirectionFromFlags(header.Flags) > BenchDirection.ServerToClient)
        {
            return false;
        }

        BinaryPrimitives.WriteUInt64LittleEndian(buffer[0..8], header.Seq);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer[8..16], header.SchedTsNs);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer[16..24], header.SendTsNs);
        buffer[24] = header.Flags;
        BinaryPrimitives.WriteUInt32LittleEndian(buffer[25..29], header.OriginId);
        buffer[29] = header.TrafficId;
        buffer[30] = 0;
        buffer[31] = 0;
        return true;
    }

    public static bool TryRead(ReadOnlySpan<byte> buffer, out BenchHeader header)
    {
        header = default;
        if (buffer.Length < BenchConstants.HeaderSize)
        {
            return false;
        }

        var flags = buffer[24];
        if ((flags & BenchConstants.ReservedFlagsMask) != 0 ||
            BenchConstants.DirectionFromFlags(flags) > BenchDirection.ServerToClient ||
            buffer[30] != 0 || buffer[31] != 0)
        {
            return false;
        }

        header = new BenchHeader(
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[0..8]),
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[8..16]),
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[16..24]),
            flags,
            BinaryPrimitives.ReadUInt32LittleEndian(buffer[25..29]),
            buffer[29]);
        return true;
    }

    public static bool TryFillBody(Span<byte> buffer, BenchHeader header)
    {
        if (buffer.Length < BenchConstants.HeaderSize)
        {
            return false;
        }

        var key = BodyPatternKey(header);
        var bodyIndex = 0;
        while (BenchConstants.HeaderSize + bodyIndex < buffer.Length)
        {
            var block = BodyPatternBlock(key, (ulong)(bodyIndex / sizeof(ulong)));
            for (var offset = 0;
                 offset < sizeof(ulong) && BenchConstants.HeaderSize + bodyIndex < buffer.Length;
                 offset++, bodyIndex++)
            {
                buffer[BenchConstants.HeaderSize + bodyIndex] = (byte)(block >> (offset * 8));
            }
        }
        return true;
    }

    public static bool ValidateBody(ReadOnlySpan<byte> buffer, BenchHeader header)
    {
        if (buffer.Length < BenchConstants.HeaderSize)
        {
            return false;
        }

        var key = BodyPatternKey(header);
        var bodyIndex = 0;
        while (BenchConstants.HeaderSize + bodyIndex < buffer.Length)
        {
            var block = BodyPatternBlock(key, (ulong)(bodyIndex / sizeof(ulong)));
            for (var offset = 0;
                 offset < sizeof(ulong) && BenchConstants.HeaderSize + bodyIndex < buffer.Length;
                 offset++, bodyIndex++)
            {
                if (buffer[BenchConstants.HeaderSize + bodyIndex] != (byte)(block >> (offset * 8)))
                {
                    return false;
                }
            }
        }
        return true;
    }

    public static bool TryWriteAppliedInputSeq(Span<byte> buffer, ulong appliedInputSeq)
    {
        if (buffer.Length < AuthoritativeStateMinPayloadBytes)
        {
            return false;
        }

        BinaryPrimitives.WriteUInt64LittleEndian(buffer[BenchConstants.HeaderSize..], appliedInputSeq);
        return true;
    }

    public static bool TryReadAppliedInputSeq(ReadOnlySpan<byte> buffer, out ulong appliedInputSeq)
    {
        appliedInputSeq = 0;
        if (buffer.Length < AuthoritativeStateMinPayloadBytes)
        {
            return false;
        }

        appliedInputSeq = BinaryPrimitives.ReadUInt64LittleEndian(buffer[BenchConstants.HeaderSize..]);
        return true;
    }

    public static bool TryFillTargetPad(Span<byte> buffer, uint targetId)
    {
        if (buffer.Length < AuthoritativeStateMinPayloadBytes)
        {
            return false;
        }

        for (var i = AuthoritativeStateMinPayloadBytes; i < buffer.Length; i++)
        {
            var padIndex = i - AuthoritativeStateMinPayloadBytes;
            var targetByte = (byte)(targetId >> ((padIndex % sizeof(uint)) * 8));
            buffer[i] = (byte)(targetByte ^ (byte)(padIndex / sizeof(uint)));
        }
        return true;
    }

    public static bool ValidateTargetPad(ReadOnlySpan<byte> buffer, uint targetId)
    {
        if (buffer.Length < AuthoritativeStateMinPayloadBytes)
        {
            return false;
        }

        for (var i = AuthoritativeStateMinPayloadBytes; i < buffer.Length; i++)
        {
            var padIndex = i - AuthoritativeStateMinPayloadBytes;
            var targetByte = (byte)(targetId >> ((padIndex % sizeof(uint)) * 8));
            if (buffer[i] != (byte)(targetByte ^ (byte)(padIndex / sizeof(uint))))
            {
                return false;
            }
        }
        return true;
    }

    private static ulong SplitMix64(ulong value)
    {
        unchecked
        {
            value += 0x9e3779b97f4a7c15UL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9UL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebUL;
            return value ^ (value >> 31);
        }
    }

    private static ulong BodyPatternKey(BenchHeader header)
    {
        // prng-v1 folds the complete 32-byte wire header as four LE words.
        var tail = (ulong)header.Flags | ((ulong)header.OriginId << 8) |
                   ((ulong)header.TrafficId << 40);
        var key = 0x727564702d707231UL; // "rudp-pr1"
        key = SplitMix64(key ^ header.Seq);
        key = SplitMix64(key ^ header.SchedTsNs);
        key = SplitMix64(key ^ header.SendTsNs);
        return SplitMix64(key ^ tail);
    }

    private static ulong BodyPatternBlock(ulong key, ulong blockIndex)
    {
        unchecked
        {
            return SplitMix64(key + blockIndex * 0x9e3779b97f4a7c15UL);
        }
    }
}
