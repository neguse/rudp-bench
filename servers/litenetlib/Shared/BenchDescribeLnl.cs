using System.Globalization;

namespace LiteNetLibBench;

/// <summary>
/// --describe payload for both LiteNetLibBench binaries (benchspec/README.md
/// "開示 metadata"). Mirrors servers/magiconion/BenchKit.CS/BenchDescribe.cs in
/// shape, but lives here (not in BenchKit.CS, which is magiconion-owned and
/// read-only for this transport) and is shared between Server/Client via a
/// linked source file rather than duplicated per binary.
///
/// max_payload_bytes reports LiteNetLib.NetConstants.MaxUnreliableDataSize —
/// the *binding* constraint across both traffic classes. loss-tolerant maps to
/// DeliveryMethod.Unreliable, which LiteNetLib sends as a single UDP datagram
/// with no fragmentation (TooBigPacketException above this size — verified by
/// reflecting LiteNetLib 2.1.4's NetConstants/LiteNetPeer). must-deliver maps
/// to DeliveryMethod.ReliableOrdered, which fragments and supports much larger
/// messages; reporting the smaller, real hard limit here is the conservative
/// (non-misleading) choice for a single shared field.
/// </summary>
public static class LnlDescribe
{
    public static readonly string Json = BuildJson();

    private static string BuildJson()
    {
        var maxUnreliable = LiteNetLib.NetConstants.MaxUnreliableDataSize
            .ToString(CultureInfo.InvariantCulture);

        return "{\"transport\":\"litenetlib\"," +
            "\"class_mapping\":{\"loss_tolerant\":\"unreliable\",\"must_deliver\":\"reliable-ordered\"}," +
            "\"coalescing\":\"none\"," +
            "\"cc_algo\":\"none\"," +
            "\"thread_model\":\"server: internal_worker (NetManager.Start spawns a receive thread + " +
            "logic thread internally); client: single (StartInManualMode per conn - no internal threads, " +
            "all conns pumped from one loop via PollEvents+ManualUpdate)\"," +
            "\"encryption\":false," +
            "\"max_payload_bytes\":" + maxUnreliable + "," +
            "\"tuning\":[\"use_native_sockets\",\"mtu_discovery\"," +
            "\"disconnect_timeout=60s\",\"unsynced_receive_event(server)\"," +
            "\"update_time=1ms+trigger_update(server)\"," +
            "\"pooled-packet-direct-write(client)\"]}";
    }
}
