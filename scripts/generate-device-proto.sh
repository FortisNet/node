#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${FORTIS_PROTO_DIR:-"$project_dir/proto/schema"}
input='fortisnet/intelligence/v1/devices.proto'
descriptor="$project_dir/build/devices.pb"

if ! command -v protoc >/dev/null 2>&1; then
  echo 'protoc is required; use the pinned build container from scripts/build-image.sh.' >&2
  exit 1
fi

mkdir -p "$project_dir/build" "$project_dir/proto/generated"
protoc -I "$source_dir" --descriptor_set_out="$descriptor" "$input"
python3 "$project_dir/third_party/nanopb/generator/nanopb_generator.py" \
  --error-on-unmatched -f "$project_dir/proto/devices.options" \
  -D "$project_dir/proto/generated" "$descriptor"
echo 'Generated nanopb code from the existing device protobuf schema.'
