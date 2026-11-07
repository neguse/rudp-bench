#!/usr/bin/env python3
"""劣悪ベンチの run config を生成する。条件と判定の正は README の「劣悪ベンチ」。"""
import argparse
import json
import os

BASE_DELAY_MS = 50  # RFC 8867 §4.2 の既定片道遅延
CONNS = 8
# server が自分のアドレスを知る必要がある transport(接続トークンの宛先と照合する)
SERVER_HOST = {"yojimbo"}

TRANSPORTS = {
    "raw_udp": ("build-v2-raw_udp/raw_udp_sut", 42911, {}),
    "enet": ("build-v2-enet/enet_sut", 42917, {}),
    "kcp": ("build-v2-kcp/kcp_sut", 42923, {}),
    "coop_rudp": ("build-v2-coop_rudp/coop_rudp_sut", 42937, {}),
    "gns": ("build-v2-gns/gns_sut", 42921, {}),
    "msquic": ("build-v2-msquic/msquic_sut", 42919, {}),
    "msquic-cancel-on-blocked": ("build-v2-msquic/msquic_cancel_on_blocked_sut", 42920, {}),
    "yojimbo": ("build-v2-yojimbo/yojimbo_sut", 42925, {}),
    "quiche": ("build-v2-quiche/quiche_sut", 42929, {}),
    "lsquic": ("build-v2-lsquic/lsquic_sut", 42935, {}),
    "litenetlib": ("build-v2-dotnet/LiteNetLibBench/LiteNetLibBench", 42941, {}),
    "slikenet": ("build-v2-slikenet/slikenet_sut", 42927, {}),
    "websocket": ("build-v2-dotnet/WebSocketBench/WebSocketBench", 42933, {"sched_is_measurand": True}),
    "magiconion": ("build-v2-dotnet/MagicOnionBench/MagicOnionBench", 42931, {"sched_is_measurand": True}),
}


def egress(loss=0.0, jitter=0, burst=0.0, rate=None, limit=0):
    e = {"delay_ms": BASE_DELAY_MS}
    if loss:
        e["loss_pct"] = loss
    if jitter:
        e["jitter_ms"] = jitter
    if burst:
        e["loss_burst_len"] = burst
    if rate:
        e["rate"] = rate
    if limit:
        e["limit"] = limit
    return e


def symmetric(**kw):
    return {"server_egress": egress(**kw), "client_egress": egress(**kw)}


def disturbance(start_s, length_s, **kw):
    bad = symmetric(**kw)
    good = symmetric()
    return [dict(at=f"{start_s}s", **bad), dict(at=f"{start_s + length_s}s", **good)]


# name -> (window netem, schedule, window seconds, drain seconds)
STEADY = 30
WINDOW = 40
CONDITIONS = {
    "base": (symmetric(), None),
    "loss1": (symmetric(loss=1), None),
    "loss2": (symmetric(loss=2), None),
    "loss5": (symmetric(loss=5), None),
    "loss10": (symmetric(loss=10), None),
    "loss20": (symmetric(loss=20), None),
    # RFC 8867 §4.2 の最大ジッタ 30 ms + ロス 2%
    "jitter30-loss2": (symmetric(loss=2, jitter=30), None),
    # 平均 5%、平均 10 パケット連続で落ちるバースト(Gilbert-Elliott)
    "burst5x10": (symmetric(loss=5, burst=10), None),
    # 下り 1 Mbit/s(RFC 8867 の基準容量)に対し下りの負荷は約 1.4 Mbit/s。
    # 待ち行列は約 1000 ms 分(RFC 8867 §5.6)= 1 Mbit/s で 125 KB ≒ 1050 B パケット 120 個。
    "congest1m": ({"server_egress": egress(rate="1mbit", limit=120),
                   "client_egress": egress()}, None),
    "outage5": (symmetric(), disturbance(10, 5, loss=100)),
    "outage15": (symmetric(), disturbance(10, 15, loss=100)),
    "loss20-then-clean": (symmetric(), disturbance(10, 20, loss=20)),
}


def scenario():
    md = {"rate_hz": 10, "payload_bytes": 64, "deadline_ns": 1_000_000_000,
          "min_deadline_hit_ratio": 0.95, "min_eventual_delivery_ratio": 1}

    def lt(rate, payload):
        return {"rate_hz": rate, "payload_bytes": payload,
                "staleness_p99_ns": 500_000_000, "min_delivery_ratio": 0.5}

    return {"name": "badnet-auth-s1000", "kind": "authoritative_state",
            "client_input": {"traffic_id": 1, "loss_tolerant": lt(30, 64), "must_deliver": md},
            "server_state": {"traffic_id": 2, "loss_tolerant": lt(20, 1000), "must_deliver": md}}


def config(transport, cond, outdir, loopback=False):
    sut, port, extra = TRANSPORTS[transport]
    netem, schedule = CONDITIONS[cond]
    host = "127.0.0.1" if loopback else "10.200.0.1"
    cfg = {
        "transport": transport,
        "server_command": [sut, "server", "--port", str(port)] + (["--host", host] if transport in SERVER_HOST else []),
        "client_command": [sut, "client", "--host", host, "--port", str(port),
                           "--conns", "{conns}", "--proc-index", "{proc_index}",
                           "--origin-base", "{origin_id_start}"],
        "client_procs": 1,
        "total_conns": CONNS,
        "scenario": scenario(),
        "warmup": "20s",
        "duration": f"{WINDOW if schedule else STEADY}s",
        "drain": "10s" if schedule else "5s",
        "staleness_period_ns": 10_000_000,
        "output_dir": os.path.join(outdir, cond, transport),
        **extra,
    }
    if loopback:
        # 回線を絞らない手元の確認(root 不要)。短くして steady warmup で窓を早める。
        cfg.update(warmup="5s", steady_warmup=True, duration="3s", drain="1s")
        return cfg
    cfg.update({
        "netem": dict(link_mtu_bytes=1500, disable_offloads=True, **netem),
        # 事前ゲートの iperf3 は高ロスで開始時の UDP の合図が落ちて失敗する。
        # ロスは計測窓内の qdisc カウンタ(loss evidence)で検証する。
        "netem_gate_off": True,
        "server_cpus": "7,15",
        "client_cpus": "3,4,5,6,11,12,13,14",
    })
    if schedule:
        cfg["netem_schedule"] = schedule
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="session directory")
    ap.add_argument("--transports", default=",".join(TRANSPORTS))
    ap.add_argument("--conditions", default=",".join(CONDITIONS))
    ap.add_argument("--loopback", action="store_true",
                    help="netem なしの 127.0.0.1 で短く回す(条件は base のみ)")
    args = ap.parse_args()
    if args.loopback:
        args.conditions = "base"
    os.makedirs(os.path.join(args.out, "configs"), exist_ok=True)
    for cond in args.conditions.split(","):
        for t in args.transports.split(","):
            path = os.path.join(args.out, "configs", f"{cond}__{t}.json")
            with open(path, "w") as f:
                json.dump(config(t, cond, args.out, args.loopback), f, indent=2)
            print(path)


if __name__ == "__main__":
    main()
