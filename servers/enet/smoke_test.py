#!/usr/bin/env python3
import json
import os
import signal
import subprocess
import sys
import tempfile
import time


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
    with tempfile.TemporaryDirectory(prefix="rudp-bench-enet-smoke-") as tmp:
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
            time.sleep(0.2)
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
                timeout=12,
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
