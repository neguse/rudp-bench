#!/usr/bin/env python3
# Native adapters 共通の dev-mode loopback smoke。BENCH_CONTROL_SOCK を外して
# 自前スケジュールで走らせ、wire/scenario/metrics 契約を無損失で確認する。
import json
import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time


STARTUP_DELAY = 0.6
CLIENT_TIMEOUT = 18


def validate_describe(server_bin, client_bin):
    descriptions = []
    for binary in (server_bin, client_bin):
        result = subprocess.run(
            [binary, "--describe"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        doc = json.loads(result.stdout)
        for key in (
            "transport",
            "class_mapping",
            "max_payload_bytes",
            "scenarios",
            "tuning",
        ):
            if key not in doc:
                raise AssertionError(f"{binary}: --describe missing {key}")
        if set(doc["class_mapping"]) != {"loss_tolerant", "must_deliver"}:
            raise AssertionError(f"{binary}: invalid class_mapping")
        for class_name, mapping in doc["class_mapping"].items():
            if not isinstance(mapping, dict):
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name} is not an object"
                )
            if set(mapping) != {"primitive", "delivery", "ordering", "realization"}:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name} schema mismatch"
                )
            if not isinstance(mapping["primitive"], str) or not mapping["primitive"]:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.primitive is empty"
                )
            if mapping["delivery"] not in {"best_effort", "reliable"}:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.delivery is invalid"
                )
            if mapping["ordering"] not in {"unordered", "ordered"}:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.ordering is invalid"
                )
            if mapping["realization"] not in {
                "native",
                "emulated",
                "reliable_fallback",
                "unsupported",
            }:
                raise AssertionError(
                    f"{binary}: class_mapping.{class_name}.realization is invalid"
                )
        if int(doc["max_payload_bytes"]) < 64:
            raise AssertionError(f"{binary}: max_payload_bytes too small")
        if set(doc["scenarios"]) != {
            "environment_baseline",
            "authoritative_state",
            "room_relay",
        }:
            raise AssertionError(f"{binary}: scenario advertisement mismatch")
        for index, tuning in enumerate(doc["tuning"]):
            if not isinstance(tuning, dict):
                raise AssertionError(f"{binary}: tuning[{index}] is not an object")
            if set(tuning) != {"knob", "value", "upstream_ref"}:
                raise AssertionError(f"{binary}: tuning[{index}] schema mismatch")
            if not all(isinstance(tuning[key], str) and tuning[key] for key in tuning):
                raise AssertionError(f"{binary}: tuning[{index}] has empty metadata")
        descriptions.append(doc)
    if descriptions[0]["transport"] != descriptions[1]["transport"]:
        raise AssertionError("server/client transport descriptions differ")
    if descriptions[0]["class_mapping"] != descriptions[1]["class_mapping"]:
        raise AssertionError("server/client class mappings differ")


def class_delivery(metrics, name):
    cls = metrics["classes"][name]
    slots = int(cls["slots"])
    delivered = int(cls["delivered_unique"])
    if slots <= 0:
        raise AssertionError(f"{name}: no measured slots")
    if delivered != slots:
        raise AssertionError(f"{name}: delivery {delivered}/{slots}, want 1.0")


def traffic(metrics, traffic_id, direction, class_name):
    matches = [
        item
        for item in metrics.get("traffic", [])
        if int(item["traffic_id"]) == traffic_id
        and item["direction"] == direction
        and item["class"] == class_name
    ]
    if len(matches) != 1:
        raise AssertionError(
            f"traffic {traffic_id}/{direction}/{class_name}: "
            f"expected one series, got {len(matches)}"
        )
    return matches[0]


def dump_log(path, label):
    print(f"--- {label} ---", file=sys.stderr)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
    except OSError as exc:
        print(f"<failed to read log: {exc}>", file=sys.stderr)
        return
    print(data if data else "<empty>", file=sys.stderr)


def read_control_message(control, label):
    line = control.readline()
    if not line:
        raise AssertionError(f"{label}: control socket closed")
    return json.loads(line)


