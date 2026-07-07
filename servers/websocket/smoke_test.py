#!/usr/bin/env python3
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


HIST_BINS = 16 * 28


def assert_hist_shape(hist, label):
    expected = {"scheme", "min_ns", "max_ns", "count", "p50_ns", "p90_ns", "p99_ns", "bins"}
    if set(hist.keys()) != expected:
        raise AssertionError(f"{label}: keys {sorted(hist.keys())}, want {sorted(expected)}")
    if hist["scheme"] != "log2x16":
        raise AssertionError(f"{label}: scheme {hist['scheme']!r}")
    if int(hist["min_ns"]) != 1000 or int(hist["max_ns"]) != 100000000000:
        raise AssertionError(f"{label}: range mismatch")
    bins = hist["bins"]
    if not isinstance(bins, list) or len(bins) != HIST_BINS:
        raise AssertionError(f"{label}: bins length {len(bins) if isinstance(bins, list) else '<non-list>'}")
    if int(hist["count"]) != sum(int(v) for v in bins):
        raise AssertionError(f"{label}: count does not match sum(bins)")


def assert_class_shape(metrics, name):
    cls = metrics["classes"][name]
    expected = {
        "slots",
        "slots_broadcast",
        "submitted",
        "delivered_unique",
        "duplicates",
        "deadline_hit",
        "latency_sched_ns",
        "latency_send_ns",
        "update_gap_ns",
    }
    if set(cls.keys()) != expected:
        raise AssertionError(f"{name}: keys {sorted(cls.keys())}, want {sorted(expected)}")
    assert_hist_shape(cls["latency_sched_ns"], f"{name}.latency_sched_ns")
    assert_hist_shape(cls["latency_send_ns"], f"{name}.latency_send_ns")
    assert_hist_shape(cls["update_gap_ns"], f"{name}.update_gap_ns")
    return cls


def assert_metrics_shape(metrics):
    expected_top = {"version", "histogram", "classes", "staleness_ns", "raw"}
    if set(metrics.keys()) != expected_top:
        raise AssertionError(f"top-level keys {sorted(metrics.keys())}, want {sorted(expected_top)}")
    if int(metrics["version"]) != 1:
        raise AssertionError(f"version {metrics['version']}, want 1")
    hist_meta = metrics["histogram"]
    if hist_meta != {"scheme": "log2x16", "subbins": 16, "min_ns": 1000, "max_ns": 100000000000}:
        raise AssertionError(f"histogram metadata mismatch: {hist_meta}")
    if set(metrics["classes"].keys()) != {"loss_tolerant", "must_deliver"}:
        raise AssertionError(f"class keys mismatch: {metrics['classes'].keys()}")
    assert_class_shape(metrics, "loss_tolerant")
    assert_class_shape(metrics, "must_deliver")
    assert_hist_shape(metrics["staleness_ns"], "staleness_ns")
    if set(metrics["raw"].keys()) != {"slots", "submitted", "recv_measured", "recv_unmeasured"}:
        raise AssertionError(f"raw keys mismatch: {metrics['raw'].keys()}")


def class_delivery(metrics, name):
    cls = metrics["classes"][name]
    slots = int(cls["slots"])
    delivered = int(cls["delivered_unique"])
    if slots <= 0:
        raise AssertionError(f"{name}: no measured slots")
    if delivered != slots:
        raise AssertionError(f"{name}: delivery {delivered}/{slots}, want 1.0")


def dump_log(path, label):
    print(f"--- {label} ---", file=sys.stderr)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
    except OSError as exc:
        print(f"<failed to read log: {exc}>", file=sys.stderr)
        return
    print(data if data else "<empty>", file=sys.stderr)


def run_once(server_bin, client_bin, port):
    with tempfile.TemporaryDirectory(prefix="rudp-bench-websocket-smoke-") as tmp:
        metrics_path = os.path.join(tmp, "client-metrics.json")
        server_log_path = os.path.join(tmp, "server.log")
        client_log_path = os.path.join(tmp, "client.log")
        server_log = open(server_log_path, "wb")
        client_log = open(client_log_path, "wb")
        server_env = os.environ.copy()
        server_env.pop("BENCH_CONTROL_SOCK", None)
        server_env.pop("BENCH_METRICS_OUT", None)
        server = subprocess.Popen(
            [server_bin, "--port", str(port)],
            stdout=server_log,
            stderr=server_log,
            env=server_env,
        )
        try:
            time.sleep(1.0)
            client_env = os.environ.copy()
            client_env.pop("BENCH_CONTROL_SOCK", None)
            client_env["BENCH_METRICS_OUT"] = metrics_path
            subprocess.run(
                [
                    client_bin,
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(port),
                    "--conns",
                    "1",
                    "--proc-index",
                    "0",
                    "--origin-base",
                    "0",
                    "--rate-lt",
                    "10",
                    "--rate-md",
                    "10",
                    "--payload",
                    "64",
                    "--deadline-ns",
                    "1000000000",
                    "--staleness-period-ns",
                    "10000000",
                ],
                check=True,
                stdout=client_log,
                stderr=client_log,
                env=client_env,
                timeout=15,
            )
            with open(metrics_path, "r", encoding="utf-8") as f:
                metrics = json.load(f)
            assert_metrics_shape(metrics)
            class_delivery(metrics, "loss_tolerant")
            class_delivery(metrics, "must_deliver")
        except Exception:
            server_log.flush()
            client_log.flush()
            dump_log(server_log_path, "server log")
            dump_log(client_log_path, "client log")
            raise
        finally:
            if server.poll() is None:
                server.send_signal(signal.SIGINT)
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
            server_log.close()
            client_log.close()

        if server.returncode not in (0, -signal.SIGINT):
            raise AssertionError(f"server exited with {server.returncode}")
    return 0


def main():
    if len(sys.argv) != 3:
        print("usage: smoke_test.py SERVER CLIENT", file=sys.stderr)
        return 2

    server_bin, client_bin = sys.argv[1], sys.argv[2]
    last_error = None
    for attempt in range(20):
        port = 49152 + ((os.getpid() + attempt) % 10000)
        try:
            return run_once(server_bin, client_bin, port)
        except Exception as exc:
            last_error = exc
            time.sleep(0.5)
    raise last_error


if __name__ == "__main__":
    raise SystemExit(main())
