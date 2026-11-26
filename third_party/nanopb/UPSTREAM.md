# Nanopb source

These `pb_*` C sources, headers, generator, and `LICENSE.txt` come unchanged
from [nanopb 0.4.9.2](https://github.com/nanopb/nanopb/tree/nanopb-0.4.9.2),
commit `160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f`.

The generated `proto/generated/fortisnet/intelligence/v1/dashpay.pb.{c,h}`
comes from the existing `server/intelligence/proto/.../dashpay.proto` with
`proto/dashpay.options`. Run `sh scripts/generate-probe-proto.sh` inside the
build container after changing the source schema or options.
