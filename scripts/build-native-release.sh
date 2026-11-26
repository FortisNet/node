#!/usr/bin/env sh
# Produce an unsigned immutable static release of the node for one processor
# architecture. Sign its manifest on a trusted host. The orchestrator is
# released separately, from FortisNet Node OS.
#
#   build-native-release.sh VERSION [x86_64|aarch64]
#   output: dist/native-releases/VERSION/ARCHITECTURE/
#
# The architecture defaults to x86_64. `make native-release` builds both; the
# two carry the same version and each has its own manifest and signature.
set -eu
project=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
version=${1:?usage: build-native-release.sh VERSION [x86_64|aarch64]}
architecture=${2:-x86_64}
case "$architecture" in
  x86_64) platform=linux/amd64 ;;
  aarch64) platform=linux/arm64 ;;
  *) echo 'Architecture must be x86_64 or aarch64' >&2; exit 2 ;;
esac
case "$version" in ''|*[!a-zA-Z0-9._-]*|.|..) echo 'Invalid release version' >&2; exit 2;; esac
[ "${#version}" -le 64 ] || exit 2
cd "$project"
python3 scripts/native_release.py --check-inputs
image=fortis-node-native-build:local-$architecture
docker build --platform "$platform" -f Dockerfile.native-build -t "$image" .
# An isolated volume avoids mixing musl/LTO objects with normal native/guest builds.
volume=fortis-native-build-$(date -u +%Y%m%dT%H%M%SZ)-$$
docker volume create "$volume" >/dev/null
trap 'docker volume rm "$volume" >/dev/null' EXIT HUP INT TERM
release=dist/native-releases/$version/$architecture
mkdir -p "$release" build/native-runtime
[ ! -e "$release/manifest" ] || { echo 'Release already exists' >&2; exit 1; }
docker run --rm --platform "$platform" -v "$volume:/work" \
  --mount "type=bind,source=$project,target=/input,readonly" \
  --mount "type=bind,source=$project/$release,target=/output" \
  --mount "type=bind,source=$project/build/native-runtime,target=/runtime" \
  -e FORTIS_BUILD_JOBS="${FORTIS_BUILD_JOBS:-4}" -e "ARCHITECTURE=$architecture" "$image" sh -eu -c '
    [ "$(uname -m)" = "$ARCHITECTURE" ]
    [ "$(gcc -dumpfullversion)" = 15.2.0 ]
    apk info -v | grep -q "^musl-1.2.6-"
    cp -a /input/src /input/third_party /input/proto /input/contracts /input/assets /input/scripts /input/tests /input/sources.mk /input/Makefile /work/
    mkdir -p .deps
    if [ -f /input/.deps/mbedtls-3.6.7.tar.bz2 ]; then cp /input/.deps/mbedtls-3.6.7.tar.bz2 .deps/; fi
    hardening_c=$(make -s -f scripts/native_hardening.mk c)
    hardening_cxx=$(make -s -f scripts/native_hardening.mk cxx)
    hardening_asm=$(make -s -f scripts/native_hardening.mk asm)
    hardening_link=$(make -s -f scripts/native_hardening.mk link)
    make -j"$FORTIS_BUILD_JOBS" CXX="python3 /work/scripts/compiler_audit.py /output/compiler-commands.jsonl g++" CC="python3 /work/scripts/compiler_audit.py /output/compiler-commands.jsonl gcc" \
      FORTIS_DEPENDENCY_CFLAGS="$hardening_c" FORTIS_DEPENDENCY_ASFLAGS="$hardening_asm" \
      CXXFLAGS="-Os -std=c++23 -fno-rtti -ffp-contract=off -flto -fstack-usage -fcallgraph-info=su -ffunction-sections -fdata-sections $hardening_cxx $hardening_link -Wl,--gc-sections -Wl,-z,stack-size=2097152 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wundef -Wcast-align -Woverloaded-virtual -Wnon-virtual-dtor" \
      CFLAGS="-Os -std=c11 $hardening_c -ffunction-sections -fdata-sections -Wall -Wextra -Wpedantic -Werror" build/fortis_node build/spot_provider_owner_test
    # Exercise real provider completion with the exact musl/-Os/LTO toolchain,
    # using generated-CA local fixtures rather than real provider credentials.
    python3 tests/spot_provider_owner_test.py build/spot_provider_owner_test
    # Fail closed if a loader or shared-library dependency reached the release.
    ! readelf -l build/fortis_node | grep -q INTERP
    ! readelf -d build/fortis_node | grep -q NEEDED
    python3 scripts/stack_budget.py build /output/stack-usage
    cp build/fortis_node /output/fortis-node.debug
    strip --strip-all -o /output/fortis-node build/fortis_node
    python3 scripts/native_hardening.py /output /output/compiler-commands.jsonl --architecture "$ARCHITECTURE"
    python3 tests/native_hardening_probe.py /output
    chmod 555 /output/fortis-node
    cp /etc/ssl/certs/ca-certificates.crt /runtime/
    mkdir -p /output/licenses
    cp /input/LICENSE /input/NOTICE /input/THIRD_PARTY_NOTICES.md /output/licenses/
    cp /input/third_party/native-runtime/* /output/licenses/
    cp .deps/mbedtls-3.6.7/LICENSE /output/licenses/mbedtls.LICENSE
    cp third_party/nanopb/LICENSE.txt /output/licenses/nanopb.LICENSE
    cp third_party/blst/v0.3.12/LICENSE /output/licenses/blst.LICENSE
    cp third_party/blake3/1.8.7/LICENSE* /output/licenses/
    cp third_party/ada-idna/v2.9.2/LICENSE-APACHE /output/licenses/idna-apache.LICENSE
    cp third_party/ada-idna/v2.9.2/LICENSE-MIT /output/licenses/idna-mit.LICENSE
  '
python3 scripts/native_release.py "$release" "$version" "$architecture"
printf 'Unsigned release: %s/%s\n' "$project" "$release"
