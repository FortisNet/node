#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${FORTIS_PROTO_DIR:-"$project_dir/proto/schema"}
descriptor="$project_dir/build/prices.pb"

if ! command -v protoc >/dev/null 2>&1; then
  echo 'protoc is required; use the pinned build container from scripts/build-image.sh.' >&2
  exit 1
fi

mkdir -p "$project_dir/build" "$project_dir/proto/generated"
protoc -I "$source_dir" --descriptor_set_out="$descriptor" \
  fortisnet/intelligence/v1/prices.proto
python3 "$project_dir/third_party/nanopb/generator/nanopb_generator.py" \
  --error-on-unmatched -f "$project_dir/proto/prices_history.options" \
  -D "$project_dir/proto/generated" "$descriptor"
python3 - "$project_dir/proto/generated/fortisnet/intelligence/v1/prices.pb.c" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
path.write_bytes(path.read_bytes().rstrip(b'\n') + b'\n')
PY
echo 'Generated nanopb price code from the shared protobuf schema.'
