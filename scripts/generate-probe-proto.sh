#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$project_dir/proto/schema"
input='fortisnet/intelligence/v1/dashpay.proto'
descriptor="$project_dir/build/dashpay.pb"

mkdir -p "$project_dir/build" "$project_dir/proto/generated"
protoc -I "$source_dir" --descriptor_set_out="$descriptor" "$input"
python3 "$project_dir/third_party/nanopb/generator/nanopb_generator.py" \
  --error-on-unmatched -f "$project_dir/proto/dashpay.options" \
  -D "$project_dir/proto/generated" "$descriptor"
echo 'Generated nanopb code from the existing DashPayNameResponse schema.'
