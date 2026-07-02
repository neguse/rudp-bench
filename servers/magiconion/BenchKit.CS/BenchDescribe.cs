namespace BenchKit.CS;

public static class BenchDescribe
{
    public const string Json =
        "{\"transport\":\"magiconion\"," +
        "\"class_mapping\":{\"loss_tolerant\":\"grpc-stream(coalesced)\",\"must_deliver\":\"grpc-stream\"}," +
        "\"coalescing\":\"latest-value\"," +
        "\"cc_algo\":\"kernel-tcp(cubic)\"," +
        "\"thread_model\":\"internal_worker\"," +
        "\"encryption\":false," +
        "\"max_payload_bytes\":65536," +
        "\"tuning\":[" +
        "{\"knob\":\"Kestrel.ListenAnyIP.protocols\",\"value\":\"HttpProtocols.Http2 (h2c)\",\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/grpc/aspnetcore#kestrel\"}," +
        "{\"knob\":\"GrpcChannelOptions.HttpHandler\",\"value\":\"one SocketsHttpHandler per benchmark connection; EnableMultipleHttp2Connections=true\",\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/grpc/performance#connection-concurrency\"}," +
        "{\"knob\":\"GrpcChannelOptions.MaxReceiveMessageSize/MaxSendMessageSize\",\"value\":\"65536\",\"upstream_ref\":\"https://learn.microsoft.com/aspnet/core/grpc/configuration\"}" +
        "]}";
}