def collect_done_messages(connections, timeout):
    pending = dict(connections)
    buffers = {sock: bytearray() for sock in pending}
    done = {}
    deadline = time.monotonic() + timeout
    while pending and time.monotonic() < deadline:
        readable, _, _ = select.select(
            list(pending), [], [], max(0.0, deadline - time.monotonic())
        )
        if not readable:
            break
        for sock in readable:
            chunk = sock.recv(65536)
            if not chunk:
                role = pending.pop(sock)
                if role not in done:
                    raise AssertionError(f"{role}: closed before done")
                continue
            buffers[sock].extend(chunk)
            while b"\n" in buffers[sock]:
                line, _, remainder = buffers[sock].partition(b"\n")
                buffers[sock] = bytearray(remainder)
                message = json.loads(line)
                if message.get("type") == "done":
                    role = pending[sock]
                    done[role] = message.get("stats")
    if set(done) != {"server", "client"}:
        raise AssertionError(f"missing done messages: got {sorted(done)}")
    return done


def validate_authoritative_done(done, state_lt_slots):
    required = {
        "role",
        "local_conns",
        "roster_conns",
        "input_last_sent_min",
        "input_last_sent_max",
        "state_header_seq_recv_min",
        "state_header_seq_recv_max",
        "state_applied_input_seq_recv_min",
        "state_applied_input_seq_recv_max",
        "server_state_ticks",
    }
    for role in ("server", "client"):
        stats = done[role]
        if not isinstance(stats, dict) or int(stats.get("invalid_payload", -1)) != 0:
            raise AssertionError(f"{role}: invalid done.stats {stats!r}")
        progress = stats.get("authoritative_progress")
        if not isinstance(progress, dict) or set(progress) != required:
            raise AssertionError(f"{role}: authoritative_progress schema mismatch")
        if progress["role"] != role:
            raise AssertionError(f"{role}: progress role mismatch")
        if int(progress["roster_conns"]) != 3:
            raise AssertionError(f"{role}: roster coverage mismatch")

    server = done["server"]["authoritative_progress"]
    if int(server["local_conns"]) != 0:
        raise AssertionError("server: local_conns must be zero")
    if state_lt_slots % 3 != 0:
        raise AssertionError("server: LT state slots are not per-target expanded")
    if int(server["server_state_ticks"]) != state_lt_slots // 3:
        raise AssertionError("server: global LT tick count does not match state slots")

    client = done["client"]["authoritative_progress"]
    if int(client["local_conns"]) != 3:
        raise AssertionError("client: local connection coverage mismatch")
    for key in (
        "input_last_sent_min",
        "state_header_seq_recv_min",
        "state_applied_input_seq_recv_min",
    ):
        if int(client[key]) <= 0:
            raise AssertionError(f"client: {key} did not cover every connection")
    for minimum, maximum in (
        ("input_last_sent_min", "input_last_sent_max"),
        ("state_header_seq_recv_min", "state_header_seq_recv_max"),
        ("state_applied_input_seq_recv_min", "state_applied_input_seq_recv_max"),
    ):
        if int(client[minimum]) > int(client[maximum]):
            raise AssertionError(f"client: inverted progress range {minimum}/{maximum}")
    if int(client["state_applied_input_seq_recv_max"]) > int(
        client["input_last_sent_max"]
    ):
        raise AssertionError("client: applied seq exceeds LT input seq")


def authoritative_args():
    return [
        "--scenario",
        "authoritative_state",
        "--total-conns",
        "3",
        "--input-traffic-id",
        "1",
        "--input-rate-lt",
        "20",
        "--input-rate-md",
        "5",
        "--input-payload-lt",
        "48",
        "--input-payload-md",
        "48",
        "--input-deadline-ns",
        "1000000000",
        "--state-traffic-id",
        "2",
        "--state-rate-lt",
        "15",
        "--state-rate-md",
        "5",
        "--state-payload-lt",
        "64",
        "--state-payload-md",
        "64",
        "--state-deadline-ns",
        "500000000",
    ]


def flow_scenario_args(kind):
    if kind == "environment_baseline":
        prefix = "input"
        traffic_id = 3
    elif kind == "room_relay":
        prefix = "publish"
        traffic_id = 4
    else:
        raise ValueError(f"unknown flow scenario {kind}")
    return [
        "--scenario",
        kind,
        "--total-conns",
        "3",
        f"--{prefix}-traffic-id",
        str(traffic_id),
        f"--{prefix}-rate-lt",
        "10",
        f"--{prefix}-rate-md",
        "0",
        f"--{prefix}-payload-lt",
        "64",
        f"--{prefix}-payload-md",
        "0",
        f"--{prefix}-deadline-ns",
        "0",
    ]


