#!/usr/bin/env python3
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from describe_contract import validate_describe_pair


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
        "expected_flows",
        "observed_flows",
        "never_received_flows",
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
    expected_top = {"version", "histogram", "classes", "traffic", "staleness_ns", "raw"}
    if set(metrics.keys()) != expected_top:
        raise AssertionError(f"top-level keys {sorted(metrics.keys())}, want {sorted(expected_top)}")
    if int(metrics["version"]) != 2:
        raise AssertionError(f"version {metrics['version']}, want 2")
    hist_meta = metrics["histogram"]
    if hist_meta != {"scheme": "log2x16", "subbins": 16, "min_ns": 1000, "max_ns": 100000000000}:
        raise AssertionError(f"histogram metadata mismatch: {hist_meta}")
    if set(metrics["classes"].keys()) != {"loss_tolerant", "must_deliver"}:
        raise AssertionError(f"class keys mismatch: {metrics['classes'].keys()}")
    assert_class_shape(metrics, "loss_tolerant")
    assert_class_shape(metrics, "must_deliver")
    assert_hist_shape(metrics["staleness_ns"], "staleness_ns")
    if set(metrics["raw"].keys()) != {"slots", "submitted", "recv_measured", "recv_unmeasured", "timestamp_order_violations"}:
        raise AssertionError(f"raw keys mismatch: {metrics['raw'].keys()}")
    if not isinstance(metrics["traffic"], list) or not metrics["traffic"]:
        raise AssertionError("traffic series is empty")
    seen = set()
    for index, traffic in enumerate(metrics["traffic"]):
        expected = {
            "traffic_id",
            "direction",
            "class",
            "deadline_ns",
            "slots",
            "slots_broadcast",
            "submitted",
            "delivered_unique",
            "duplicates",
            "deadline_hit",
            "expected_flows",
            "observed_flows",
            "never_received_flows",
            "latency_sched_ns",
            "latency_send_ns",
            "update_gap_ns",
            "staleness_ns",
        }
        if set(traffic.keys()) != expected:
            raise AssertionError(f"traffic[{index}]: keys {sorted(traffic.keys())}, want {sorted(expected)}")
        key = (int(traffic["traffic_id"]), traffic["direction"], traffic["class"])
        if key in seen:
            raise AssertionError(f"duplicate traffic series {key}")
        seen.add(key)
        if traffic["direction"] not in {"room_relay", "client_to_server", "server_to_client"}:
            raise AssertionError(f"traffic[{index}]: invalid direction {traffic['direction']!r}")
        if traffic["class"] not in {"loss_tolerant", "must_deliver"}:
            raise AssertionError(f"traffic[{index}]: invalid class {traffic['class']!r}")
        for hist_name in ("latency_sched_ns", "latency_send_ns", "update_gap_ns", "staleness_ns"):
            assert_hist_shape(traffic[hist_name], f"traffic[{index}].{hist_name}")


def traffic_series(metrics, traffic_id, direction, class_name):
    for traffic in metrics["traffic"]:
        if (
            int(traffic["traffic_id"]) == traffic_id
            and traffic["direction"] == direction
            and traffic["class"] == class_name
        ):
            return traffic
    raise AssertionError(f"missing traffic series {(traffic_id, direction, class_name)}")


def assert_authoritative(server_metrics, client_metrics):
    for class_name in ("loss_tolerant", "must_deliver"):
        server_input = traffic_series(server_metrics, 1, "client_to_server", class_name)
        server_state = traffic_series(server_metrics, 2, "server_to_client", class_name)
        client_input = traffic_series(client_metrics, 1, "client_to_server", class_name)
        client_state = traffic_series(client_metrics, 2, "server_to_client", class_name)

        if int(server_input["slots"]) != 0 or int(server_input["delivered_unique"]) <= 0:
            raise AssertionError(f"server {class_name} input is not a receive-only sink: {server_input}")
        if int(server_state["slots"]) <= 0 or int(server_state["submitted"]) != int(server_state["slots"]):
            raise AssertionError(f"server {class_name} state was not fully submitted: {server_state}")
        if int(server_state["slots"]) % 3 != 0:
            raise AssertionError(f"server {class_name} state slots are not expanded over the frozen c3 roster")
        if int(client_input["slots"]) <= 0 or int(client_input["submitted"]) != int(client_input["slots"]):
            raise AssertionError(f"client {class_name} input was not fully submitted: {client_input}")
        if int(client_state["slots"]) != 0 or int(client_state["delivered_unique"]) <= 0:
            raise AssertionError(f"client {class_name} state is not receive-only: {client_state}")

    server_input_lt = traffic_series(server_metrics, 1, "client_to_server", "loss_tolerant")
    client_state_lt = traffic_series(client_metrics, 2, "server_to_client", "loss_tolerant")
    for label, traffic in (("server input", server_input_lt), ("client state", client_state_lt)):
        if int(traffic["expected_flows"]) != 3:
            raise AssertionError(f"{label}: expected_flows={traffic['expected_flows']}, want 3")
        if int(traffic["observed_flows"]) != 3 or int(traffic["never_received_flows"]) != 0:
            raise AssertionError(f"{label}: latest flow coverage mismatch: {traffic}")


