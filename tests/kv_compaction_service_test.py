#!/usr/bin/env python3
"""Automatic KV compaction inside the running service: the expiry sweeper's
maintenance thread compacts on its own (small test policy) across repeated
seven-day retention cycles on the operations clock, /metrics exposes the
counters, crash reports survive compaction byte for byte, and SIGTERM,
SIGKILL during a run, and the production binary all reopen the same volume."""
import json, os, re, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
T0 = 1767323045000
DAY = 24 * 60 * 60 * 1000
METRICS_TOKEN = "kv-compaction-metrics-token"
PERMANENT = dict(set=1, count=200, size=1024, version=1)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(port, path, method="GET", headers=None, body=b""):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    if body or method == "POST":
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
        s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
        data = bytearray()
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    return int(head.split(b" ", 2)[1]), payload


def start(binary, kv, clock, test=True):
    port = free_port()
    env = os.environ.copy()
    for name in list(env):
        if name.startswith("FORTIS_TEST_") or name.startswith("FORTIS_ENABLE_TEST"):
            env.pop(name)
    env.update(FORTIS_KV_DIR=str(kv), METRICS_TOKEN=METRICS_TOKEN)
    if test:
        env.update(FORTIS_ENABLE_TEST_OPERATIONS="1",
                   FORTIS_TEST_OPERATIONS_NOW_FILE=str(clock),
                   FORTIS_TEST_KV_SWEEP_INTERVAL_MS="50",
                   FORTIS_TEST_KV_COMPACTION_SMALL="1")
    process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE)
    for _ in range(300):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return process, port
        except OSError:
            time.sleep(0.03)
    raise RuntimeError("server failed to start")


def stop(process, sig=signal.SIGTERM):
    process.send_signal(sig)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise AssertionError("server did not stop within 10 s")
    if process.stderr:
        process.stderr.close()


