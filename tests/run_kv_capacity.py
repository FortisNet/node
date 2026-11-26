#!/usr/bin/env python3
"""Run disposable KV storage benchmarks and an optional loopback-only guest.

Host measures timed open-loop arrivals at the storage layer. Guest measures
the real HTTP probe route and includes guest TCP, admission, global route
lock, and storage. The host batches only synthetic unconditional mutations.
Neither harness addresses an existing node or opens an existing KV store.

Run the direct KV workload:
  python3 tests/run_kv_capacity.py host --seconds 5

Run the guest on a dedicated Linux KVM host and fresh block storage mount:
  python3 tests/run_kv_capacity.py guest --image IMAGE --storage-root /mnt/benchmark
Add --emulated for a local QEMU/TCG run. Omitting --storage-root uses a
fresh Docker volume. Both ports bind to loopback, and the process and its
storage directory are removed after the replay check. Omit the production
host and volume entirely to avoid affecting its service.

Guest status 0 means a transport error or timeout. Driver rejection happens
before the guest sees the request, and differs from an HTTP response. Guest
latency measures time from the scheduled offer through HTTP completion,
including any delay in the driver. Health samples time only their request.
Short guest runs leave the production compaction policy intact. Use the
host workload for forced compaction and in-frame crash injection.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, nullcontext
import http.client
import json
import os
from pathlib import Path
import platform
import secrets
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
# The KV store's units (KV_STORE_UNITS in the Makefile).
KV_STORE_UNITS = ("kv_store", "kv_index", "kv_log", "kv_recovery", "kv_commit", "kv_compaction", "kv_batch")


def host(args):
    print(json.dumps({"environment": "host direct KV", "platform": platform.platform(),
                      "filesystem": "temporary directory", "keys": 4096,
                      "value_bytes": 256, "hot_percent": args.hot_percent,
                      "workers": args.workers, "queue": args.queue, "compaction": True}),
          flush=True)
    with tempfile.TemporaryDirectory(prefix="fortis-kv-capacity-") as temp:
        binary = Path(temp) / "benchmark"
        subprocess.run([args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pthread", "-DFORTIS_KV_TEST_FAILURES",
                        str(ROOT / "tests/kv_capacity_benchmark.cpp"),
                        *(str(ROOT / "src/storage" / f"{unit}.cpp") for unit in KV_STORE_UNITS),
                        "-o", str(binary)], check=True)
        # 0 seconds means simultaneous arrival, followed by three sustained
        # fixed-rate stages. Each variant recreates exactly the same seed.
        for seconds, reads, writes in [(0, 100000, 10000),
                                      (args.seconds, 2000, 200),
                                      (args.seconds, 20000, 2000),
                                      (args.seconds, 100000, 10000)]:
            if args.parallel_point_reads:
                variants = [(1, False, False, False), (1, False, True, False)]
            elif args.group_commit:
                variants = [(1, False, False, False), (1, True, False, False)]
            elif args.commit_slots:
                variants = [(1, False, False, False), (1, False, False, True)]
            else:
                variants = [(1, False, False, False), (args.batch, False, False, False)]
            for batch, group_commit, parallel_reads, commit_slots in variants:
                with tempfile.TemporaryDirectory(prefix="workload-", dir=temp) as data:
                    output = subprocess.check_output([
                        str(binary), data, str(reads), str(writes), str(seconds), str(args.workers),
                        str(args.queue), str(batch), str(args.hot_percent), "1", str(int(group_commit)),
                        str(int(parallel_reads)), str(int(commit_slots))], text=True)
                    record = json.loads(output)
                    record.update(mode="sustained" if seconds else "burst", offered_seconds=seconds,
                                  batch_limit=batch, group_commit=group_commit,
                                  durability="commit-slots" if commit_slots else "rollback-marker")
                    print(json.dumps(record), flush=True)
        for crash in (["crash", "crash-commit-slots"] if args.commit_slots else ["crash"]):
            with tempfile.TemporaryDirectory(prefix="crash-", dir=temp) as data:
                report = subprocess.check_output([str(binary), data, crash], text=True)
                print(json.dumps({"mode": "process-crash", "variant": crash,
                                  **json.loads(report)}), flush=True)


def docker(*args, check=True):
    return subprocess.run(["docker", *args], check=check, capture_output=True, text=True,
                          timeout=90)


def request(port, method, route, body=None, headers=None):
    started = time.monotonic()
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        if body is None and method == "POST":
            body = b""
        connection.request(method, route, body=body, headers=headers or {})
        response = connection.getresponse()
        raw = response.read(65536)
        data = json.loads(raw) if raw and response.status in (200, 201) else None
        return response.status, data, round((time.monotonic() - started) * 1e6)
    except (OSError, ValueError, http.client.HTTPException):
        return 0, None, round((time.monotonic() - started) * 1e6)
    finally:
        connection.close()


def listen_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


@contextmanager
def guest_container(name, volume, image, emulated, data_path):
    port, health = listen_port(), listen_port()
    while port == health:
        health = listen_port()
    command = ["/opt/fortis/guest-launcher"]
    command += ["--memory", "512Mi", "--env", "PORT=8080", "--env", "NODE_ENV=development",
                "-p", "8080:8080", "--env", "FORTIS_HEALTH_PORT=8081", "-p", "8081:8081",
                "--env", "FORTIS_KV_LOCK_MODE=host", "--volume", "/data:/data",
                "--env", "FORTIS_ENABLE_DIAGNOSTIC_PROBES=1", "/opt/fortis/kernel"]
    acceleration = ["--device", "/dev/kvm"]
    if emulated:
        command.insert(command.index("--memory"), "--disable-acceleration")
        acceleration = []
    storage = ["--mount", f"type=bind,source={data_path},target=/data"] if data_path else [
        "-v", f"{volume}:/data"]
    docker("run", "-d", "--name", name, "--platform", "linux/amd64",
           *acceleration, "-p", f"127.0.0.1:{port}:8080", "-p", f"127.0.0.1:{health}:8081",
           *storage, image, *command)
    try:
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            code, data, _ = request(health, "GET", "/healthz")
            if code == 200 and data.get("ok"):
                yield port, health
                return
            if docker("inspect", "--format", "{{.State.Running}}", name).stdout.strip() != "true":
                break
            time.sleep(0.2)
        logs = docker("logs", "--tail", "30", name, check=False)
        raise RuntimeError(f"guest failed to become healthy: {logs.stdout[-1500:]}{logs.stderr[-1000:]}")
    finally:
        docker("rm", "-f", name, check=False)
        if docker("container", "inspect", name, check=False).returncode == 0:
            raise RuntimeError(f"could not remove disposable container {name}")


def guest_load(port, health_port, reads, writes, rate, workers):
    statuses = {"read": {}, "write": {}, "health": {}}
    latencies = {"read": [], "write": [], "health": []}
    interval = {}
    scheduling_lag = []
    outstanding_limit = workers + 256
    capacity = threading.BoundedSemaphore(outstanding_limit)
    start_gate = threading.Event()
    stop_health = threading.Event()
    # Callback updates and lists remain protected across Python runtimes.
    result_lock = threading.Lock()

    def record(kind, code, latency):
        with result_lock:
            statuses[kind][code] = statuses[kind].get(code, 0) + 1
            if code == 200:
                latencies[kind].append(latency)

    def load(method, due):
        start_gate.wait()
        scheduled_late = max(0, round((time.monotonic() - due) * 1e6))
        with result_lock:
            scheduling_lag.append(scheduled_late)
        try:
            code, _, _ = request(port, method, "/probe/kv")
            record("write" if method == "POST" else "read", code,
                   round((time.monotonic() - due) * 1e6))
        finally:
            capacity.release()

    def health():
        start_gate.wait()
        while not stop_health.is_set():
            code, _, latency = request(health_port, "GET", "/healthz")
            record("health", code, latency)
            stop_health.wait(0.25)

    monitor = threading.Thread(target=health)
    monitor.start()
    try:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            start = time.monotonic()
            if rate:
                start_gate.set()
            total = reads + writes
            try:
                # In burst mode, keep workers gated until the bounded queue
                # has admitted or rejected every arrival.
                for index in range(total):
                    due = start + index / rate if rate else start
                    if rate:
                        remaining = due - time.monotonic()
                        if remaining > 0:
                            time.sleep(remaining)
                    write = (index + 1) * writes // total != index * writes // total
                    if capacity.acquire(blocking=False):
                        pool.submit(load, "POST" if write else "GET", due)
                    else:
                        record("write" if write else "read", "driver_rejected", 0)
            finally:
                interval["producer_elapsed_s"] = round(time.monotonic() - start, 3)
                start_gate.set()
        interval["elapsed_s"] = round(time.monotonic() - start, 3)
    finally:
        start_gate.set()
        stop_health.set()
        monitor.join()

    latencies["driver_start_lag"] = scheduling_lag
    for kind, values in latencies.items():
        values.sort()
        interval[f"{kind}_us"] = {
            label: values[(len(values) * rank + 999) // 1000 - 1]
            for label, rank in (("p50", 500), ("p95", 950), ("p99", 990),
                                ("p99_9", 999), ("p100", 1000))
        } if values else None
    return {**interval, "offered_rate": rate, "offered_reads": reads, "offered_writes": writes,
            "statuses": statuses, "driver_outstanding_limit": outstanding_limit}


def database_sample(port, after_ms=None):
    # /statusz may reuse the previous sample for up to a second. Require a
    # later sample when collecting the after side of a workload window.
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        code, status, _ = request(port, "GET", "/statusz")
        if code == 200:
            activity = status.get("databaseActivity")
            if activity and (after_ms is None or activity["sampleMs"] > after_ms):
                metadata = dict(status.get("database", {}))
                metadata["httpTiming"] = status.get("httpTiming")
                metadata["snapshotAuth"] = status.get("snapshotAuth")
                return activity, metadata
        time.sleep(0.15)
    return None, {}


def activity_delta(before, after, names=None):
    if not before or not after or after["sampleMs"] <= before["sampleMs"]:
        return None
    bounds = before["boundsUs"]
    if after["boundsUs"] != bounds:
        raise RuntimeError("database histogram bucket boundaries changed during the run")
    histograms = names or ("reads", "writes", "lockWait", "readHold", "writeHold", "sweepHold",
                  "compactionHold", "recoveryHold", "fileRead", "fileSync", "samplerWakeLag", "scanHold")
    report = {"sample_window_ms": after["sampleMs"] - before["sampleMs"]}
    if "peakLockWaiters" in after:
        report["peak_lock_waiters_since_boot"] = after["peakLockWaiters"]
    for name in histograms:
        if name not in before or name not in after:
            continue
        start, stop = before[name], after[name]
        if len(start) != len(bounds) + 1 or len(start) != len(stop):
            raise RuntimeError(f"invalid database histogram: {name}")
        buckets = [right - left for left, right in zip(start, stop)]
        if any(count < 0 for count in buckets):
            raise RuntimeError(f"database histogram reset during workload: {name}")
        samples = sum(buckets)
        result = {"count": samples}
        for p in (95, 99, 100):
            rank = (samples * p + 99) // 100
            quantile = None
            if rank:
                for index, count in enumerate(buckets):
                    rank -= count
                    if rank <= 0:
                        quantile = (f"<={bounds[index]}" if index < len(bounds)
                                    else f">{bounds[-1]}")
                        break
            result[f"p{p}_us"] = quantile
        report[name] = result
    return report


def measured_load(port, health, reads, writes, rate, workers):
    before, before_meta = database_sample(port)
    row = guest_load(port, health, reads, writes, rate, workers)
    after, after_meta = database_sample(port, before["sampleMs"] if before else None)
    row["database_activity_delta"] = activity_delta(before, after)
    if before and after and before_meta.get("httpTiming") and after_meta.get("httpTiming"):
        start = {"sampleMs": before["sampleMs"], **before_meta["httpTiming"]}
        stop = {"sampleMs": after["sampleMs"], **after_meta["httpTiming"]}
        row["http_timing_delta"] = activity_delta(start, stop, (
            "dispatch", "readLockWait", "readLockHold", "mutationLockWait", "mutationLockHold"))
    if before_meta.get("snapshotAuth") and after_meta.get("snapshotAuth"):
        row["snapshot_auth_delta"] = {key: value - before_meta["snapshotAuth"][key]
                                      for key, value in after_meta["snapshotAuth"].items()}
    return row, after_meta


def auth_contract(port):
    statuses = {}
    def call(name, route, *, document=None, bearer=None):
        headers = {"Accept": "application/json"}
        if bearer is not None:
            headers["Authorization"] = "Bearer " + bearer
        if document is not None:
            headers["Content-Type"] = "application/json"
        body = json.dumps(document).encode() if document is not None else None
        status, payload, latency = request(port, "POST", route, body, headers)
        statuses[name] = {"status": status, "latency_us": latency}
        return status, payload

    token = None
    try:
        code, payload = call("mint", "/v1/devices/challenge")
        if code != 200:
            raise RuntimeError(f"challenge mint returned {code}")
        enrollment = {"platform": "ios", "appVersion": "kv-benchmark",
                      "attestation": {"provider": "development", "challenge": payload["challenge"]}}
        code, payload = call("enroll", "/v1/devices", document=enrollment)
        if code != 201:
            raise RuntimeError(f"synthetic enrollment returned {code}")
        token = payload["deviceToken"]
        code, _ = call("retry_consumed_challenge", "/v1/devices", document=enrollment)
        assert code == 401, f"consumed challenge retry returned {code}"
        code, payload = call("rotate", "/v1/devices/rotate", bearer=token)
        if code != 200:
            raise RuntimeError(f"synthetic rotation returned {code}")
        old, token = token, payload["deviceToken"]
        code, _ = call("retry_old_bearer", "/v1/devices/rotate", bearer=old)
        assert code == 401, f"old bearer retry returned {code}"
        code, _ = call("revoke", "/v1/devices/revoke", bearer=token)
        assert code == 204, f"revoke returned {code}"
        code, _ = call("retry_revoked_bearer", "/v1/devices/rotate", bearer=token)
        assert code == 401, f"revoked bearer retry returned {code}"
        token = None
        return statuses
    finally:
        if token:
            request(port, "POST", "/v1/devices/revoke", headers={"Authorization": "Bearer " + token})


def driver(args):
    # This mode is for an SSH forward to the disposable guest, never a public URL.
    if args.auth_contract:
        print(json.dumps({"mode": "device-contract", "requests": auth_contract(args.port)}), flush=True)
    if not args.rates and not args.burst and args.auth_contract:
        return
    stages = [(100000, 10000, 0)] if args.burst else []
    for rate in args.rates:
        offered = rate * args.seconds
        writes = offered // 11
        stages.append((offered - writes, writes, rate))
    for reads, writes, rate in stages:
        row, database = measured_load(args.port, args.health_port, reads, writes, rate, args.workers)
        print(json.dumps({"mode": "burst" if not rate else "sustained",
                          **row, "compactions_completed": database.get("compactionsCompleted")}), flush=True)


def guest(args):
    docker("image", "inspect", args.image)
    nonce = secrets.token_hex(6)
    name = f"fortis-kv-guest-bench-{nonce}"
    volume = f"fortis-kv-guest-bench-data-{nonce}"
    storage = Path(args.storage_root).resolve() if args.storage_root else None
    if storage and (not storage.is_mount() or storage == Path("/mnt/fortis-data")):
        raise ValueError("--storage-root must be a mounted filesystem dedicated to benchmarking")
    if not args.emulated:
        live = docker("ps", "-a", "--filter", "name=^/fortis-node$",
                      "--format", "{{.Names}}").stdout.strip()
        if live:
            raise ValueError("use an independent disposable host instead of the live canary")
    if not storage:
        docker("volume", "create", volume)
    try:
        directory = (tempfile.TemporaryDirectory(prefix="fortis-kv-guest-data-", dir=storage)
                     if storage else nullcontext(None))
        with directory as data:
            print(json.dumps({"environment": "QEMU TCG" if args.emulated else "Linux KVM",
                              "storage": "isolated mounted filesystem" if data else "Docker named volume",
                              "runtime_image": args.image,
                              "guest_memory_mib": 512, "client_threads": args.workers}), flush=True)
            with guest_container(name, volume, args.image, args.emulated, data) as (port, health):
                if args.auth_contract:
                    print(json.dumps({"mode": "device-contract", "requests": auth_contract(port)}), flush=True)
                code, body, _ = request(port, "GET", "/probe/kv")
                if code != 200 or body != {"counter": 0}:
                    raise RuntimeError("isolated hot counter unavailable at boot")
                last_counter = 0
                stages = [(100000, 10000, 0)]
                for rate in args.rates:
                    offered = rate * args.seconds
                    writes = offered // 11
                    stages.append((offered - writes, writes, rate))
                for reads, writes, rate in stages:
                    row, database = measured_load(port, health, reads, writes, rate, args.workers)
                    row["compactions_completed"] = database.get("compactionsCompleted")
                    code, body, _ = request(port, "GET", "/probe/kv")
                    if code != 200:
                        raise RuntimeError("counter unavailable after workload")
                    counter = body["counter"]
                    acknowledged = row["statuses"]["write"].get(200, 0)
                    if counter - last_counter < acknowledged:
                        raise RuntimeError("acknowledged hot-counter increments were lost")
                    last_counter = counter
                    print(json.dumps({"mode": "burst" if not rate else "sustained",
                                      **row, "counter": counter}), flush=True)
                docker("kill", name)
            restart = time.monotonic()
            with guest_container(name, volume, args.image, args.emulated, data) as (port, _):
                code, body, _ = request(port, "GET", "/probe/kv")
                recovered = body.get("counter") if isinstance(body, dict) else None
                if type(recovered) is not int or recovered < last_counter:
                    raise RuntimeError(f"guest replay returned {recovered} (HTTP {code}); "
                                       f"last observed durable counter was {last_counter}")
                # A probe that timed out at the client may have committed after
                # the final health/read sample and before the kill.
                print(json.dumps({"mode": "guest-crash-replay", "counter": recovered,
                                  "counter_before_crash": last_counter,
                                  "restart_seconds": round(time.monotonic() - restart, 3)}), flush=True)
    finally:
        if not storage:
            docker("volume", "rm", volume)
            if docker("volume", "inspect", volume, check=False).returncode == 0:
                raise RuntimeError(f"could not remove disposable volume {volume}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    host_parser = sub.add_parser("host")
    host_parser.add_argument("--cxx", default=os.getenv("CXX", "c++"))
    host_parser.add_argument("--workers", type=int, default=4)
    host_parser.add_argument("--queue", type=int, default=1024)
    host_parser.add_argument("--batch", type=int, default=32)
    host_modes = host_parser.add_mutually_exclusive_group()
    host_modes.add_argument("--parallel-point-reads", action="store_true",
                            help="compare serial reads against four file-pinned point reads")
    host_modes.add_argument("--group-commit", action="store_true",
                             help="compare individual writes with and without store group commit")
    host_modes.add_argument("--commit-slots", action="store_true",
                            help="compare the rollback-marker protocol with commit slots")
    host_parser.add_argument("--hot-percent", type=int, default=80)
    host_parser.add_argument("--seconds", type=int, default=5)
    guest_parser = sub.add_parser("guest")
    guest_parser.add_argument("--image", required=True, help="locally built fortis runtime image")
    guest_parser.add_argument("--workers", type=int, default=256)
    guest_parser.add_argument("--seconds", type=int, default=5)
    guest_parser.add_argument("--rates", nargs="+", type=int, default=[100, 300],
                              help="total request rates for successive sustained stages")
    guest_parser.add_argument("--emulated", action="store_true", help="use TCG instead of KVM")
    guest_parser.add_argument("--storage-root", help="mountpoint of a disposable filesystem visible to Docker")
    guest_parser.add_argument("--auth-contract", action="store_true")
    driver_parser = sub.add_parser("driver", help="load through loopback SSH forwards to a disposable guest")
    driver_parser.add_argument("--port", type=int, required=True)
    driver_parser.add_argument("--health-port", type=int, required=True)
    driver_parser.add_argument("--workers", type=int, default=64)
    driver_parser.add_argument("--seconds", type=int, default=5)
    driver_parser.add_argument("--rates", nargs="+", type=int, default=[])
    driver_parser.add_argument("--burst", action="store_true")
    driver_parser.add_argument("--auth-contract", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.workers <= 512 or not 1 <= args.seconds <= 120:
        parser.error("workers must be 1–512 and seconds 1–120")
    if args.mode == "host":
        if not 1 <= args.queue <= 1000000 or not 2 <= args.batch <= 64 or not 0 <= args.hot_percent <= 100:
            parser.error("invalid host queue, batch or hot-key configuration")
        host(args)
    else:
        if any(not 1 <= rate <= 110000 for rate in args.rates):
            parser.error("guest rates must be 1–110,000")
        if args.mode == "guest":
            guest(args)
        else:
            if args.port == args.health_port or any(not 1 <= port <= 65535 for port in (args.port, args.health_port)):
                parser.error("driver requires distinct valid loopback ports")
            if not (args.rates or args.burst or args.auth_contract):
                parser.error("driver requires --rates, --burst or --auth-contract")
            driver(args)


if __name__ == "__main__":
    main()