def dump_log(path, label):
    print(f"--- {label} ---", file=sys.stderr)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
    except OSError as exc:
        print(f"<failed to read log: {exc}>", file=sys.stderr)
        return
    print(data if data else "<empty>", file=sys.stderr)


def read_diagnostics(path):
    progress = None
    invalid_payload = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("BENCH_PROGRESS "):
                progress = dict(item.split("=", 1) for item in line.split()[1:])
            elif line.startswith("BENCH_INVALID_PAYLOAD "):
                invalid_payload = int(line.split("=", 1)[1])
    if progress is None or invalid_payload is None:
        raise AssertionError(f"missing benchmark diagnostics in {path}")
    return progress, invalid_payload


def assert_progress(server_log_path, client_log_path):
    server, server_invalid = read_diagnostics(server_log_path)
    client, client_invalid = read_diagnostics(client_log_path)
    if server_invalid != 0 or client_invalid != 0:
        raise AssertionError(f"invalid payloads: server={server_invalid}, client={client_invalid}")
    if server["role"] != "server" or int(server["local_conns"]) != 0 or int(server["roster_conns"]) != 3:
        raise AssertionError(f"invalid server progress identity: {server}")
    if client["role"] != "client" or int(client["local_conns"]) != 3 or int(client["roster_conns"]) != 3:
        raise AssertionError(f"invalid client progress identity: {client}")
    required_positive = (
        "input_last_sent_min",
        "input_last_sent_max",
        "state_header_seq_recv_min",
        "state_header_seq_recv_max",
        "state_applied_input_seq_recv_min",
        "state_applied_input_seq_recv_max",
    )
    if any(int(client[name]) <= 0 for name in required_positive):
        raise AssertionError(f"authoritative progress did not advance: {client}")
    if int(server["server_state_ticks"]) <= 0:
        raise AssertionError(f"server state tick did not advance: {server}")
    if int(client["state_header_seq_recv_max"]) <= int(client["input_last_sent_max"]):
        raise AssertionError(f"state sequence did not advance independently of input: {client}")


def run_once(server_bin, client_bin, port):
    with tempfile.TemporaryDirectory(prefix="rudp-bench-websocket-smoke-") as tmp:
        metrics_path = os.path.join(tmp, "client-metrics.json")
        server_metrics_path = os.path.join(tmp, "server-metrics.json")
        server_log_path = os.path.join(tmp, "server.log")
        client_log_path = os.path.join(tmp, "client.log")
        server_log = open(server_log_path, "wb")
        client_log = open(client_log_path, "wb")
        server_env = os.environ.copy()
        server_env.pop("BENCH_CONTROL_SOCK", None)
        server_env["BENCH_METRICS_OUT"] = server_metrics_path
        scenario_args = [
            "--scenario",
            "authoritative_state",
            "--total-conns",
            "3",
            "--input-traffic-id",
            "1",
            "--input-rate-lt",
            "13",
            "--input-rate-md",
            "5",
            "--input-payload-lt",
            "64",
            "--input-payload-md",
            "64",
            "--input-deadline-ns",
            "1000000000",
            "--state-traffic-id",
            "2",
            "--state-rate-lt",
            "20",
            "--state-rate-md",
            "5",
            "--state-payload-lt",
            "64",
            "--state-payload-md",
            "64",
            "--state-deadline-ns",
            "1000000000",
        ]
        server = subprocess.Popen(
            [server_bin, "--port", str(port), *scenario_args, "--staleness-period-ns", "10000000"],
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
                    "3",
                    "--proc-index",
                    "0",
                    "--origin-base",
                    "0",
                    "--deadline-ns",
                    "1000000000",
                    "--staleness-period-ns",
                    "10000000",
                    *scenario_args,
                ],
                check=True,
                stdout=client_log,
                stderr=client_log,
                env=client_env,
                timeout=15,
            )
            with open(metrics_path, "r", encoding="utf-8") as f:
                client_metrics = json.load(f)
            server.wait(timeout=10)
            with open(server_metrics_path, "r", encoding="utf-8") as f:
                server_metrics = json.load(f)
            assert_metrics_shape(server_metrics)
            assert_metrics_shape(client_metrics)
            assert_authoritative(server_metrics, client_metrics)
            assert_progress(server_log_path, client_log_path)
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
    validate_describe_pair(server_bin, client_bin)
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
