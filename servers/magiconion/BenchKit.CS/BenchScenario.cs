using System.Globalization;

namespace BenchKit.CS;

public enum BenchScenarioKind
{
    Legacy,
    EnvironmentBaseline,
    AuthoritativeState,
    RoomRelay,
}

public readonly record struct BenchTrafficConfig(
    byte TrafficId,
    double RateLt,
    double RateMd,
    int PayloadLt,
    int PayloadMd,
    ulong DeadlineNs)
{
    public IReadOnlyList<BenchStream> Streams(BenchDirection direction, bool broadcast = false)
    {
        var streams = new List<BenchStream>(2);
        if (RateLt > 0)
        {
            streams.Add(new BenchStream(false, broadcast, IntervalFromRate(RateLt), TrafficId, direction));
        }
        if (RateMd > 0)
        {
            streams.Add(new BenchStream(true, broadcast, IntervalFromRate(RateMd), TrafficId, direction));
        }
        return streams;
    }

    public int PayloadSize(bool mustDeliver) => mustDeliver ? PayloadMd : PayloadLt;

    public bool Accepts(byte flags, int payloadBytes)
    {
        var mustDeliver = (flags & BenchConstants.FlagMustDeliver) != 0;
        var rate = mustDeliver ? RateMd : RateLt;
        return rate > 0 && payloadBytes == PayloadSize(mustDeliver);
    }

    private static ulong IntervalFromRate(double rateHz)
    {
        var interval = 1_000_000_000.0 / rateHz;
        if (!double.IsFinite(interval) || interval < 1.0 || interval > ulong.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(rateHz), "invalid rate");
        }
        return Math.Max(1UL, (ulong)(interval + 0.5));
    }
}

