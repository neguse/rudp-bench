using System.Buffers.Binary;

namespace BenchKit.CS;

public readonly record struct BenchHeader(
    ulong Seq,
    ulong SchedTsNs,
    ulong SendTsNs,
    byte Flags,
    uint OriginId);

public static class BenchPayload
{
    public static bool TryWrite(Span<byte> buffer, BenchHeader header)
    {
        if (buffer.Length < BenchConstants.HeaderSize)
        {
            return false;
        }

        BinaryPrimitives.WriteUInt64LittleEndian(buffer[0..8], header.Seq);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer[8..16], header.SchedTsNs);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer[16..24], header.SendTsNs);
        buffer[24] = header.Flags;
        BinaryPrimitives.WriteUInt32LittleEndian(buffer[25..29], header.OriginId);
        buffer[29] = 0;
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
        if ((flags & 0xf8) != 0 || buffer[29] != 0 || buffer[30] != 0 || buffer[31] != 0)
        {
            return false;
        }

        header = new BenchHeader(
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[0..8]),
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[8..16]),
            BinaryPrimitives.ReadUInt64LittleEndian(buffer[16..24]),
            flags,
            BinaryPrimitives.ReadUInt32LittleEndian(buffer[25..29]));
        return true;
    }
}