def run_once(server_bin, client_bin, port):
    with tempfile.TemporaryDirectory(prefix="rudp-bench-raw_udp-smoke-") as tmp:
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
            time.sleep(STARTUP_DELAY)
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
                    "2",
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
                timeout=CLIENT_TIMEOUT,
            )
            with open(metrics_path, "r", encoding="utf-8") as f:
                metrics = json.load(f)
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


def run_flow_scenario_once(server_bin, client_bin, port, kind):
    with tempfile.TemporaryDirectory(prefix=f"rudp-bench-{kind}-") as tmp:
        metrics_path = os.path.join(tmp, "client-metrics.json")
        server_log_path = os.path.join(tmp, "server.log")
        client_log_path = os.path.join(tmp, "client.log")
        server_log = open(server_log_path, "wb")
        client_log = open(client_log_path, "wb")
        args = flow_scenario_args(kind)
        server_env = os.environ.copy()
        server_env.pop("BENCH_CONTROL_SOCK", None)
        server_env.pop("BENCH_METRICS_OUT", None)
        server = subprocess.Popen(
            [server_bin, "--port", str(port), *args],
            stdout=server_log,
            stderr=server_log,
            env=server_env,
        )
        try:
            time.sleep(STARTUP_DELAY)
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
                    "--staleness-period-ns",
                    "10000000",
                    *args,
                ],
                check=True,
                stdout=client_log,
                stderr=client_log,
                env=client_env,
                timeout=CLIENT_TIMEOUT,
            )
        except Exception:
            server_log.flush()
            client_log.flush()
            dump_log(server_log_path, f"{kind} server log")
            dump_log(client_log_path, f"{kind} client log")
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
            raise AssertionError(f"{kind} server exited with {server.returncode}")
        with open(metrics_path, "r", encoding="utf-8") as f:
            metrics = json.load(f)
        traffic_id = 3 if kind == "environment_baseline" else 4
        series = traffic(metrics, traffic_id, "room_relay", "loss_tolerant")
        expected_flows = 3 if kind == "environment_baseline" else 9
        if int(series["expected_flows"]) != expected_flows:
            raise AssertionError(
                f"{kind}: expected_flows={series['expected_flows']}, "
                f"want {expected_flows}"
            )
        if int(series["observed_flows"]) != expected_flows:
            raise AssertionError(
                f"{kind}: observed_flows={series['observed_flows']}, "
                f"want {expected_flows}"
            )
        if int(series["never_received_flows"]) != 0:
            raise AssertionError(f"{kind}: expected flow was never received")
    return 0


