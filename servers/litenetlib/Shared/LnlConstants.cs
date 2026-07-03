namespace LiteNetLibBench;

/// <summary>
/// Values shared between the LiteNetLibBench.Server and LiteNetLibBench.Client
/// binaries. Kept here (rather than duplicated in each Program.cs) since both
/// projects compile this file directly (see &lt;Compile Include="../Shared/*.cs" /&gt;
/// in each .csproj) — servers/magiconion/BenchKit.CS is referenced read-only via
/// ProjectReference and must not be edited to hold transport-specific values.
/// </summary>
public static class LnlConstants
{
    /// <summary>
    /// LiteNetLib connection handshake key. Purely a protocol handshake value
    /// (ConnectionRequest.AcceptIfKey / NetManager.Connect), not a performance
    /// tuning knob, so it is not listed in --describe's "tuning" array.
    /// </summary>
    public const string ConnectKey = "rudpbench";
}