def metrics(port):
    status, body = request(port, "/metrics",
                           headers={"Authorization": "Bearer " + METRICS_TOKEN})
    assert status == 200, status
    values = {}
    for line in body.decode().splitlines():
        match = re.match(r"^(fortis_kv_[a-z_]+) (\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2))
    return values


def churn(port, **query):
    path = "/probe/test-kv-churn?" + "&".join(f"{k}={v}" for k, v in query.items())
    status, body = request(port, path)
    assert status == 200 and json.loads(body)["ok"], body


def check(port, **query):
    query = {k: v for k, v in query.items() if k != "ttl_ms"}
    path = "/probe/test-kv-churn-check?" + "&".join(f"{k}={v}" for k, v in query.items())
    status, body = request(port, path)
    assert status == 200, status
    return json.loads(body)


def crash_report(port, device):
    cases = json.loads((FIXTURES / "crash_reports_route.json").read_text())["cases"]
    valid = next(case for case in cases if case["name"] == "json valid")
    headers = dict(valid["headers"])
    headers["Authorization"] = f"Bearer fintel_dev_{device:02d}" + "K" * 41
    status, body = request(port, "/v1/crash-reports", method="POST", headers=headers,
                           body=valid["body"]["value"].encode())
    assert status == 202, (status, body)
    report_id = json.loads(body)["id"]
    status, stored = request(port, f"/probe/test-crash-report/{report_id}")
    assert status == 200
    return report_id, stored


def settle(port, minimum_runs, timeout=60):
    """Waits for at least `minimum_runs` completed runs and an idle,
    not-due store (file below twice the live bytes, or below the floor)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        values = metrics(port)
        idle = values["fortis_kv_compaction_phase"] == 0
        file_bytes, live = values["fortis_kv_file_bytes"], values["fortis_kv_live_bytes"]
        if (idle and values["fortis_kv_compaction_runs_completed_total"] >= minimum_runs and
                (file_bytes < 256 * 1024 or (file_bytes - live) * 100 < file_bytes * 50)):
            return values
        time.sleep(0.05)
    raise AssertionError(f"compaction did not settle: {metrics(port)}")


def main():
    binary, production = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="fortis-kv-compaction-service-") as temp:
        kv = Path(temp) / "kv"
        kv.mkdir()
        clock = Path(temp) / "now"
        now = T0
        clock.write_text(str(now))
        process, port = start(binary, kv, clock)
        reports = {}
        try:
            churn(port, **PERMANENT)
            before = metrics(port)
            assert before["fortis_kv_compaction_runs_started_total"] == 0
            completed = 0
            for cycle in range(3):
                start_bytes = metrics(port)["fortis_kv_file_bytes"]
                peak = start_bytes
                report_id, stored = crash_report(port, cycle)
                reports[report_id] = (stored, now + 7 * DAY)
                for day in range(7):
                    version = cycle * 7 + day + 1
                    # Short-lived domains: rewritten daily with seven-day
                    # retention, plus a one-day set that expires each day.
                    churn(port, set=2, count=300, size=2048, ttl_ms=7 * DAY, version=version)
                    churn(port, set=3, count=150, size=512, ttl_ms=DAY, version=version)
                    peak = max(peak, metrics(port)["fortis_kv_file_bytes"])
                    now += DAY
                    clock.write_text(str(now))
                values = settle(port, completed + 1)
                completed = values["fortis_kv_compaction_runs_completed_total"]
                assert values["fortis_kv_compaction_failures_total"] == 0, values
                assert check(port, **PERMANENT) == {"present": 200, "valid": 200}
                assert check(port, set=2, count=300, size=2048,
                             version=cycle * 7 + 7) == {"present": 300, "valid": 300}
                assert check(port, set=3, count=150, size=512,
                             version=cycle * 7 + 7)["present"] == 0  # Expired a day ago.
                for report_id, (stored, expires) in reports.items():
                    status, body = request(port, f"/probe/test-crash-report/{report_id}")
                    if now < expires:
                        assert status == 200 and body == stored, report_id
                    else:
                        assert status == 404, report_id
                print(f"cycle {cycle + 1}: file {start_bytes} -> peak {peak} -> {values['fortis_kv_file_bytes']} "
                      f"bytes (live {values['fortis_kv_live_bytes']}), runs {completed}, "
                      f"copied {values['fortis_kv_compaction_bytes_copied_total']}, "
                      f"reclaimed {values['fortis_kv_compaction_bytes_reclaimed_total']}, "
                      f"max pause {values['fortis_kv_compaction_pause_us_max']} us, "
                      f"swap pause {values['fortis_kv_compaction_swap_pause_us_max']} us")
                assert values["fortis_kv_file_bytes"] < peak
        finally:
            stop(process)
        assert not (kv / "fortis.kv.compact").exists()

        # SIGKILL while a run is in progress, three times: every restart
        # reopens the authoritative log with the acknowledged records.
        kills = 0
        for attempt in range(12):
            process, port = start(binary, kv, clock)
            try:
                churn(port, set=2, count=300, size=2048, ttl_ms=7 * DAY, version=100 + attempt)
                deadline = time.time() + 20
                phase = 0
                while time.time() < deadline:
                    phase = metrics(port)["fortis_kv_compaction_phase"]
                    if phase:
                        break
                    churn(port, set=2, count=300, size=2048, ttl_ms=7 * DAY,
                          version=100 + attempt)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                    process.stderr.close()
            kills += 1 if phase else 0
            process, port = start(binary, kv, clock)
            try:
                assert not (kv / "fortis.kv.compact").exists()
                assert check(port, **PERMANENT) == {"present": 200, "valid": 200}
                assert check(port, set=2, count=300, size=2048,
                             version=100 + attempt) == {"present": 300, "valid": 300}
            finally:
                stop(process)
            if kills >= 3:
                break
        assert kills >= 3, kills

        # A clean SIGTERM during a run cancels between slices and removes the
        # temporary file.
        process, port = start(binary, kv, clock)
        try:
            for version in range(200, 260):
                churn(port, set=2, count=300, size=2048, ttl_ms=7 * DAY, version=version)
                if metrics(port)["fortis_kv_compaction_phase"]:
                    break
            started = time.time()
        finally:
            stop(process)
        assert time.time() - started < 5
        assert not (kv / "fortis.kv.compact").exists()

        # The production binary (default policy: no compaction within five
        # minutes of start) serves the compacted volume.
        process, port = start(production, kv, clock, test=False)
        try:
            assert request(port, "/healthz")[0] == 200
            values = metrics(port)
            assert values["fortis_kv_compaction_runs_started_total"] == 0
        finally:
            stop(process)
    print(f"kv compaction service: 3 seven-day cycles, {kills} mid-run SIGKILLs, "
          "SIGTERM cancellation and production reopen OK")


if __name__ == "__main__":
    main()