def run_authoritative_once(server_bin, client_bin, port):
    with tempfile.TemporaryDirectory(prefix="rudp-bench-raw_udp-auth-smoke-") as tmp:
        server_metrics_path = os.path.join(tmp, "server-metrics.json")
        client_metrics_path = os.path.join(tmp, "client-metrics.json")
        server_log_path = os.path.join(tmp, "server.log")
        client_log_path = os.path.join(tmp, "client.log")
        server_log = open(server_log_path, "wb")
        client_log = open(client_log_path, "wb")
        common = authoritative_args()
        control_path = os.path.join(tmp, "control.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(control_path)
        listener.listen(2)
        listener.settimeout(CLIENT_TIMEOUT)
        server_env = os.environ.copy()
        server_env["BENCH_CONTROL_SOCK"] = control_path
        server_env["BENCH_METRICS_OUT"] = server_metrics_path
        server = subprocess.Popen(
            [server_bin, "--port", str(port), *common],
            stdout=server_log,
            stderr=server_log,
            env=server_env,
        )
        client = None
        server_conn = None
        client_conn = None
        server_control = None
        client_control = None
        done = None
        try:
            server_conn, _ = listener.accept()
            server_conn.settimeout(CLIENT_TIMEOUT)
            server_control = server_conn.makefile("rwb", buffering=0)
            server_hello = read_control_message(server_control, "server hello")
            server_ready = read_control_message(server_control, "server ready")
            if server_hello.get("type") != "hello" or server_hello.get("role") != "server":
                raise AssertionError(f"unexpected server hello: {server_hello}")
            if server_ready.get("type") != "ready":
                raise AssertionError(f"unexpected server ready: {server_ready}")

            time.sleep(STARTUP_DELAY)
            client_env = os.environ.copy()
            client_env["BENCH_CONTROL_SOCK"] = control_path
            client_env["BENCH_METRICS_OUT"] = client_metrics_path
            client = subprocess.Popen(
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
                    "--staleness-period-ns",
                    "10000000",
                    *common,
                ],
                stdout=client_log,
                stderr=client_log,
                env=client_env,
            )
            client_conn, _ = listener.accept()
            client_conn.settimeout(CLIENT_TIMEOUT)
            client_control = client_conn.makefile("rwb", buffering=0)
            client_hello = read_control_message(client_control, "client hello")
            client_ready = read_control_message(client_control, "client ready")
            if client_hello.get("type") != "hello" or client_hello.get("role") != "client":
                raise AssertionError(f"unexpected client hello: {client_hello}")
            if client_ready.get("type") != "ready" or int(client_ready.get("conns", -1)) != 3:
                raise AssertionError(f"unexpected client ready: {client_ready}")

            # Give transports with explicit registration queues time to publish
            # the full authoritative roster before the schedule is released.
            time.sleep(0.2)
            start_at = time.monotonic_ns() + 400_000_000
            schedule = {
                "type": "schedule",
                "start_at_ns": start_at,
                "stop_at_ns": start_at + 2_000_000_000,
                "drain_until_ns": start_at + 2_500_000_000,
            }
            wire_schedule = (
                json.dumps(schedule, separators=(",", ":")).encode() + b"\n"
            )
            server_control.write(wire_schedule)
            client_control.write(wire_schedule)
            done = collect_done_messages(
                {server_conn: "server", client_conn: "client"}, CLIENT_TIMEOUT
            )
            client.wait(timeout=5)
            server.wait(timeout=5)
            if client.returncode != 0:
                raise AssertionError(f"client exited with {client.returncode}")
        except Exception:
            server_log.flush()
            client_log.flush()
            dump_log(server_log_path, "authoritative server log")
            dump_log(client_log_path, "authoritative client log")
            raise
        finally:
            if client is not None and client.poll() is None:
                client.kill()
                client.wait(timeout=5)
            if server.poll() is None:
                server.kill()
                server.wait(timeout=5)
            if server_control is not None:
                server_control.close()
            elif server_conn is not None:
                server_conn.close()
            if client_control is not None:
                client_control.close()
            elif client_conn is not None:
                client_conn.close()
            listener.close()
            server_log.close()
            client_log.close()

        if server.returncode != 0:
            dump_log(server_log_path, "authoritative server log")
            dump_log(client_log_path, "authoritative client log")
            raise AssertionError(f"server exited with {server.returncode}")

        with open(server_metrics_path, "r", encoding="utf-8") as f:
            server_metrics = json.load(f)
        with open(client_metrics_path, "r", encoding="utf-8") as f:
            client_metrics = json.load(f)

        for class_name in ("loss_tolerant", "must_deliver"):
            input_sent = traffic(client_metrics, 1, "client_to_server", class_name)
            input_recv = traffic(server_metrics, 1, "client_to_server", class_name)
            state_sent = traffic(server_metrics, 2, "server_to_client", class_name)
            state_recv = traffic(client_metrics, 2, "server_to_client", class_name)
            if int(input_sent["slots"]) <= 0 or int(state_sent["slots"]) <= 0:
                raise AssertionError(f"{class_name}: no authoritative measured slots")
            if int(input_sent["slots"]) != int(input_recv["delivered_unique"]):
                raise AssertionError(f"{class_name}: client input was not fully consumed")
            if int(input_sent["delivered_unique"]) != 0:
                raise AssertionError(f"{class_name}: authoritative input was echoed")
            if int(state_sent["slots"]) != int(state_recv["delivered_unique"]):
                raise AssertionError(f"{class_name}: server state was not fully delivered")
            if int(state_sent["slots_broadcast"]) != 0:
                raise AssertionError(f"{class_name}: state must use per-target unicast slots")
            if int(state_sent["slots"]) % 3 != 0:
                raise AssertionError(f"{class_name}: state slots were not expanded to c=3")
            if int(input_sent["deadline_ns"]) != 1000000000:
                raise AssertionError(f"{class_name}: input deadline metadata mismatch")
            if int(input_recv["deadline_ns"]) != 1000000000:
                raise AssertionError(f"{class_name}: server input deadline metadata mismatch")
            if int(state_sent["deadline_ns"]) != 500000000:
                raise AssertionError(f"{class_name}: server state deadline metadata mismatch")
            if int(state_recv["deadline_ns"]) != 500000000:
                raise AssertionError(f"{class_name}: client state deadline metadata mismatch")

        input_latest = traffic(server_metrics, 1, "client_to_server", "loss_tolerant")
        state_latest = traffic(client_metrics, 2, "server_to_client", "loss_tolerant")
        for label, series in (("input", input_latest), ("state", state_latest)):
            if int(series["expected_flows"]) != 3:
                raise AssertionError(f"{label}: expected_flows != 3")
            if int(series["never_received_flows"]) != 0:
                raise AssertionError(f"{label}: an expected flow was never received")
        state_sent = traffic(server_metrics, 2, "server_to_client", "loss_tolerant")
        validate_authoritative_done(done, int(state_sent["slots"]))
    return 0


