#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${FORTIS_PROTO_DIR:-"$project_dir/proto/schema"}
descriptor="$project_dir/build/networks.pb"
news_descriptor="$project_dir/build/news.pb"

if ! command -v protoc >/dev/null 2>&1; then
  echo 'protoc is required; use the pinned build container from scripts/build-image.sh.' >&2
  exit 1
fi

mkdir -p "$project_dir/build" "$project_dir/proto/generated"
protoc -I "$source_dir" --descriptor_set_out="$news_descriptor" \
  fortisnet/intelligence/v1/news.proto
python3 "$project_dir/third_party/nanopb/generator/nanopb_generator.py" \
  -D "$project_dir/proto/generated" "$news_descriptor"
protoc -I "$source_dir" --include_imports --descriptor_set_out="$descriptor" \
  fortisnet/intelligence/v1/networks.proto
python3 "$project_dir/third_party/nanopb/generator/nanopb_generator.py" \
  --error-on-unmatched -f "$project_dir/proto/networks.options" \
  -D "$project_dir/proto/generated" "$descriptor"
python3 - "$project_dir/proto/generated/fortisnet/intelligence/v1" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
for name in ('networks.pb.c', 'news.pb.c'):
    path = root / name
    path.write_bytes(path.read_bytes().rstrip(b'\n') + b'\n')
PY
echo 'Generated nanopb network and imported news code.'
