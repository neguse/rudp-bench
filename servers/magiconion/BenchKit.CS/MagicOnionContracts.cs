using MagicOnion;

namespace BenchKit.CS;

public interface IBenchHub : IStreamingHub<IBenchHub, IBenchHubReceiver>
{
    ValueTask JoinAsync();
    ValueTask SendPayloadAsync(byte[] payload);
}

public interface IBenchHubReceiver
{
    void OnPayload(byte[] payload);
}