def run_incomplete_roster_gate(server_bin, port):
    with tempfile.TemporaryDirectory(prefix="rudp-bench-roster-gate-") as tmp:
        control_path = os.path.join(tmp, "control.sock")
        server_log_path = os.path.join(tmp, "server.log")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(control_path)
        listener.listen(1)
        listener.settimeout(8)
        server_log = open(server_log_path, "wb")
        env = os.environ.copy()
        env["BENCH_CONTROL_SOCK"] = control_path
        env.pop("BENCH_METRICS_OUT", None)
        server = subprocess.Popen(
            [server_bin, "--port", str(port), *authoritative_args()],
            stdout=server_log,
            stderr=server_log,
            env=env,
        )
        conn = None
        control = None
        try:
            conn, _ = listener.accept()
            conn.settimeout(5)
            control = conn.makefile("rwb", buffering=0)
            hello = json.loads(control.readline())
            ready = json.loads(control.readline())
            if hello.get("type") != "hello" or hello.get("role") != "server":
                raise AssertionError(f"unexpected server hello: {hello}")
            if ready.get("type") != "ready":
                raise AssertionError(f"unexpected server ready: {ready}")
            start_at = time.monotonic_ns() + 200_000_000
            schedule = {
                "type": "schedule",
                "start_at_ns": start_at,
                "stop_at_ns": start_at + 200_000_000,
                "drain_until_ns": start_at + 400_000_000,
            }
            control.write(json.dumps(schedule, separators=(",", ":")).encode() + b"\n")
            ack = json.loads(control.readline())
            if ack.get("type") != "sched_ack":
                raise AssertionError(f"unexpected schedule ack: {ack}")
            server.wait(timeout=5)
            if server.returncode == 0:
                raise AssertionError("incomplete authoritative roster was accepted")
            server_log.flush()
            with open(server_log_path, "r", encoding="utf-8", errors="replace") as f:
                if "authoritative roster incomplete" not in f.read():
                    raise AssertionError("missing incomplete-roster diagnostic")
        except Exception:
            server_log.flush()
            dump_log(server_log_path, "incomplete-roster server log")
            raise
        finally:
            if server.poll() is None:
                server.kill()
                server.wait(timeout=5)
            if control is not None:
                control.close()
            elif conn is not None:
                conn.close()
            listener.close()
            server_log.close()
    return 0


def main():
    if len(sys.argv) != 3:
        print("usage: smoke_test.py SERVER CLIENT", file=sys.stderr)
        return 2

    server_bin, client_bin = sys.argv[1], sys.argv[2]
    validate_describe(server_bin, client_bin)
    last_error = None
    for attempt in range(20):
        # canonical 42917 は避け、各 subtest に別の 49xxx port を使う。
        port = 49152 + ((os.getpid() + attempt * 7) % 9000)
        try:
            run_once(server_bin, client_bin, port)
            run_flow_scenario_once(
                server_bin, client_bin, port + 1, "environment_baseline"
            )
            run_flow_scenario_once(server_bin, client_bin, port + 2, "room_relay")
            run_authoritative_once(server_bin, client_bin, port + 3)
            return run_incomplete_roster_gate(server_bin, port + 4)
        except Exception as exc:
            last_error = exc
            time.sleep(0.5)
    raise last_error


if __name__ == "__main__":
    raise SystemExit(main())