public sealed record BenchScenarioConfig(
    BenchScenarioKind Kind,
    int TotalConns,
    BenchTrafficConfig? ClientInput,
    BenchTrafficConfig? ServerState,
    BenchTrafficConfig? RoomPublish)
{
    public void ConfigureMetrics(BenchMetrics metrics)
    {
        if (ClientInput is { } input)
        {
            ConfigureTraffic(
                metrics,
                input,
                Kind == BenchScenarioKind.AuthoritativeState
                    ? BenchDirection.ClientToServer
                    : BenchDirection.RoomRelay);
        }
        if (ServerState is { } state)
        {
            ConfigureTraffic(metrics, state, BenchDirection.ServerToClient);
        }
        if (RoomPublish is { } publish)
        {
            ConfigureTraffic(metrics, publish, BenchDirection.RoomRelay);
        }
    }

    public void RegisterExpectedLatestFlows(
        BenchMetrics metrics,
        uint localConns,
        uint originBase,
        ulong firstSchedTsNs)
    {
        switch (Kind)
        {
            case BenchScenarioKind.EnvironmentBaseline when ClientInput is { RateLt: > 0 } baseline:
                for (uint localIndex = 0; localIndex < localConns; localIndex++)
                {
                    ExpectLatest(
                        metrics,
                        localIndex,
                        checked(originBase + localIndex),
                        baseline.TrafficId,
                        BenchDirection.RoomRelay,
                        firstSchedTsNs);
                }
                break;

            case BenchScenarioKind.AuthoritativeState when ServerState is { RateLt: > 0 } state:
                for (uint localIndex = 0; localIndex < localConns; localIndex++)
                {
                    ExpectLatest(
                        metrics,
                        localIndex,
                        (uint)TotalConns,
                        state.TrafficId,
                        BenchDirection.ServerToClient,
                        firstSchedTsNs);
                }
                break;

            case BenchScenarioKind.RoomRelay when RoomPublish is { RateLt: > 0 } publish:
                for (uint localIndex = 0; localIndex < localConns; localIndex++)
                {
                    for (uint originId = 0; originId < (uint)TotalConns; originId++)
                    {
                        ExpectLatest(
                            metrics,
                            localIndex,
                            originId,
                            publish.TrafficId,
                            BenchDirection.RoomRelay,
                            firstSchedTsNs);
                    }
                }
                break;
        }
    }

    public static bool TryParseArguments(
        string[] args,
        out BenchScenarioConfig? scenario,
        out string[] remaining)
    {
        scenario = null;
        var rest = new List<string>();
        string? kindText = null;
        var totalConns = 0;
        var input = new MutableTraffic();
        var state = new MutableTraffic();
        var publish = new MutableTraffic();

        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (arg == "--scenario" && TryTake(args, ref i, out kindText))
            {
                continue;
            }
            if (arg == "--total-conns" && TryTakeInt(args, ref i, out totalConns) && totalConns > 0)
            {
                continue;
            }
            if (TryTrafficArg(arg, args, ref i, "input", input) ||
                TryTrafficArg(arg, args, ref i, "state", state) ||
                TryTrafficArg(arg, args, ref i, "publish", publish))
            {
                continue;
            }
            if (arg.StartsWith("--scenario", StringComparison.Ordinal) ||
                arg.StartsWith("--total-conns", StringComparison.Ordinal) ||
                arg.StartsWith("--input-", StringComparison.Ordinal) ||
                arg.StartsWith("--state-", StringComparison.Ordinal) ||
                arg.StartsWith("--publish-", StringComparison.Ordinal))
            {
                remaining = [];
                return false;
            }
            rest.Add(arg);
        }

        remaining = rest.ToArray();
        if (kindText is null)
        {
            return totalConns == 0 && !input.Present && !state.Present && !publish.Present;
        }

        var kind = kindText switch
        {
            "environment_baseline" => BenchScenarioKind.EnvironmentBaseline,
            "authoritative_state" => BenchScenarioKind.AuthoritativeState,
            "room_relay" => BenchScenarioKind.RoomRelay,
            _ => BenchScenarioKind.Legacy,
        };
        if (kind == BenchScenarioKind.Legacy || totalConns <= 0)
        {
            return false;
        }

        if (!input.IsUsable || !state.IsUsable || !publish.IsUsable)
        {
            return false;
        }

        BenchTrafficConfig? inputConfig = input.Present ? input.Build() : null;
        BenchTrafficConfig? stateConfig = state.Present ? state.Build() : null;
        BenchTrafficConfig? publishConfig = publish.Present ? publish.Build() : null;
        if (!Validate(kind, inputConfig, stateConfig, publishConfig))
        {
            return false;
        }

        scenario = new BenchScenarioConfig(kind, totalConns, inputConfig, stateConfig, publishConfig);
        return true;
    }

    private static bool Validate(
        BenchScenarioKind kind,
        BenchTrafficConfig? input,
        BenchTrafficConfig? state,
        BenchTrafficConfig? publish)
    {
        return kind switch
        {
            BenchScenarioKind.EnvironmentBaseline => ValidTraffic(input, BenchConstants.MinPayloadBytes) && state is null && publish is null,
            BenchScenarioKind.AuthoritativeState =>
                ValidTraffic(input, BenchConstants.MinPayloadBytes) &&
                ValidTraffic(state, BenchPayload.AuthoritativeStateMinPayloadBytes) &&
                input!.Value.TrafficId != state!.Value.TrafficId && publish is null,
            BenchScenarioKind.RoomRelay => ValidTraffic(publish, BenchConstants.MinPayloadBytes) && input is null && state is null,
            _ => false,
        };
    }

    private static void ConfigureTraffic(
        BenchMetrics metrics,
        BenchTrafficConfig traffic,
        BenchDirection direction)
    {
        metrics.SetTrafficDeadline(traffic.TrafficId, direction, traffic.DeadlineNs);
        if (traffic.RateLt > 0)
        {
            metrics.EnsureTraffic(traffic.TrafficId, direction, false);
        }
        if (traffic.RateMd > 0)
        {
            metrics.EnsureTraffic(traffic.TrafficId, direction, true);
        }
    }

    private static void ExpectLatest(
        BenchMetrics metrics,
        uint localIndex,
        uint originId,
        byte trafficId,
        BenchDirection direction,
        ulong firstSchedTsNs)
    {
        if (!metrics.ExpectLatest(localIndex, originId, trafficId, direction, firstSchedTsNs))
        {
            throw new InvalidOperationException(
                $"expected flow is outside metrics bounds: local={localIndex}, origin={originId}");
        }
    }

    private static bool ValidTraffic(BenchTrafficConfig? traffic, int minPayload)
    {
        if (traffic is not { } t || t.TrafficId == 0 || (t.RateLt == 0 && t.RateMd == 0))
        {
            return false;
        }
        return t.RateLt is >= 0 and <= 1_000_000_000 &&
               t.RateMd is >= 0 and <= 1_000_000_000 &&
               (t.RateLt == 0 || t.PayloadLt >= minPayload && t.PayloadLt <= BenchConstants.MaxPayloadBytes) &&
               (t.RateMd == 0 || t.PayloadMd >= minPayload && t.PayloadMd <= BenchConstants.MaxPayloadBytes);
    }

    private static bool TryTrafficArg(string arg, string[] args, ref int i, string prefix, MutableTraffic traffic)
    {
        if (!arg.StartsWith("--" + prefix + "-", StringComparison.Ordinal))
        {
            return false;
        }
        var name = arg[(prefix.Length + 3)..];
        switch (name)
        {
            case "traffic-id":
                traffic.Mark(MutableTraffic.TrafficIdField, TryTakeByte(args, ref i, out traffic.TrafficId));
                break;
            case "rate-lt":
                traffic.Mark(MutableTraffic.RateLtField, TryTakeDouble(args, ref i, out traffic.RateLt));
                break;
            case "rate-md":
                traffic.Mark(MutableTraffic.RateMdField, TryTakeDouble(args, ref i, out traffic.RateMd));
                break;
            case "payload-lt":
                traffic.Mark(MutableTraffic.PayloadLtField, TryTakeInt(args, ref i, out traffic.PayloadLt));
                break;
            case "payload-md":
                traffic.Mark(MutableTraffic.PayloadMdField, TryTakeInt(args, ref i, out traffic.PayloadMd));
                break;
            case "deadline-ns":
                traffic.Mark(MutableTraffic.DeadlineField, TryTakeUlong(args, ref i, out traffic.DeadlineNs));
                break;
            default:
                return false;
        }
        return true;
    }

    private static bool TryTake(string[] args, ref int i, out string value)
    {
        value = "";
        if (i + 1 >= args.Length)
        {
            return false;
        }
        value = args[++i];
        return true;
    }

    private static bool TryTakeInt(string[] args, ref int i, out int value)
    {
        value = 0;
        return TryTake(args, ref i, out var text) &&
               int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryTakeByte(string[] args, ref int i, out byte value)
    {
        value = 0;
        return TryTake(args, ref i, out var text) &&
               byte.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryTakeUlong(string[] args, ref int i, out ulong value)
    {
        value = 0;
        return TryTake(args, ref i, out var text) &&
               ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryTakeDouble(string[] args, ref int i, out double value)
    {
        value = 0;
        return TryTake(args, ref i, out var text) &&
               double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value) &&
               double.IsFinite(value);
    }

    private sealed class MutableTraffic
    {
        public const int TrafficIdField = 1 << 0;
        public const int RateLtField = 1 << 1;
        public const int RateMdField = 1 << 2;
        public const int PayloadLtField = 1 << 3;
        public const int PayloadMdField = 1 << 4;
        public const int DeadlineField = 1 << 5;
        private const int AllFields = (1 << 6) - 1;

        private int fields;
        public bool Valid = true;
        public byte TrafficId;
        public double RateLt;
        public double RateMd;
        public int PayloadLt;
        public int PayloadMd;
        public ulong DeadlineNs;

        public bool Present => fields != 0;
        public bool IsUsable => !Present || (Valid && fields == AllFields);

        public void Mark(int field, bool valid)
        {
            if ((fields & field) != 0)
            {
                Valid = false;
            }
            fields |= field;
            Valid &= valid;
        }

        public BenchTrafficConfig Build() => new(TrafficId, RateLt, RateMd, PayloadLt, PayloadMd, DeadlineNs);
    }
}
