using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace BenchKit.CS;

public readonly record struct BenchSchedule(ulong StartAtNs, ulong StopAtNs, ulong DrainUntilNs);

public sealed class BenchControl : IAsyncDisposable
{
    private readonly Socket socket;
    private readonly NetworkStream stream;
    private readonly StreamReader reader;
    private readonly StreamWriter writer;

    private BenchControl(Socket socket)
    {
        this.socket = socket;
        stream = new NetworkStream(socket, ownsSocket: true);
        reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
        writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true)
        {
            NewLine = "\n",
            AutoFlush = true
        };
    }

    public static async ValueTask<BenchControl?> ConnectFromEnvironmentAsync(CancellationToken cancellationToken)
    {
        var path = Environment.GetEnvironmentVariable("BENCH_CONTROL_SOCK");
        if (string.IsNullOrWhiteSpace(path) || path.StartsWith("fd:", StringComparison.Ordinal))
        {
            return null;
        }

        try
        {
            var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            await socket.ConnectAsync(new UnixDomainSocketEndPoint(path), cancellationToken).ConfigureAwait(false);
            return new BenchControl(socket);
        }
        catch
        {
            return null;
        }
    }

    public Task HelloAsync(string role, string transport, int procIndex, CancellationToken cancellationToken)
    {
        var line = JsonSerializer.Serialize(new
        {
            type = "hello",
            role,
            transport,
            pid = Environment.ProcessId,
            proc_index = procIndex
        });
        return WriteLineAsync(line, cancellationToken);
    }

    public Task ReadyAsync(int conns, CancellationToken cancellationToken)
    {
        var line = JsonSerializer.Serialize(new { type = "ready", conns });
        return WriteLineAsync(line, cancellationToken);
    }

    public async Task<BenchSchedule> WaitScheduleAsync(CancellationToken cancellationToken)
    {
        var line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
        if (line is null)
        {
            throw new IOException("control socket closed before schedule");
        }

        var recvNs = BenchClock.NowNs();
        using var doc = JsonDocument.Parse(line);
        var root = doc.RootElement;
        if (!root.TryGetProperty("type", out var type) || type.GetString() != "schedule")
        {
            throw new InvalidDataException("unexpected control message while waiting for schedule");
        }

        var schedule = new BenchSchedule(
            root.GetProperty("start_at_ns").GetUInt64(),
            root.GetProperty("stop_at_ns").GetUInt64(),
            root.GetProperty("drain_until_ns").GetUInt64());
        var marginNs = BenchClock.DiffSaturatingI64(schedule.StartAtNs, recvNs);
        await WriteLineAsync(
            JsonSerializer.Serialize(new { type = "sched_ack", margin_ns = marginNs }),
            cancellationToken).ConfigureAwait(false);
        return schedule;
    }

    public Task DoneAsync(string statsJson, CancellationToken cancellationToken) =>
        WriteLineAsync("{\"type\":\"done\",\"stats\":" + (string.IsNullOrWhiteSpace(statsJson) ? "{}" : statsJson) + "}", cancellationToken);

    public async ValueTask DisposeAsync()
    {
        await writer.DisposeAsync().ConfigureAwait(false);
        reader.Dispose();
        stream.Dispose();
        socket.Dispose();
    }

    private async Task WriteLineAsync(string line, CancellationToken cancellationToken)
    {
        await writer.WriteLineAsync(line.AsMemory(), cancellationToken).ConfigureAwait(false);
        await writer.FlushAsync(cancellationToken).ConfigureAwait(false);
    }
}
