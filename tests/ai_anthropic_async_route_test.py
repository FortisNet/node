#!/usr/bin/env python3
"""Anthropic async route against local TLS; no real credentials."""
from concurrent.futures import ThreadPoolExecutor
import json
import secrets
import sys
import tempfile
import time
from pathlib import Path

import ai_conversations_route_test as replay
import ai_conversations_native_test as native
import discovery_route_harness as harness


def answer(text):
    return {"json": {"type": "message", "stop_reason": "end_turn",
                     "content": [{"type": "text", "text": text}],
                     "usage": {"input_tokens": 11, "output_tokens": 5,
                               "cache_creation_input_tokens": 2,
                               "cache_read_input_tokens": 3}}}


def wait_status(server, thread, token, wanted):
    for _ in range(100):
        code, headers, body, raw = native.call(
            server, "GET", f"/v1/ai/conversations/{thread}/messages?limit=20", chat=token)
        assert code == 200, (code, raw)
        if headers.get("x-ai-reply-status") == wanted:
            return headers, body["messages"]
        time.sleep(.1)
    raise AssertionError(f"reply status never became {wanted}; last={headers}; rows={body}")


def main(binary):
    with tempfile.TemporaryDirectory(prefix="fortis-ai-anthropic-") as directory:
        root = Path(directory)
        (root / "kv").mkdir()
        (root / "tls").mkdir()
        upstream = replay.OpenAi(root / "tls")
        key = root / "anthropic-key"
        key.write_text("sk-local-fixture-never-a-real-credential\n")
        key.chmod(0o600)
        harness.set_clock(root / "clock", native.NOW)
        fixture = {"env": {"SUPPORT_AUTH_SECRET": native.SECRET}}
        config = {"AI_PROVIDER": "anthropic", "AI_MODEL": "claude-opus-4-6",
                  "AI_EFFORT": "low", "AI_MAX_OUTPUT_TOKENS": "2048",
                  "AI_TIMEOUT_MS": "3000", "AI_CONCURRENCY": "2",
                  "ANTHROPIC_API_KEY_PATH": str(key)}
        upstream.default = answer("First reply")
        server = replay.Server(binary, root, fixture, upstream, extra_env=config)
        try:
            health_code, _, health, _ = native.call(server, "GET", "/healthz")
            assert health_code == 200 and health["supportedModels"] == ["claude-opus-4-6"]
            request = native.open_request(native.NOW)
            status, _, opened, raw = native.call(server, "POST", "/v1/ai/conversations", request)
            assert status == 201, (status, raw)
            thread, token = opened["id"], request["accessToken"]
            _, rows = wait_status(server, thread, token, "complete")
            assert rows[-1]["role"] == "assistant" and rows[-1]["body"] == "First reply"
            stats_code, _, _, stats_page = native.call(server, "GET", "/ai/stats")
            assert stats_code == 200 and b"AI statistics" in stats_page
            assert b'id="ai-chart"' in stats_page and b'data-metric="messages"' in stats_page
            assert b'const aiHours=[' in stats_page and b'const aiDays=[' in stats_page
            assert b"<td>All recorded</td><td>1</td><td>1</td><td>1</td><td>16</td><td>5</td><td>1</td>" in stats_page
            calls = upstream.calls()
            assert len(calls) == 1 and calls[0]["path"] == "/v1/messages"
            body = json.loads(calls[0]["body"])
            assert body["output_config"] == {"effort": "low"}
            assert body["model"] == "claude-opus-4-6" and body["max_tokens"] == 2048
            assert len(body["messages"]) == 2 and body["messages"][-1]["role"] == "user"
            assert calls[0]["headers"]["x-api-key"].startswith("sk-local-fixture")
            assert calls[0]["headers"]["anthropic-version"] == "2023-06-01"

            upstream.load([{"status": 503, "json": {"error": "fixture outage"}}])
            status, _, posted, raw = native.post(server, thread, token, secrets.token_hex(16), "Retry me")
            assert status == 201, (status, raw)
            failed_id = posted["message"]["id"]
            headers, rows = wait_status(server, thread, token, "unavailable")
            assert headers["x-ai-reply-for"] == failed_id and rows[-1]["role"] == "user"
            upstream.default = answer("Recovered reply")
            status, _, _, raw = native.call(
                server, "POST", f"/v1/ai/conversations/{thread}/messages/{failed_id}/retry", chat=token)
            assert status == 202, (status, raw)
            _, rows = wait_status(server, thread, token, "complete")
            assert rows[-1]["body"] == "Recovered reply"
            assert sum(row["body"] == "Retry me" for row in rows) == 1
            _, _, _, stats_page = native.call(server, "GET", "/ai/stats")
            assert b"<td>All recorded</td><td>1</td><td>2</td><td>2</td><td>32</td><td>10</td><td>2</td>" in stats_page

            before_restart = len(upstream.calls())
            upstream.load([{"timeout": True, "stallMs": 5000}])
            status, _, posted, raw = native.post(server, thread, token, secrets.token_hex(16), "Restart me")
            assert status == 201, (status, raw)
            pending_id = posted["message"]["id"]
            wait_status(server, thread, token, "running")
            for _ in range(100):
                if len(upstream.calls()) > before_restart:
                    break
                time.sleep(.05)
            assert len(upstream.calls()) > before_restart, "provider request started before crash"
            server.kill()
            upstream.load([answer("After restart")])
            upstream.default = answer("After restart")
            server = replay.Server(binary, root, fixture, upstream, extra_env=config)
            _, rows = wait_status(server, thread, token, "complete")
            assert rows[-1]["body"] == "After restart"
            assert sum(row["id"] == pending_id for row in rows) == 1
            _, _, _, stats_page = native.call(server, "GET", "/ai/stats")
            assert b"<td>All recorded</td><td>1</td><td>3</td><td>3</td><td>48</td><td>15</td><td>3</td>" in stats_page
            harness.set_clock(root / "clock", native.NOW + 3600000)
            status, _, _, raw = native.post(server, thread, token, secrets.token_hex(16), "Next hour")
            assert status == 201, (status, raw)
            _, rows = wait_status(server, thread, token, "complete")
            assert rows[-1]["role"] == "assistant"
            _, _, _, stats_page = native.call(server, "GET", "/ai/stats")
            assert b"<td>All recorded</td><td>1</td><td>4</td><td>4</td><td>64</td><td>20</td><td>4</td>" in stats_page
            assert b"2026-03-01 01:00</td><td>1</td><td>1</td><td>1</td><td>16</td><td>5</td><td>1</td>" in stats_page
            assert b"2026-03-01 00:00</td><td>1</td><td>3</td><td>3</td><td>48</td><td>15</td><td>3</td>" in stats_page
            failures = [
                (401, "authentication_error", "auth_failed"),
                (402, "billing_error", "no_credits"),
                (403, "permission_error", "auth_failed"),
                (404, "not_found_error", "model_missing"),
                (429, "rate_limit_error", "rate_limited"),
                (503, "api_error", "unavailable"),
                (400, "invalid_request_error", "failed"),
            ]
            for code, kind, expected in failures:
                upstream.load([{"status": code, "json": {
                    "type": "error", "error": {"type": kind, "message": "fixture provider failure"}}}])
                status, _, posted, raw = native.post(
                    server, thread, token, secrets.token_hex(16), f"Failure {code}")
                assert status == 201, (status, raw)
                message_id = posted["message"]["id"]
                headers, rows = wait_status(server, thread, token, expected)
                assert headers["x-ai-reply-for"] == message_id and rows[-1]["role"] == "user"
                upstream.default = answer(f"Recovered {code}")
                status, _, _, raw = native.call(
                    server, "POST", f"/v1/ai/conversations/{thread}/messages/{message_id}/retry", chat=token)
                assert status == 202, (status, raw)
                _, rows = wait_status(server, thread, token, "complete")
                assert rows[-1]["body"] == f"Recovered {code}"
                assert sum(row["id"] == message_id for row in rows) == 1

            upstream.load([{"status": 400, "json": {
                "type": "error", "error": {"type": "invalid_request_error",
                    "message": "You have reached your specified workspace API usage limits"}}}])
            status, _, posted, raw = native.post(
                server, thread, token, secrets.token_hex(16), "Spend cap")
            assert status == 201, (status, raw)
            headers, rows = wait_status(server, thread, token, "no_credits")
            assert headers["x-ai-reply-for"] == posted["message"]["id"]
            assert rows[-1]["role"] == "user"

            upstream.load([{"status": 200, "json": {
                "type": "message", "stop_reason": "max_tokens",
                "content": [{"type": "text", "text": "partial reply"}]}}])
            status, _, posted, raw = native.post(
                server, thread, token, secrets.token_hex(16), "Too long")
            assert status == 201, (status, raw)
            headers, rows = wait_status(server, thread, token, "token_limit")
            assert headers["x-ai-reply-for"] == posted["message"]["id"]
            assert rows[-1]["role"] == "user", "partial reply must not appear as complete"
            # Saved-turn acknowledgements and history reads remain usable while
            # multiple durable jobs are awaiting the provider.
            upstream.default = answer("Concurrent reply")
            messages = [f"Concurrent turn {index}" for index in range(8)]
            with ThreadPoolExecutor(max_workers=8) as pool:
                accepted = list(pool.map(lambda text: native.post(
                    server, thread, token, secrets.token_hex(16), text), messages))
                assert all(result[0] == 201 for result in accepted)
                reads = list(pool.map(lambda _: native.call(
                    server, "GET", f"/v1/ai/conversations/{thread}/messages?limit=100", chat=token), range(8)))
                assert all(result[0] == 200 for result in reads)
            for _ in range(100):
                _, _, history, _ = native.call(
                    server, "GET", f"/v1/ai/conversations/{thread}/messages?limit=100", chat=token)
                if sum(row["body"] == "Concurrent reply" for row in history["messages"]) == 8:
                    break
                time.sleep(.1)
            assert all(sum(row["body"] == text for row in history["messages"]) == 1 for text in messages)
            assert sum(row["body"] == "Concurrent reply" for row in history["messages"]) == 8
            for accept in ("application/json", "application/protobuf"):
                assert native.call(server, "HEAD", f"/v1/ai/conversations/{thread}/messages",
                                   chat=token, headers={"Accept": accept})[0] == 200
                assert native.call(server, "GET", f"/v1/ai/conversations/{thread}/messages",
                                   chat="fintel_chat_" + "A" * 43, headers={"Accept": accept})[0] == 404
            upstream.load([{"status": 200, "json": {"private": "malformed provider payload"}}])
            status, _, posted, _ = native.post(server, thread, token, secrets.token_hex(16), "Malformed reply")
            assert status == 201
            wait_status(server, thread, token, "invalid_reply")
            # Public failures are classified, never echoed from provider bodies.
            time.sleep(5.2)
            _, _, board, _ = native.call(server, "GET", "/errorsz")
            records = board["operational"]["records"]
            assert any(row["service"] == "ai" and row["code"] == "invalid_reply" for row in records)
            assert "malformed provider payload" not in json.dumps(board)
            assert key.read_text().strip() not in json.dumps(board)
        finally:
            server.stop()
            upstream.close()
        logs = (root / "stderr.log").read_text() + (root / "stdout.log").read_text()
        assert key.read_text().strip() not in logs
        assert "Retry me" not in logs and "Restart me" not in logs
    print("ai_anthropic_async_route_test: reply, retry, restart and redaction passed")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        raise SystemExit(2)
    main(sys.argv[1])
