#!/usr/bin/env python3
"""Save-only AI thread, owner recovery, and long-lived paged history."""
import json
import sys
import tempfile
from pathlib import Path

import ai_conversations_route_test as replay
import ai_open_route_test as op
import discovery_route_harness as harness


def require(condition, label):
    if not condition:
        raise AssertionError(label)


def main(binary):
    fixture = replay.load_fixture(replay.FIXTURE)
    op.DEVICES.update(fixture["devices"])
    now = fixture["now"]
    with tempfile.TemporaryDirectory(prefix="fortis-ai-durable-") as directory:
        root = Path(directory)
        (root / "kv").mkdir()
        (root / "tls").mkdir()
        upstream = replay.OpenAi(root / "tls")
        harness.set_clock(root / "clock", now)
        server = replay.Server(binary, root, fixture, upstream)
        try:
            first = op.make_open(now, "durable-first", device="A")
            status, _, payload = op.call(server, first)
            opened = json.loads(payload)
            require(status == 201 and "expiresAt" not in opened and "assistant" not in opened,
                    "opening saves the user turn without an AI reply or expiry")
            thread = opened["id"]
            token = first["accessToken"]
            path = f"/v1/ai/conversations/{thread}/messages"
            snapshot = op.snapshot_json(now - 60_000)
            message = {"clientMessageId": op.sha("durable-second")[:32],
                       "body": "Next question", "snapshotJson": snapshot,
                       "snapshotHash": op.sha(snapshot)}
            status, _, payload = op.call(server, message, path=path,
                                         extra=(f"X-Intelligence-Authorization: Bearer {token}",))
            saved = json.loads(payload)
            require(status == 201 and saved["message"]["body"] == "Next question" and
                    saved["message"]["portfolioSnapshot"]["wallets"] and
                    "assistant" not in saved, "posting saves one user turn")
            require(len(op.history(server, thread, token)) == 2, "both turns are in history")

            # Restart and move beyond the former seven-day limit. A fresh
            # wallet proof recovers the same thread with a rotated credential.
            server.stop()
            harness.set_clock(root / "clock", now + 365 * 24 * 3600 * 1000)
            server = replay.Server(binary, root, fixture, upstream)
            require(len(op.history(server, thread, token)) == 2, "history survives restart and a year")
            later = op.make_open(now + 365 * 24 * 3600 * 1000, "durable-recover", device="B")
            status, _, payload = op.call(server, later, device="B")
            recovered = json.loads(payload)
            require(status == 201 and recovered["id"] == thread and "assistant" not in recovered,
                    "same wallet proof recovers the same thread")
            require(op.history(server, thread, token) is None, "old credential is revoked")
            new_token = later["accessToken"]
            require(len(op.history(server, thread, new_token, "B")) == 3,
                    "recovered history includes the new question")
            snapshot = op.snapshot_json(now + 365 * 24 * 3600 * 1000 - 60_000)
            for index in range(23):
                turn = {"clientMessageId": op.sha(f"durable-page-{index}")[:32],
                        "body": f"Question {index}", "snapshotJson": snapshot,
                        "snapshotHash": op.sha(snapshot)}
                status, _, payload = op.call(
                    server, turn, device="B", path=path, accept="application/protobuf",
                    extra=(f"X-Intelligence-Authorization: Bearer {new_token}",))
                fields = op.wire_fields(payload)
                require(status == 201 and set(fields) == {1},
                        "protobuf acknowledgement contains the user message only")
            headers = [f"Authorization: Bearer {op.TOKENS['B']}",
                       f"X-Intelligence-Authorization: Bearer {new_token}"]
            status, first_headers, payload = op.raw(
                server, [f"GET {path}?limit=20 HTTP/1.1", "Host: localhost", *headers])
            latest = json.loads(payload)["messages"]
            cursor = first_headers.get("x-next-cursor")
            require(status == 200 and len(latest) == 20 and cursor == latest[0]["id"],
                    "first history page exposes an exclusive cursor")
            status, next_headers, payload = op.raw(
                server, [f"GET {path}?limit=20&before={cursor} HTTP/1.1",
                         "Host: localhost", *headers])
            earlier = json.loads(payload)["messages"]
            require(status == 200 and len(earlier) == 6 and
                    earlier[-1]["id"] < latest[0]["id"] and
                    "x-next-cursor" not in next_headers,
                    "older page reaches the beginning without overlap")

            # Shared request query contract: percent decoding, first value,
            # strict decimal bounds and validation of unknown/later fields.
            for query, expected, count in (
                    ("%6cimit=%32%30&limit=1", 200, 20),
                    (f"limit=20&before={cursor}&before=invalid", 200, 6),
                    (f"before=%{ord(cursor[0]):02X}{cursor[1:]}&limit=20", 200, 6),
                    ("limit=1&unused=a+b%2Bc", 200, 1),
                    ("limit=0x10", 400, None), ("limit=1e1", 400, None),
                    ("limit=101", 400, None), ("limit=18446744073709551616", 400, None),
                    ("limit=20&unknown=%GG", 400, None),
                    ("limit=20&limit=%FF", 400, None),
                    ("limit=20&%GG=ignored", 400, None)):
                status, _, payload = op.raw(
                    server, [f"GET {path}?{query} HTTP/1.1", "Host: localhost", *headers])
                require(status == expected, f"history query {query}: {status}")
                if count is not None:
                    require(len(json.loads(payload)["messages"]) == count,
                            f"history query {query}: row count")
                else:
                    require(payload == b'{"error":"BAD_REQUEST"}', "query refusal body")

            server_now = now + 365 * 24 * 3600 * 1000
            for offset, expected in ((-300_001, 401), (-300_000, 201),
                                     (300_000, 201), (300_001, 401)):
                signed = op.make_open(server_now, f"clock-skew-{offset}", device="B",
                                      timestamp=op.iso(server_now + offset))
                status, _, body = op.call(server, device="B", body=op.proto_open(signed),
                                          content_type="application/protobuf",
                                          accept="application/protobuf")
                require(status == expected, f"signed timestamp offset {offset} ms: HTTP {status}")
                if expected == 401:
                    require(json.loads(body)["error"] == "PROOF_INVALID",
                            "out-of-window timestamps fail proof validation")
            require(not upstream.calls(), "save-only flow never calls the AI provider")
        finally:
            server.stop()
            upstream.close()
    print("ai_durable_thread_route_test: save, recovery, restart, and timestamp boundaries passed")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: ai_durable_thread_route_test.py BINARY")
    main(sys.argv[1])
