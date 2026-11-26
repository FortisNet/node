# Third-party materials

The project license ([LICENSE](LICENSE), Apache-2.0) covers the project's own
code. It does not replace the licenses of the code listed here. Keep these
notices when you redistribute source or binaries.

## Code in this repository

| Material | Where | License |
| --- | --- | --- |
| nanopb 0.4.9.2 | `third_party/nanopb` | zlib; `LICENSE.txt` |
| blst 0.3.12 | `third_party/blst/v0.3.12` | Apache-2.0; `LICENSE` |
| BLAKE3 1.8.7 | `third_party/blake3/1.8.7` | CC0-1.0, Apache-2.0 or Apache-2.0 with LLVM exception; the three license files |
| ada-idna 2.9.2 (data) | `third_party/ada-idna/v2.9.2` | Apache-2.0 or MIT; `LICENSE-APACHE`, `LICENSE-MIT` |

## Code the build downloads

| Material | How | License |
| --- | --- | --- |
| Mbed TLS 3.6.7 | `scripts/fetch-mbedtls.sh` downloads the source archive and checks its hash | Apache-2.0 |
| blst assembly | `scripts/fetch-blst-assembly.sh`, checked by hash | Apache-2.0 |
| `yaml`, `@bufbuild/protobuf` | `package.json`; used by generators and tests only, never by the server | ISC; Apache-2.0 and BSD-3-Clause |

## Code inside a release

A release is a static program. Besides the code above it contains parts of
the C library and of the compiler's runtime, whose notices are kept in
`third_party/native-runtime` and copied into every release:

| Material | License |
| --- | --- |
| musl 1.2.6 | MIT |
| GCC 15.2.0 runtime libraries | GPL-3.0 with the GCC Runtime Library Exception |

## Test data and images

The files in `tests/corpus` are invented test cases, or record the answers
of the project's own earlier implementation to invented inputs. Names of
outside services in them label made-up data. Two files,
`dashpay_proof_corpus.json` and `dashpay_upstream.json`, hold proofs
recorded from the public Dash network, used as test vectors for proof
verification. `contracts/bip39-english.json` is the standard BIP-39 word
list (MIT).

The images in `assets` and the logos in `docs/assets` belong to the
FortisNet project.
