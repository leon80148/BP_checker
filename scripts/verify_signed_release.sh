#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

bundle=${1:-}
[[ $# -eq 1 && -d "$bundle" ]] || {
  echo "usage: scripts/verify_signed_release.sh SIGNED_BUNDLE" >&2
  exit 2
}

sha256_file() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
sha256_stream() {
  if command -v sha256sum >/dev/null; then sha256sum | awk '{print $1}';
  else shasum -a 256 | awk '{print $1}'; fi
}

release_root=$(cd build/release && pwd -P)
bundle=$(cd "$bundle" && pwd -P)
case "$bundle" in
  "$release_root"/*-signed) ;;
  *) echo "signed bundle must be under build/release and end in -signed" >&2; exit 2 ;;
esac
if find "$bundle" -type l -print | grep -q .; then
  echo "signed bundle cannot contain symbolic links" >&2
  exit 1
fi
expected_files=$'checksums.sha256\ncompile.log\nfirmware.bin\nmanifest.sig.b64\nmanifest.sig.der\nmanifest.txt\nrelease-public-key.der\nrelease-public-key.pem\nrelease.json\nsbom.json'
actual_files=$(cd "$bundle" && find . -maxdepth 1 -type f -print | sed 's#^./##' | LC_ALL=C sort)
[[ "$actual_files" == "$expected_files" ]] || { echo "signed bundle file set invalid" >&2; exit 1; }
if ! (cd "$bundle" && {
  if command -v sha256sum >/dev/null; then sha256sum -c checksums.sha256;
  else shasum -a 256 -c checksums.sha256; fi
}) >/dev/null 2>&1; then
  echo "signed bundle checksum verification failed" >&2
  exit 1
fi

release_json="$bundle/release.json"
firmware="$bundle/firmware.bin"
version=$(jq -r '.version' "$release_json")
source_sha=$(jq -r '.source_sha' "$release_json")
sequence=$(jq -r '.sequence' "$release_json")
minimum=$(jq -r '.minimum_sequence' "$release_json")
artifact_sha=$(sha256_file "$firmware")
artifact_size=$(wc -c < "$firmware" | tr -d ' ')
[[ "$source_sha" == "$(git rev-parse --verify 'HEAD^{commit}')" ]] || {
  echo "signed bundle source SHA is not current" >&2; exit 1;
}
[[ "$sequence" =~ ^(0|[1-9][0-9]*)$ && "$minimum" =~ ^(0|[1-9][0-9]*)$ ]] || {
  echo "signed bundle sequence is malformed" >&2; exit 1;
}
jq -e --arg version "$version" --arg sha "$source_sha" \
  --arg sequence "$sequence" --arg minimum "$minimum" \
  --arg artifact_sha "$artifact_sha" '
  .schema == "bp-release-candidate-v1" and .status == "signed" and
  .source_dirty == false and .trust_anchor_configured == true and
  .version == $version and .source_sha == $sha and .sequence == $sequence and
  .minimum_sequence == $minimum and .artifact_sha256 == $artifact_sha
' "$release_json" >/dev/null

expected_manifest=$(mktemp)
expected_public_der=""
trap 'rm -f "$expected_manifest"; [[ -z "$expected_public_der" ]] || rm -f "$expected_public_der"' EXIT
printf '%s\n' 'schema=bp-update-v1' "version=$version" \
  'target=esp32:esp32:esp32s3' "source_sha=$source_sha" \
  "sequence=$sequence" "minimum_sequence=$minimum" \
  "size=$artifact_size" "sha256=$artifact_sha" > "$expected_manifest"
cmp -s "$expected_manifest" "$bundle/manifest.txt" || {
  echo "signed bundle canonical manifest mismatch" >&2; exit 1;
}

anchor_hex=$(xxd -p -c 1000 "$bundle/release-public-key.der" | tr -d '\r\n')
anchor_sha=$(printf '%s' "$anchor_hex" | sha256_stream)
release_public_key_der_sha256=$(sha256_file "$bundle/release-public-key.der")
approved_release_public_key_der_sha256=$(
  jq -r '.release_public_key_der_sha256' config/evidence-trust-anchors.json
)
[[ "$approved_release_public_key_der_sha256" =~ ^[0-9a-f]{64}$ &&
   "$release_public_key_der_sha256" == "$approved_release_public_key_der_sha256" ]] || {
  echo "signed bundle release key is not the reviewed trust anchor" >&2; exit 1;
}
[[ "$anchor_sha" == "$(jq -r '.trust_anchor_sha256' "$release_json")" ]] || {
  echo "signed bundle trust anchor mismatch" >&2; exit 1;
}
jq -e --arg version "$version" --arg sha "$source_sha" \
  --arg artifact_sha "$artifact_sha" --argjson artifact_size "$artifact_size" \
  --arg anchor_sha "$anchor_sha" '
  .firmware.version == $version and .firmware.source_sha == $sha and
  .firmware.source_dirty == false and .firmware.trust_anchor_configured == true and
  .firmware.trust_anchor_sha256 == $anchor_sha and
  .artifact.sha256 == $artifact_sha and .artifact.size_bytes == $artifact_size
' "$bundle/sbom.json" >/dev/null

expected_public_der=$(mktemp)
openssl pkey -pubin -in "$bundle/release-public-key.pem" -outform DER \
  -out "$expected_public_der" 2>/dev/null
cmp -s "$expected_public_der" "$bundle/release-public-key.der" || {
  echo "signed bundle public key encodings differ" >&2; exit 1;
}
openssl pkey -pubin -in "$bundle/release-public-key.pem" -text -noout 2>&1 \
  | grep -Eq 'ASN1 OID: prime256v1|NIST CURVE: P-256' || {
    echo "signed bundle trust anchor is not P-256" >&2; exit 1;
  }
openssl dgst -sha256 -verify "$bundle/release-public-key.pem" \
  -signature "$bundle/manifest.sig.der" "$bundle/manifest.txt" >/dev/null || {
    echo "signed bundle release signature is invalid" >&2; exit 1;
  }
expected_b64=$(base64 < "$bundle/manifest.sig.der" | tr -d '\r\n')
[[ "$expected_b64" == "$(tr -d '\r\n' < "$bundle/manifest.sig.b64")" ]] || {
  echo "signed bundle signature encodings differ" >&2; exit 1;
}

rm -f "$expected_manifest" "$expected_public_der"
trap - EXIT
echo "Signed release bundle verified: $bundle"
