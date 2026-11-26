# Device route contract and benchmark profile

This document states the externally visible behavior of the device routes: challenge, enrollment, rotation and the authorization that every protected route applies. It also proposes a benchmark workload for these routes. Developers who change `src/auth` and authors of clients and load tests need it.

The contract is recorded as request and response pairs in `tests/corpus/device_routes.json`, with its SHA-256 in `tests/corpus/device_routes.json.sha256`. The fixture was captured with a fixed clock (`2026-01-02T03:04:05.000Z`) and with fixed placeholders in place of random challenges, tokens and device IDs. `tests/smoke.py` replays it against the program. Do not edit the fixture to make a failing test pass; a difference is either a regression or a deliberate contract change that clients must learn about.

## Route behavior

| Route or condition | Behavior |
| --- | --- |
| `POST /v1/devices/challenge` | `200`. The challenge is valid for 300 seconds and is stored with that lifetime. JSON by default, protobuf when `Accept` selects it |
| `POST /v1/devices` | `201`. `platform` is one of `ios`, `android`, `macos`, `windows`, `linux`. Optional fields are bounded as listed below |
| `POST /v1/devices/rotate` | Authenticated. `200` with a new bearer token and its expiry. The previous token stops working at once. JSON or protobuf |
| `POST /v1/devices/revoke` | Authenticated. The token stops working at once |
| Missing, malformed or unknown bearer token | `401` `UNAUTHORIZED` |
| Expired or revoked token | `401` `SESSION_EXPIRED`. A token replaced by rotation is unknown and gets `UNAUTHORIZED` |
| Any `Origin` header on a `/v1` route | `403` `BROWSER_ORIGIN_FORBIDDEN`, including the public device routes |
| Development enrollment from an address that is not local | `403` `DEVELOPMENT_ATTESTATION_LOCAL_ONLY` |
| Attestation required for the platform and none supplied | `401` `ATTESTATION_REQUIRED`. No development token is issued |
| Unsupported or invalid attestation | `401` `ATTESTATION_INVALID` |
| Missing, used or expired challenge | `401` `ATTESTATION_EXPIRED`. A challenge can be used once |
| Invalid enrollment body | `400` `BAD_REQUEST` |
| Address quota on the public routes | 10 requests per address in a fixed 60-second window, shared by challenge and enrollment. The eleventh gets `429` `RATE_LIMITED` with `Retry-After: 60` |
| Device quota | Cost units per device in a fixed 60-second window. Over the limit: `429` `RATE_LIMITED` with retry and rate-limit headers |

Addresses that count as local for development enrollment: `127.0.0.1`, `::1`, the private IPv4 ranges `10.0.0.0/8`, `172.16.0.0/12` and `192.168.0.0/16`, IPv6 addresses that begin with `fc`, `fd` or `fe80`, and IPv4-mapped IPv6 forms of the IPv4 addresses above. The node takes the address from the connection, never from a header.

A failure of a quota counter does not refuse the request: the address and device quotas fail open.

## Bounds on input and stored state

| Input or state | Bound |
| --- | --- |
| Request body | 512 KiB |
| `platform` | Required; one of the five values above |
| `appVersion` | Optional; at most 50 UTF-16 code units |
| `attestation.provider` | `development`, `apple-app-attest` or `play-integrity` |
| `attestation.challenge` | Required when attestation is supplied; 16 to 200 UTF-16 code units |
| `attestation.token` | Optional; at most 32,000 UTF-16 code units |
| Challenge | Stored for five minutes; consumed in the same storage frame that creates the device |
| Device token | `fintel_dev_` followed by 32 random bytes in base64url, 54 characters in total. Only its SHA-256 is stored. Lifetime 3,600 seconds by default; `DEVICE_TOKEN_TTL_SECONDS` accepts 300 to 3,600 |
| Device record | Kept until seven days after the token expires |
| Quota tiers | Units per minute: standard 120, development 240, attested 600, premium 1,200 |

An ordinary read such as `GET /v1/networks` costs one unit, as does a rotation.

Whether attestation is required is set per platform by `ATTESTATION_MODE` and the `FORTIS_ANDROID_ATTESTATION_REQUIRED`, `FORTIS_APPLE_ATTESTATION_REQUIRED` and `FORTIS_DESKTOP_ATTESTATION_REQUIRED` variables ([src/config/native_config.h](../src/config/native_config.h)). A client-supplied provider name alone never makes a device attested.

## Proposed benchmark workload

These are inputs for a benchmark of the device routes. They are not measured usage and not a model of the whole service.

- 1,000 active device records, 32 concurrent clients, one node.
- A two-minute warm-up, then ten minutes at 1,000 requests a minute.
- Per 1,000 requests: 900 authenticated `GET /v1/networks`, 40 rotations, 30 challenges and 30 enrollments (15 JSON, 15 protobuf).
- Spread the challenge and enrollment requests over at least six client addresses, so that the 10-per-minute address quota is not the limit being measured.
- Use the development tier and keep each device below 240 units a minute. Test the quota boundary separately with 240 and 241 requests.
- Typical bodies: empty for challenge and rotation, under 128 bytes for enrollment without attestation, under 256 bytes with it. Test the maximum input (a 32,000-unit token and a body at the 512 KiB limit) separately from the steady mix.
- The device routes call no provider, so provider latency plays no part. This profile says nothing about prices, news, addresses or chain data.
- Suggested targets, to be confirmed against real traffic: p95 at most 100 ms, p99 at most 250 ms, unexpected 5xx at most 0.1%. Report expected 4xx and 429 answers separately.
- Report the CPU model and quota, memory use, latency percentiles and error classes of each run.

No baseline for this profile has been recorded on Linux x86-64 hardware. The profile should be compared with real request telemetry before it is called representative.
