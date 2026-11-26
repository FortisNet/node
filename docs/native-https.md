# Native HTTPS ingress

The node can terminate TLS itself, without a reverse proxy. This document describes the built-in HTTPS listener: its design, its limits, how to configure it, how certificates are supplied and rotated, and how to test it. Operators who expose a node directly to the internet and developers who change `src/app/https_listener.cpp` need it.

On a host built from [FortisNet Node OS](https://github.com/FortisNet/node-os), the orchestrator obtains and renews the certificate, opens the public ports and writes this configuration. The sections below describe the node's side of that interface.

## Design

`HttpsListener` ([src/app/https_listener.h](../src/app/https_listener.h)) is a single nonblocking reactor thread. It owns every public socket and TLS context.

- **Handshakes and uploads stay in the reactor.** The reactor performs TLS handshakes and accumulates each request until it is complete and admitted. Only then does it queue the request for the four application workers. A slow handshake, a slow upload or an idle keep-alive connection never occupies a worker.
- **Fairness.** Each reactor turn has a byte budget and an accept budget, so one busy connection cannot starve the others.
- **Health bypass.** A complete `/healthz` request is dispatched through a separate health-only router. It does not use the application workers, the route lock or the admission queue.
- **One parser.** The reactor uses the same `RequestParser` as the plain listener. Its `Head` entry point validates the request line and headers before any body memory is allocated.
- **Strict paths.** The reactor decodes the path once. It refuses control characters, backslashes, dot segments, repeated slashes, leftover percent escapes and encoded `?` or `#`. It then refuses the whole `/probe` namespace. The original target is passed on unchanged; the reactor never rewrites a request into a different route.
- **Ordered requests.** A connection serves one request at a time. Pipelined bytes are kept within the header bound and dispatched in order. A reserved response completes before the next request starts. Malformed framing closes the connection.
- **Low latency.** Accepted sockets use `TCP_NODELAY`, so TLS records of one response are not delayed by the peer's delayed acknowledgment.
- **Date header.** The response writer adds a UTC `Date` header, as [RFC 9110 section 6.6.1](https://www.rfc-editor.org/rfc/rfc9110#name-date) requires of an origin server.

### Ownership

`HttpTransport` ([src/http/http_transport.h](../src/http/http_transport.h)) is a small I/O interface registered by file descriptor in a bounded table. The reactor owns the socket and the TLS context. A worker or a response writer borrows the transport exclusively while it handles the request. `CloseHttpConnection` returns a TLS connection to the reactor instead of destroying it. No two threads read or write TLS records on one context at the same time. The node never peeks at the raw socket of a TLS connection: deadlines and the final write detect a disconnected client.

On shutdown the node interrupts the sockets, then joins the workers, the service owners and the response handoff, and only then frees the reactor.

## Behavior and limits

The constants are in [src/app/https_listener_internal.h](../src/app/https_listener_internal.h).

| Subject | Policy |
| --- | --- |
| TLS | Mbed TLS in server mode. TLS 1.2 minimum, TLS 1.3 supported. Forward-secret AEAD cipher suites only. No renegotiation. Session tickets disabled |
| Certificate selection | SNI equal to the configured domain selects the domain certificate. The address certificate is the default. Any other SNI is refused |
| ALPN | `http/1.1` only. HTTP/2 is not offered |
| Address family | IPv4 |
| Connections | 256 in total, 224 per client address |
| Handshakes | 32 at a time. Others wait, oldest first, inside the 256-connection limit. Three seconds from accept to completion, including waiting time |
| Request size | 16 KiB of headers, 512 KiB body. Five seconds to receive the complete request |
| Uploads | 32 requests with a body at a time, 8 per client address, admitted before body memory is allocated. Over the limit: `503` |
| Request rate | 100 requests a second per client address with a burst of 400. Over the limit: `429`. `/healthz` is exempt |
| Rate state | 512 client entries. An entry idle for 60 seconds can be reused |
| Keep-alive | 15 seconds idle, at most 100 requests per connection |
| Application requests | At most 224 admitted at a time, then the existing queue of 256 and four workers |
| Framing | `Content-Length` only. `Transfer-Encoding` is refused. `Expect` is refused with `417`. A body on `GET`, `HEAD` or `OPTIONS` is refused with `405` |
| Host | The configured domain or address, optionally with a numeric port. Anything else: `421` |
| Reactor writes | Health, error and redirect replies written by the reactor never wait for the socket to become writable |

These limits bound resource use. They do not guarantee that a new client can connect while all handshake or connection slots are in use. Connections that are already established and the separate health port keep working in that case.

Error bodies and logs contain no credentials and no request payloads. Once a minute the reactor logs totals of accepted and rejected connections, failed handshakes, expired connections and protocol refusals. It records no client addresses.

### The HTTP port

A second listener (`FORTIS_HTTP_REDIRECT_PORT`, default 8082) accepts plain HTTP. It never dispatches API requests. It does two things:

- For `GET` or `HEAD` under `/.well-known/acme-challenge/`, it serves the named file from `FORTIS_ACME_CHALLENGE_DIR`. The token may contain only letters, digits, `_` and `-`, at most 128 characters. The file must be a regular file of at most 4,096 bytes and must not be a symbolic link.
- For every other request it answers `308` with the same target on `https://` and the configured domain, or the address when no domain is configured.

## Configuration

The listener is enabled when `FORTIS_HTTPS_CERT_DIR` is set. An invalid configuration stops startup.

| Variable | Meaning |
| --- | --- |
| `FORTIS_HTTPS_CERT_DIR` | Absolute path of the certificate directory (see below). Setting it enables HTTPS on the main port |
| `FORTIS_HTTPS_IP` | The node's public IPv4 address. Required. Used for the default certificate and the `Host` check |
| `FORTIS_HTTPS_DOMAIN` | The node's DNS name, lower case. Optional. Without it the node serves only the address certificate |
| `FORTIS_HTTP_REDIRECT_PORT` | Port of the plain HTTP listener. Default 8082 |
| `FORTIS_ACME_CHALLENGE_DIR` | Directory of ACME HTTP-01 challenge files. Optional |
| `FORTIS_HEALTH_PORT` | Port of a separate plain `/healthz` listener for the supervisor. Optional. Do not publish it |
| `FORTIS_CA_BUNDLE_PATH` | CA bundle used to verify the node's own certificates at load time. Default `/etc/ssl/certs/ca-certificates.crt` |
| `FORTIS_LISTEN_FDS` | `PORT=FD,...`: listeners that the host has already opened. When set, the node binds nothing itself |

The main port is the program's port argument or `PORT`. Example for a node reachable as `node.example.org` at `203.0.113.10`, with the host mapping public ports 443 and 80 to 8080 and 8082:

```sh
FORTIS_HTTPS_CERT_DIR=/certificates
FORTIS_HTTPS_DOMAIN=node.example.org
FORTIS_HTTPS_IP=203.0.113.10
FORTIS_HTTP_REDIRECT_PORT=8082
FORTIS_ACME_CHALLENGE_DIR=/acme
FORTIS_HEALTH_PORT=8081
```

## Certificate layout and reload

The host supplies one directory:

```
/certificates/current -> generation-...
/certificates/generation-.../domain.pem
/certificates/generation-.../domain.key
/certificates/generation-.../ip.pem
/certificates/generation-.../ip.key
```

`domain.pem` and `domain.key` are needed only when `FORTIS_HTTPS_DOMAIN` is set. `current` is a symbolic link to one generation directory. The node's user must be able to read the files. Each file is at most 64 KiB. A private key must not be readable by group or others. Supply the directory as a whole, not individual files, so that a new generation becomes visible without restarting the node.

The node loads certificates at startup and on `SIGHUP`:

1. It opens `current` once and reads all files relative to that one directory. A switch of the link during loading cannot combine a new certificate with an old key.
2. It refuses symbolic links and files that are not regular files.
3. It parses each chain and key, checks that they match, verifies the chain against the CA bundle for the configured name or address, and checks that the certificate allows server authentication.
4. If everything is valid, the new generation becomes active for new connections at once. Existing connections keep the generation they started with.
5. If anything is invalid, the node keeps the generation it has and logs `Certificate reload refused; retaining active generation`.

A successful reload logs `Certificate generation activated`. Changing the link without sending `SIGHUP` activates nothing. At most four generations are alive at a time; a reload beyond that is refused until old connections finish, and can be retried.

To rotate a certificate: write a new generation directory, switch `current` to it atomically, send `SIGHUP`, and check the log line. Keep the previous generation until the new one is confirmed. Obtaining and renewing certificates is the host's job.

## Development enrollment

Development enrollment is accepted only from loopback and private addresses. Behind a reverse proxy on the same machine every client appears local. With built-in HTTPS the node sees the real client address, so a development enrollment from a public address is refused with `DEVELOPMENT_ATTESTATION_LOCAL_ONLY`. This is intended. Do not work around it by trusting forwarding headers or by allowing development attestation from public addresses.

To run the load and probe tools against a remote development node, forward a local port to the node's HTTPS port over SSH and pass it to the tools. Only the enrollment connections use the tunnel. TLS still verifies the server's real name, and measured requests use the public address directly.

```sh
ssh -N -L 9443:127.0.0.1:443 USER@ADDRESS
python3 tests/public_price_device_probe.py --base-url https://node.example.org --enrollment-tunnel-port 9443
python3 tests/portfolio_load_test.py --base-url https://node.example.org --allow-remote \
  --enrollment-tunnel-port 9443 --users 100 --mode mixed --require-snapshot \
  --warmup --rounds 3 --interval 30
```

## Testing

```sh
make https-unit-tests        # listener, TLS and transport unit tests
make https-unit-coverage     # the same tests with a coverage report
python3 tests/incoming_https_test.py build/fortis_node --handoff-binary build/incoming_https_fixture
./build/http_boundary_fuzz_test
```

[native-https-unit-tests.md](native-https-unit-tests.md) describes what the unit tests cover. `tests/incoming_https_test.py` starts the real program with a generated CA and exercises handshakes, limits, redirects, challenge files and certificate reload over real sockets.

To run the integration and fuzz tests under AddressSanitizer:

```sh
python3 scripts/sanitize.py asan build build/fortis_node build/incoming_https_fixture build/http_boundary_fuzz_test
python3 scripts/sanitize.py asan run python3 tests/incoming_https_test.py build/fortis_node --handoff-binary build/incoming_https_fixture
python3 scripts/sanitize.py asan run ./build/http_boundary_fuzz_test
```

`http_boundary_fuzz_test` runs a fixed mutation corpus against the request parser and needs no network. The same file exports `LLVMFuzzerTestOneInput`; compile it with `FORTIS_LIBFUZZER` and Clang's fuzzer, address and undefined-behavior sanitizers for coverage-guided fuzzing.

These tests run on one machine with generated certificates. They do not measure real provider behavior or mobile network performance.
