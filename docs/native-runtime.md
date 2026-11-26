# Native release build and runtime contract

This document describes how to build and sign a release of the node, what a release contains, and what the released program needs from the machine that runs it. Release engineers and anyone who runs the node outside a development checkout need it.

A release is one static Linux executable for x86-64 or ARM64, with a manifest and a signature. It contains no shell, interpreter, compiler or updater. Installing a release on a host, switching between releases, backing up the database and restarting the node are the job of the host. [FortisNet Node OS](https://github.com/FortisNet/node-os) is the reference host: its orchestrator verifies signed releases, updates and recovers the node, obtains the HTTPS certificate and runs the node unprivileged in its own container. See its [release documentation](https://github.com/FortisNet/node-os/blob/main/docs/releases.md).

## Build

Requirements: Docker, Python 3, and a complete passing `make verify` or `make lean-verify` report for the current checkout. The build refuses to start when a verified input has changed since the report was written ([verification.md](verification.md)).

```sh
make native-release VERSION=1.0.0
```

This runs [scripts/build-native-release.sh](../scripts/build-native-release.sh) once for each architecture and writes:

```text
dist/native-releases/1.0.0/x86_64/
dist/native-releases/1.0.0/aarch64/
  fortis-node               stripped executable, mode 555
  fortis-node.debug         the same program with symbols
  manifest                  what the signature covers
  hardening.json            compiler flag and ELF checks
  hardening-probes.json     results of run-time hardening probes
  compiler-commands.jsonl   every compiler invocation
  stack-usage/              stack frame reports
  licenses/                 notices for this project and its static dependencies
```

A version is 1 to 64 characters from `a-z A-Z 0-9 . _ -`. The build refuses to overwrite an existing release.

The build runs in a container made from [Dockerfile.native-build](../Dockerfile.native-build) with a pinned Alpine image, musl 1.2.6 and GCC 15.2.0. It copies the sources into a separate volume, so release objects never mix with development objects. It compiles with `-Os`, C++23, link-time optimization and dead-section removal, and links statically.

Before it writes a release, the build:

- runs the spot provider owner integration test against the release objects, with a generated CA and local fixtures. This exercises the deepest stack use in the program with the release compiler flags, which a health check alone would not;
- fails if the executable has an ELF interpreter or a shared-library dependency;
- checks stack frames ([resource-budgets.md](resource-budgets.md#stack-qualification));
- checks hardening, as described next.

### Hardening checks

The required flags are in [scripts/native_hardening.mk](../scripts/native_hardening.mk): position-independent code, strong stack protector, stack-clash protection, `_FORTIFY_SOURCE=3`, libstdc++ assertions, a static PIE link, read-only relocations, immediate binding, a non-executable stack and separate code segments.

[scripts/compiler_audit.py](../scripts/compiler_audit.py) records every compiler command. [scripts/native_hardening.py](../scripts/native_hardening.py) then checks that each source file was compiled with the required flags and that the stripped ELF has the expected properties. It writes `hardening.json`. `tests/native_hardening_probe.py` writes `hardening-probes.json`.

These checks cover code compiled by this build. The prebuilt C and C++ runtime libraries and handwritten assembly are outside compiler instrumentation.

### The manifest

`manifest` is a short text file of `key=value` lines:

| Key | Meaning |
| --- | --- |
| `format` | `2` |
| `version` | The version given to the build |
| `architecture` | `x86_64` or `aarch64` |
| `runtime` | `linux-musl-static-v1` |
| `storage_epoch` | The on-disk storage format the release reads and writes. Currently `1` |
| `sha256`, `bytes` | Hash and size of `fortis-node` |
| `source_commit` | The Git commit, or `unknown` when the checkout had uncommitted changes to verified inputs |
| `build_inputs_sha256` | Digest of the verified input hashes |

Two releases with the same `storage_epoch` can open each other's stores. A release that changes the storage format must change the epoch, and a host must not switch between epochs without a migration procedure.

## Sign

Sign each architecture's manifest on a trusted machine. Keep the private key off every node.

```sh
# Once: create a P-256 signing key and export its public half.
mkdir -p "$HOME/.config/fortis-signing" && chmod 700 "$HOME/.config/fortis-signing"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
  -out "$HOME/.config/fortis-signing/release-private.pem"
chmod 600 "$HOME/.config/fortis-signing/release-private.pem"
openssl pkey -in "$HOME/.config/fortis-signing/release-private.pem" -pubout -out release-public.pem

# For each release:
./scripts/sign-native-release.sh dist/native-releases/1.0.0/x86_64 "$HOME/.config/fortis-signing/release-private.pem"
./scripts/sign-native-release.sh dist/native-releases/1.0.0/aarch64 "$HOME/.config/fortis-signing/release-private.pem"
```

The script writes `manifest.sig`, an ECDSA signature over the exact bytes of `manifest`. A host that trusts the public key verifies the signature, then checks that the executable's hash and size match the manifest. The signature therefore identifies one exact executable.

Keep `fortis-node.debug` and the reports with the release, privately. Do not install the debug file on a node.

## What the program needs at run time

The node is one process. It needs no root privileges, no Linux capabilities and no writable files outside its data directory.

| Need | Detail |
| --- | --- |
| Data directory | `FORTIS_KV_DIR`: a writable directory on durable storage. The node takes an exclusive lock on it. Never start two nodes on one directory |
| Port | One argument, `fortis-node [port]`, or `PORT`. Default 8788. Alternatively `FORTIS_LISTEN_FDS=PORT=FD,...` names listeners that the host has already opened; the node then binds nothing |
| Health | `FORTIS_HEALTH_PORT`: optional separate port that serves only `/healthz`. Publish it on loopback only |
| CA bundle | `FORTIS_CA_BUNDLE_PATH`: PEM file used to verify providers. Required by every service that calls a provider. The node reads it at startup; restart the node after replacing the file |
| HTTPS | `FORTIS_HTTPS_CERT_DIR` and related variables ([native-https.md](native-https.md#configuration)) |
| Secrets | Provider keys as environment variables or as private key files, for example `FORTIS_COINGECKO_API_KEY_FILE` and `ANTHROPIC_API_KEY_PATH`. Never build them into an image |
| Release and health reports | `FORTIS_RELEASE_STATUS_DIR` (default `/deployment`): optional read-only directory written by the host ([node-updates.md](node-updates.md)) |
| Mode | `NODE_ENV` and `ATTESTATION_MODE` select development or production policy |
| Logging | The node writes log lines to standard error. `FORTIS_LOG_LEVEL` is `error`, `warn`, `info` (default) or `debug` |
| Operating system | Entropy, a monotonic and a wall clock, DNS and outbound TCP |

[src/config/native_config.h](../src/config/native_config.h) documents every variable. An invalid value stops startup with exit code 2 and a message that names the variable. Secrets are never printed.

Signals:

| Signal | Effect |
| --- | --- |
| `SIGTERM`, `SIGINT` | Stop accepting, finish and join all work, close the store, exit. Durably acknowledged asynchronous jobs resume after the next start |
| `SIGHUP` | Reload HTTPS certificates |

The node handles `SIGTERM` correctly as process 1 of a container.

A suitable container for the node has a read-only root filesystem, a non-root user, all capabilities dropped, memory and process limits, the data directory mounted read-write, and the certificate and report directories mounted read-only. The node does not restart itself after a crash, so the host must supervise it.

## Storage and updates

- The store survives a process kill: startup replays the log and discards an incomplete last frame ([kv-capacity.md](kv-capacity.md)).
- Back up the whole data directory while the node is stopped, including a `fortis.kv.recover` file if one exists. [scripts/kv_backup.py](../scripts/kv_backup.py) validates and copies a store.
- When a new release fails, go back to the previous executable and keep the current data. Do not restore an older backup over writes that the newer release has acknowledged.
- One process owns the store. An update therefore includes a short interruption. Multiple writers and replication are not supported.

## Tests

`make verify` includes tests of the release and hardening scripts (`tests/native_release_test.py`, `tests/native_hardening_test.py`, `tests/stack_budget_tool_test.py`) and runs ShellCheck on the release shell scripts. The tests that install a release on a host, update it and recover from failures are in [FortisNet Node OS](https://github.com/FortisNet/node-os).
