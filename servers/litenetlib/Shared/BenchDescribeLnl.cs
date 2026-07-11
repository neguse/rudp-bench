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
            "\"scenarios\":[\"environment_baseline\",\"authoritative_state\",\"room_relay\"]," +
            "\"tuning\":[" +
            "{\"knob\":\"NetManager.UseNativeSockets\",\"value\":\"true\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetManager.MtuDiscovery\",\"value\":\"true\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetManager.DisconnectTimeout\",\"value\":\"60000 ms\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetManager.UnsyncedEvents\",\"value\":\"true (server)\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetManager.UpdateTime\",\"value\":\"1 ms; TriggerUpdate after enqueue (server)\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetManager.PacketPoolSize\",\"value\":\"16384 (server), 4096 (client)\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib/wiki/NetManager-and-NetPeer\"}," +
            "{\"knob\":\"NetPeer.CreatePacketFromPool/SendPooledPacket\",\"value\":\"direct pooled-packet write (client)\",\"upstream_ref\":\"https://github.com/RevenantX/LiteNetLib\"}" +
            "]}";
    }
}
