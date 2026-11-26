# Request, stack and ingress budgets

This document lists the fixed limits that bound the node's use of memory, threads and time, and describes how a release build checks stack frames. Developers who add a route, a buffer or a thread need it, as do operators who size a host.

The limits make the node's behavior under load predictable: when a budget is used up, the node refuses new work with an explicit status. They are not a defense against volumetric network attacks, which must be handled before traffic reaches the node.

## Enforced limits

| Boundary | Limit and behavior when it is reached |
| --- | --- |
| Request | 16 KiB of headers, 512 KiB body, 12,500-byte target. On a plain connection the whole request must arrive within three seconds. Oversized headers: `431`. Oversized body: `413`. Failed allocation: `503` |
| Request encoding | A request body with a content encoding other than identity is refused with `415` before the body is allocated. Duplicate encoding headers: `400`. Ambiguous combinations of `Transfer-Encoding` and `Content-Length` are refused |
| HTTP admission | Four workers and 256 pending requests. A request that has waited one second is refused. Full or expired queue: `503`. The health listener has its own thread and socket queue |
| HTTPS ingress | 256 connections, 32 concurrent handshakes, 32 concurrent uploads and 100 requests a second per client address. See [native-https.md](native-https.md#behavior-and-limits) |
| Response handoff | 16 reservations. A transferred socket has exactly one final owner. A partly written response is closed; it is never followed by a second response |
| Captured response | 2 MiB per response from a handler that runs under the route lock |
| Spot price query | At most 100 IDs and 4,000 UTF-16 code units, plus per-ID bounds. A valid query can be split into several provider requests because provider URLs are bounded too |
| Provider adapter | Eight jobs, two TLS workers, 256 KiB request bodies, response bodies of 256 KiB by default and 512 KiB at most, three attempts at most and 120 seconds in total. Retries count against the same deadline and lane |
| Outbound HTTPS client | Four concurrent DNS resolutions, eight addresses per name, requests of at most 2 MiB and responses of at most 4 MiB, with lower limits per operation. A response is never accumulated without bound, and redirects are not followed |
| AI work | One to four job workers (`AI_CONCURRENCY`). Each has a work arena that grows on demand up to 64 MiB. The ceiling is not reserved in advance |

The header limit includes headers added by anything in front of the node. Behind a proxy, a client can therefore send slightly less than 16 KiB.

A `GET`, `HEAD` or `OPTIONS` request with a body is refused with `405`.

The node takes the client address from the connection. It does not read forwarding headers. Do not enable trust in forwarded addresses to make a benchmark easier to set up; per-address limits and the development enrollment rule depend on the real address.

Device authorization, quotas, the durable acknowledgment of AI messages and provider deadlines apply in addition to these limits.

## Stack qualification

Threads in the node have explicit stack sizes of 128 KiB, 256 KiB, 512 KiB or 1 MiB. A thread without an explicit size uses the 2 MiB default that the release executable declares. If the operating system refuses a requested stack size, the node does not start.

`make native-release VERSION=...` compiles the release executable with GCC's `-fstack-usage` and `-fcallgraph-info=su`. [scripts/stack_budget.py](../scripts/stack_budget.py) then reads the reports of the final link and writes them, with the raw call graphs and an inventory of every explicit stack size in `src/`, to the release's `stack-usage/` directory. Frames of test programs are not included.

The build fails when:

- a frame has a size that is not known at compile time;
- a frame is larger than 96 KiB. The one exception is the completion of a spot price batch, which may use 384 KiB and runs on a thread with a 1 MiB stack;
- a new explicit stack size appears in `src/` that the script does not know.

These are limits on single frames. They are not a measurement of the deepest call chain. Indirect calls, recursion and frames in libraries that were not compiled with link-time optimization still need review and tests on the release executable. For that reason the release build also runs the spot provider owner integration test against the release objects ([native-runtime.md](native-runtime.md#build)).

Two measurements explain the current numbers. Returning provider decoder results directly instead of copying them reduced the spot completion frame from 574,752 to 350,672 bytes. Moving wallet lookup scratch space off the stack reduced two frames from 89,216 and 74,976 bytes to 1,184 and 1,280 bytes. The 1 MiB spot stack keeps the headroom above the measured frame.

## Load testing

`tests/portfolio_load_test.py` drives the price routes with enrolled devices and checks that every returned price is fresh, valid and complete ([price-snapshot.md](price-snapshot.md#simulate-100-portfolio-opens)). Run it against a development node with a test data directory. A run on one machine with synthetic provider responses measures the node's own behavior; it does not establish latency on another host or the availability of real providers.
