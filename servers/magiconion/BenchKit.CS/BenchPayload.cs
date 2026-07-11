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

        for (var i = BenchConstants.HeaderSize; i < buffer.Length; i++)
        {
            buffer[i] = BodyByte(header, i - BenchConstants.HeaderSize);
        }
        return true;
    }

    public static bool ValidateBody(ReadOnlySpan<byte> buffer, BenchHeader header)
    {
        if (buffer.Length < BenchConstants.HeaderSize)
        {
            return false;
        }

        for (var i = BenchConstants.HeaderSize; i < buffer.Length; i++)
        {
            if (buffer[i] != BodyByte(header, i - BenchConstants.HeaderSize))
            {
                return false;
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

    private static byte BodyByte(BenchHeader header, int bodyIndex)
    {
        var i = (uint)bodyIndex;
        return (byte)(
            (byte)(header.Seq >> ((bodyIndex & 7) * 8)) ^
            (byte)(header.SchedTsNs >> (((bodyIndex + 3) & 7) * 8)) ^
            (byte)(header.SendTsNs >> (((bodyIndex + 5) & 7) * 8)) ^
            (byte)(header.OriginId >> ((bodyIndex & 3) * 8)) ^
            header.Flags ^
            header.TrafficId ^
            (byte)i);
    }
}
