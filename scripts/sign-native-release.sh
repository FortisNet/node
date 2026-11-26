#!/usr/bin/env sh
# Run only on a trusted signing machine. Never upload the private key to a node.
set -eu
release=${1:?usage: sign-native-release.sh RELEASE_DIRECTORY PRIVATE_KEY}
key=${2:?missing private signing key}
[ -f "$release/manifest" ] && [ -f "$release/fortis-node" ] && [ -f "$key" ] || exit 2
umask 077
signature=$release/.manifest.sig-$$
trap 'rm -f "$signature"' EXIT HUP INT TERM
openssl dgst -sha256 -sign "$key" -out "$signature" "$release/manifest"
mv "$signature" "$release/manifest.sig"
