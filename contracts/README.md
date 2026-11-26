# Native-owned contracts

This directory and `../proto/schema/` are authoritative inputs for the C++
Intelligence project. Generation, builds and normal tests do not read the
earlier TypeScript service or its packages.

The initial data was imported from that service to preserve the existing wire
contract and behavior. This is an ownership transfer, not an ongoing sync.

- `network-catalog.json`: ordered network seeds and capability names. The
  generator retains native field-size, escaping and response-capacity checks.
- `redaction-data.json`: invisible code points, confusable mappings and the
  replacement text; `bip39-english.json`: the sorted 2,048-word vocabulary.
  The redaction algorithm is owned by `src/compat/diagnostic_redaction.cpp`.
  Unicode normalization tables still come from the pinned Node/ICU toolchain.
- `openapi.base.yaml` and `../openapi.native.yaml`: native API documentation
  inputs. Both now belong to this project; the latter applies native route
  exclusions and extensions before rendering.
- `price-id-fixtures.json`: frozen migration reference cases. This is test
  evidence, not a parser implementation; regeneration needs no legacy parser.
- `../proto/schema/`: the complete 16-file protobuf schema tree, initially
  byte-identical to the previous contract. Keep field numbers and client wire
  compatibility under review when changing it.

Install this project's tooling with `npm ci --omit=optional`. The pinned YAML
and protobuf JavaScript packages support documentation generation and an
independent protobuf test oracle. The optional Evo SDK is only for explicit
DashPay live probes (`npm ci` installs it); no server runtime uses it.

Historical fixture descriptions may still name their original TypeScript
capture source. They are provenance, not executable dependencies. Native changes
are reviewed against local schemas, fixtures and client compatibility tests.
