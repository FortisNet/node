#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
destination="$root/third_party/blst/v0.3.12/build"
if [ -s "$destination/assembly.S" ] &&
   [ -s "$destination/elf/add_mod_256-x86_64.s" ] &&
   [ -s "$destination/mach-o/add_mod_256-x86_64.s" ]; then
  exit 0
fi

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
curl -fsSL --retry 2 \
  https://github.com/supranational/blst/archive/refs/tags/v0.3.12.tar.gz \
  -o "$temporary/blst.tar.gz"
python3 - "$temporary/blst.tar.gz" <<'PY'
import hashlib
import pathlib
import sys

expected = '0b3d073a855ddb177845c60ea110e11890e0e39a75287f83b2754d67fd82ffec'
actual = hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()
if actual != expected:
    raise SystemExit(f'blst archive checksum mismatch: {actual}')
PY
tar -xzf "$temporary/blst.tar.gz" -C "$temporary"
cmp "$temporary/blst-0.3.12/src/server.c" \
    "$root/third_party/blst/v0.3.12/src/server.c"
mkdir -p "$destination"
cp "$temporary/blst-0.3.12/build/assembly.S" "$destination/assembly.S"
cp -R "$temporary/blst-0.3.12/build/elf" "$destination/elf"
cp -R "$temporary/blst-0.3.12/build/mach-o" "$destination/mach-o"
