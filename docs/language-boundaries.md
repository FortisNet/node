# Language boundaries

This document states which programming languages the node uses and where.
It is for readers who audit what runs in production and what is only build
or test tooling.

## The server

The node is one static executable, `build/fortis_node`. The request-handling
application is C++. C and assembly provide cryptography, protobuf support
(nanopb) and platform adapters. There is no server-side JavaScript request
handler.

HTML is rendered by C++ or embedded in C++ source. Interactive pages contain
browser-side CSS and JavaScript for search, charts, pagination and refresh.
The API reference HTML is generated at build time from the project's YAML
inputs (`contracts/`, `openapi.native.yaml`) and compiled into the server.

## Generator inputs

The repository owns its generator inputs: the network catalog, the redaction
data, the reference cases, the OpenAPI inputs and the protobuf schemas under
`proto/schema/`. The generators and the default build read nothing outside
this repository, and they are Python with no package to install.

## Build and test tooling

Python and shell scripts are used for build and test orchestration,
protobuf generation, linting, provider fixtures and probes. They are not
linked into the server and are not part of a release. They give verification
that is independent of the C++ code under test.

Two optional test oracles use other languages: a Flutter wire-capture test
(`tests/app_post_capture_test.dart`) and a Rust DashPay proof oracle
(`tests/dashpay_oracle/`).

## Deployment

A release is the static executable and its manifest. Installing, supervising
and updating the node on a host is the job of
[FortisNet Node OS](https://github.com/FortisNet/node-os). See
[native-runtime.md](native-runtime.md) and [node-updates.md](node-updates.md)
for the node's side of that.
