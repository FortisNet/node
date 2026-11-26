#!/usr/bin/env sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
archive="$project_dir/.deps/mbedtls-3.6.7.tar.bz2"
source_dir="$project_dir/.deps/mbedtls-3.6.7"
checksum=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

mkdir -p "$project_dir/.deps"
if [ ! -f "$archive" ]; then
  curl -fsSL \
    https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 \
    -o "$archive"
fi
actual=$(shasum -a 256 "$archive" | awk '{print $1}')
if [ "$actual" != "$checksum" ]; then
  echo "Mbed TLS archive checksum mismatch: $actual" >&2
  exit 1
fi
if [ ! -f "$source_dir/library/ssl_tls.c" ]; then
  tar -xjf "$archive" -C "$project_dir/.deps"
fi
