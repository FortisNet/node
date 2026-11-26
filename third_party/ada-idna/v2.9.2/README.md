# ada-idna (Ada v2.9.2) — generator input only

`ada_idna.cpp` is the ada-url/idna amalgamation shipped by Ada **v2.9.2**
(`https://raw.githubusercontent.com/ada-url/ada/v2.9.2/src/ada_idna.cpp`,
generated 2023-09-19, Unicode 15.1 tables). It is byte-identical to the IDNA
section of the v2.9.2 single-header release (`singleheader.zip`, SHA-256
`b2cce630590b490d79ea4f4460ba77efd5fb29c5a87a4e8cb7ebc4859bc4b564`), which is
what Node.js 22.23.4 (`node:22-alpine`, the production TypeScript runtime)
bundles.

SHA-256 of `ada_idna.cpp`:
`1b9af936cdb63fb295c61e9444fee89e0b565205f8002fb40425fcaae5d544b7`

This file is **data**, never compiled or linked. `scripts/generate_idna_tables.py`
verifies the hash and extracts the mapping, normalization, bidi and joiner
tables into `src/idna_tables.cpp`; the algorithm itself is ported by hand in
`src/idna.cpp`. Licensed under MIT or Apache-2.0 (`LICENSE-MIT`,
`LICENSE-APACHE`, copied from the same tag).
