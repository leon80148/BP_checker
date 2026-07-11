#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

usage() {
  echo "usage: scripts/package_release.sh --candidate | --sign-candidate BUNDLE_DIR" >&2
  exit 2
}

sha256_file() {
  if command -v sha256sum >/dev/null; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

valid_uint64() {
  local value=$1
  [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]] || return 1
  (( ${#value} < 20 )) && return 0
  (( ${#value} == 20 )) || return 1
  [[ "$value" < "18446744073709551616" ]]
}

refresh_checksums() {
  local bundle=$1
  : > "$bundle/checksums.sha256"
  while IFS= read -r file; do
    printf '%s  %s\n' "$(sha256_file "$bundle/$file")" "$file" \
      >> "$bundle/checksums.sha256"
  done < <(
    cd "$bundle"
    find . -type f ! -name checksums.sha256 -print \
      | sed 's#^./##' | LC_ALL=C sort
  )
}

candidate_evidence_is_current() {
  local sha=$1
  local anchor=$2
  local artifact=build/firmware/BP_checker.ino.bin
  local anchor_configured=false anchor_sha=""
  if [[ -n "$anchor" ]]; then
    anchor_configured=true
    anchor_sha=$(printf '%s' "$anchor" | {
      if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi
    } | awk '{print $1}')
  fi
  [[ -s "$artifact" && -s build/firmware/compile.log && -s build/sbom.json ]] || return 1
  jq -e --arg sha "$sha" --arg artifact_sha "$(sha256_file "$artifact")" \
    --arg anchor_sha "$anchor_sha" --argjson anchor_configured "$anchor_configured" '
    .firmware.source_sha == $sha and
    .firmware.source_dirty == false and
    .firmware.trust_anchor_configured == $anchor_configured and
    .firmware.trust_anchor_sha256 == $anchor_sha and
    .artifact.sha256 == $artifact_sha
  ' build/sbom.json >/dev/null
}

make_candidate() {
  : "${BP_RELEASE_SEQUENCE:?BP_RELEASE_SEQUENCE is required}"
  : "${BP_RELEASE_MINIMUM_SEQUENCE:?BP_RELEASE_MINIMUM_SEQUENCE is required}"
  valid_uint64 "$BP_RELEASE_SEQUENCE" || {
    echo "BP_RELEASE_SEQUENCE must be canonical uint64" >&2; exit 2;
  }
  valid_uint64 "$BP_RELEASE_MINIMUM_SEQUENCE" || {
    echo "BP_RELEASE_MINIMUM_SEQUENCE must be canonical uint64" >&2; exit 2;
  }
  if (( ${#BP_RELEASE_SEQUENCE} < ${#BP_RELEASE_MINIMUM_SEQUENCE} )) ||
     { (( ${#BP_RELEASE_SEQUENCE} == ${#BP_RELEASE_MINIMUM_SEQUENCE} )) &&
       [[ "$BP_RELEASE_SEQUENCE" < "$BP_RELEASE_MINIMUM_SEQUENCE" ]]; }; then
    echo "release sequence cannot be below minimum sequence" >&2
    exit 2
  fi
  if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "release candidates require a clean worktree" >&2
    exit 1
  fi

  local sha version anchor bundle artifact artifact_sha artifact_size
  sha=$(git rev-parse --verify 'HEAD^{commit}')
  version=$(tr -d '\r\n' < VERSION)
  anchor=${BP_RELEASE_PUBLIC_KEY_DER_HEX:-}
  if ! candidate_evidence_is_current "$sha" "$anchor"; then
    BP_RELEASE_PUBLIC_KEY_DER_HEX="$anchor" bash scripts/run_quality_gate.sh
  fi

  artifact=build/firmware/BP_checker.ino.bin
  artifact_sha=$(sha256_file "$artifact")
  artifact_size=$(wc -c < "$artifact" | tr -d ' ')
  bundle="build/release/BP_checker-${version}-${sha:0:12}"
  rm -rf "$bundle"
  mkdir -p "$bundle"
  cp "$artifact" "$bundle/firmware.bin"
  cp build/firmware/compile.log "$bundle/compile.log"
  cp build/sbom.json "$bundle/sbom.json"

  cat > "$bundle/manifest.txt" <<EOF
schema=bp-update-v1
version=$version
target=esp32:esp32:esp32s3
source_sha=$sha
sequence=$BP_RELEASE_SEQUENCE
minimum_sequence=$BP_RELEASE_MINIMUM_SEQUENCE
size=$artifact_size
sha256=$artifact_sha
EOF
  (( $(wc -c < "$bundle/manifest.txt") <= 384 )) || {
    echo "canonical manifest exceeds firmware bound" >&2; exit 1;
  }

  local anchor_configured=false anchor_sha=""
  if [[ -n "$anchor" ]]; then
    anchor_configured=true
    anchor_sha=$(printf '%s' "$anchor" | {
      if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi
    } | awk '{print $1}')
  fi
  jq -n \
    --arg schema bp-release-candidate-v1 \
    --arg version "$version" --arg source_sha "$sha" \
    --arg sequence "$BP_RELEASE_SEQUENCE" \
    --arg minimum_sequence "$BP_RELEASE_MINIMUM_SEQUENCE" \
    --arg artifact_sha "$artifact_sha" --arg anchor_sha "$anchor_sha" \
    --argjson source_dirty false --argjson trust_anchor_configured "$anchor_configured" \
    '{schema:$schema,status:"unsigned",version:$version,source_sha:$source_sha,
      source_dirty:$source_dirty,sequence:$sequence,minimum_sequence:$minimum_sequence,
      artifact_sha256:$artifact_sha,trust_anchor_configured:$trust_anchor_configured,
      trust_anchor_sha256:$anchor_sha}' > "$bundle/release.json"
  if [[ -n "$anchor" ]]; then
    command -v xxd >/dev/null || { echo "xxd is required for anchored candidates" >&2; exit 1; }
    printf '%s' "$anchor" | xxd -r -p > "$bundle/release-public-key.der"
  fi
  refresh_checksums "$bundle"
  echo "$bundle"
  echo "candidate_checksums_sha256=$(sha256_file "$bundle/checksums.sha256")"
}

sign_candidate() {
  local requested_bundle=${1:-}
  [[ -d "$requested_bundle" ]] || { echo "candidate bundle directory is required" >&2; exit 2; }
  local release_root bundle
  mkdir -p build/release
  release_root=$(cd build/release && pwd -P)
  bundle=$(cd "$requested_bundle" && pwd -P)
  case "$bundle" in
    "$release_root"/*) ;;
    *) echo "candidate bundle must be under build/release" >&2; exit 2 ;;
  esac
  if find "$bundle" -type l -print | grep -q .; then
    echo "candidate bundle cannot contain symbolic links" >&2
    exit 1
  fi
  local expected_files actual_files
  expected_files=$'checksums.sha256\ncompile.log\nfirmware.bin\nmanifest.txt\nrelease-public-key.der\nrelease.json\nsbom.json'
  actual_files=$(
    cd "$bundle"
    find . -maxdepth 1 -type f -print | sed 's#^./##' | LC_ALL=C sort
  )
  if [[ "$actual_files" != "$expected_files" ]]; then
    echo "candidate file set invalid" >&2
    exit 1
  fi
  : "${BP_RELEASE_SIGN_COMMAND:?BP_RELEASE_SIGN_COMMAND is required}"
  : "${BP_RELEASE_CANDIDATE_SHA256:?BP_RELEASE_CANDIDATE_SHA256 is required}"
  [[ "$BP_RELEASE_CANDIDATE_SHA256" =~ ^[0-9a-f]{64}$ ]] || {
    echo "BP_RELEASE_CANDIDATE_SHA256 must be lowercase SHA-256" >&2; exit 2;
  }
  [[ "$BP_RELEASE_SIGN_COMMAND" != *[[:space:]]* ]] || {
    echo "BP_RELEASE_SIGN_COMMAND must be one executable path" >&2; exit 2;
  }
  [[ -x "$BP_RELEASE_SIGN_COMMAND" ]] || {
    echo "release signer is not executable" >&2; exit 2;
  }
  command -v openssl >/dev/null || { echo "openssl is required" >&2; exit 1; }
  command -v xxd >/dev/null || { echo "xxd is required" >&2; exit 1; }
  local approval_digest
  approval_digest=$(sha256_file "$bundle/checksums.sha256")
  if [[ "$approval_digest" != "$BP_RELEASE_CANDIDATE_SHA256" ]]; then
    echo "candidate approval digest mismatch" >&2
    exit 1
  fi
  if ! (
    cd "$bundle"
    if command -v sha256sum >/dev/null; then
      sha256sum -c checksums.sha256
    else
      shasum -a 256 -c checksums.sha256
    fi
  ) >/dev/null 2>&1; then
    echo "candidate checksum verification failed" >&2
    exit 1
  fi
  jq -e '.schema == "bp-release-candidate-v1" and .status == "unsigned" and
         .source_dirty == false and .trust_anchor_configured == true' \
    "$bundle/release.json" >/dev/null
  [[ -s "$bundle/manifest.txt" && -s "$bundle/release-public-key.der" ]] || {
    echo "anchored candidate inputs are incomplete" >&2; exit 1;
  }

  local version source_sha sequence minimum artifact_sha artifact_size
  version=$(jq -r '.version' "$bundle/release.json")
  source_sha=$(jq -r '.source_sha' "$bundle/release.json")
  sequence=$(jq -r '.sequence' "$bundle/release.json")
  minimum=$(jq -r '.minimum_sequence' "$bundle/release.json")
  artifact_sha=$(sha256_file "$bundle/firmware.bin")
  artifact_size=$(wc -c < "$bundle/firmware.bin" | tr -d ' ')
  if [[ -n "$(git status --porcelain --untracked-files=normal)" ]] ||
     [[ "$source_sha" != "$(git rev-parse --verify 'HEAD^{commit}')" ]]; then
    echo "candidate source revision is not the clean signing checkout" >&2
    exit 1
  fi
  local expected_manifest
  expected_manifest=$(mktemp)
  printf '%s\n' \
    'schema=bp-update-v1' \
    "version=$version" \
    'target=esp32:esp32:esp32s3' \
    "source_sha=$source_sha" \
    "sequence=$sequence" \
    "minimum_sequence=$minimum" \
    "size=$artifact_size" \
    "sha256=$artifact_sha" > "$expected_manifest"
  if ! cmp -s "$expected_manifest" "$bundle/manifest.txt" ||
     ! jq -e --arg sha "$source_sha" --arg artifact_sha "$artifact_sha" '
       .firmware.source_sha == $sha and .firmware.source_dirty == false and
       .artifact.sha256 == $artifact_sha
     ' "$bundle/sbom.json" >/dev/null ||
     ! jq -e --arg artifact_sha "$artifact_sha" '
       .artifact_sha256 == $artifact_sha
     ' "$bundle/release.json" >/dev/null; then
    rm -f "$expected_manifest"
    echo "candidate manifest binding failed" >&2
    exit 1
  fi
  rm -f "$expected_manifest"

  local anchor_hex anchor_sha recorded_anchor_sha
  anchor_hex=$(xxd -p -c 1000 "$bundle/release-public-key.der" | tr -d '\r\n')
  anchor_sha=$(printf '%s' "$anchor_hex" | {
    if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi
  } | awk '{print $1}')
  recorded_anchor_sha=$(jq -r '.trust_anchor_sha256' "$bundle/release.json")
  if [[ "$anchor_sha" != "$recorded_anchor_sha" ]] ||
     ! jq -e --arg anchor_sha "$anchor_sha" '
       .firmware.trust_anchor_configured == true and
       .firmware.trust_anchor_sha256 == $anchor_sha
     ' "$bundle/sbom.json" >/dev/null; then
    echo "candidate trust anchor binding failed" >&2
    exit 1
  fi

  local source_bundle signed_bundle signing_dir approved_manifest_sha
  source_bundle=$bundle
  signed_bundle="${source_bundle}-signed"
  [[ ! -e "$signed_bundle" ]] || {
    echo "signed bundle already exists: $signed_bundle" >&2; exit 1;
  }
  approved_manifest_sha=$(sha256_file "$source_bundle/manifest.txt")
  signing_dir=$(mktemp -d "$release_root/.signing.XXXXXX")
  chmod 700 "$signing_dir"
  trap 'rm -rf "$signing_dir"' EXIT
  for file in checksums.sha256 compile.log firmware.bin manifest.txt \
              release-public-key.der release.json sbom.json; do
    cp "$source_bundle/$file" "$signing_dir/$file"
  done
  bundle=$signing_dir
  if [[ "$(sha256_file "$bundle/checksums.sha256")" != "$approval_digest" ]] ||
     ! (
       cd "$bundle"
       if command -v sha256sum >/dev/null; then
         sha256sum -c checksums.sha256
       else
         shasum -a 256 -c checksums.sha256
       fi
     ) >/dev/null 2>&1; then
    echo "private signing snapshot did not match approved candidate" >&2
    exit 1
  fi

  "$BP_RELEASE_SIGN_COMMAND" "$bundle/manifest.txt" "$bundle/manifest.sig.der"
  if [[ "$(sha256_file "$bundle/checksums.sha256")" != "$approval_digest" ]] ||
     ! (
       cd "$bundle"
       if command -v sha256sum >/dev/null; then
         sha256sum -c checksums.sha256
       else
         shasum -a 256 -c checksums.sha256
       fi
     ) >/dev/null 2>&1; then
    echo "approved snapshot changed while signer was running" >&2
    exit 1
  fi
  local signature_size
  signature_size=$(wc -c < "$bundle/manifest.sig.der" | tr -d ' ')
  (( signature_size > 0 && signature_size <= 80 )) || {
    echo "signer did not produce a bounded DER signature" >&2; exit 1;
  }
  openssl pkey -pubin -inform DER -in "$bundle/release-public-key.der" \
    -out "$bundle/release-public-key.pem" >/dev/null 2>&1
  openssl pkey -pubin -in "$bundle/release-public-key.pem" -text -noout 2>&1 \
    | grep -Eq 'ASN1 OID: prime256v1|NIST CURVE: P-256' || {
      echo "release trust anchor must be P-256" >&2; exit 1;
    }
  openssl dgst -sha256 -verify "$bundle/release-public-key.pem" \
    -signature "$bundle/manifest.sig.der" "$bundle/manifest.txt" >/dev/null
  base64 < "$bundle/manifest.sig.der" | tr -d '\r\n' \
    > "$bundle/manifest.sig.b64"
  if [[ "$(sha256_file "$bundle/manifest.txt")" != "$approved_manifest_sha" ]] ||
     [[ "$(sha256_file "$bundle/firmware.bin")" != "$artifact_sha" ]] ||
     [[ "$(wc -c < "$bundle/firmware.bin" | tr -d ' ')" != "$artifact_size" ]] ||
     [[ "$(xxd -p -c 1000 "$bundle/release-public-key.der" | tr -d '\r\n')" != "$anchor_hex" ]]; then
    echo "private signing snapshot changed during signing" >&2
    exit 1
  fi
  local tmp
  tmp=$(mktemp)
  jq '.status = "signed"' "$bundle/release.json" > "$tmp"
  mv "$tmp" "$bundle/release.json"
  refresh_checksums "$bundle"
  if ! (
    cd "$bundle"
    if command -v sha256sum >/dev/null; then
      sha256sum -c checksums.sha256
    else
      shasum -a 256 -c checksums.sha256
    fi
  ) >/dev/null 2>&1 ||
     ! openssl dgst -sha256 -verify "$bundle/release-public-key.pem" \
       -signature "$bundle/manifest.sig.der" "$bundle/manifest.txt" >/dev/null; then
    echo "signed snapshot final verification failed" >&2
    exit 1
  fi
  mv "$bundle" "$signed_bundle"
  trap - EXIT
  chmod -R a-w "$signed_bundle"
  echo "$signed_bundle"
}

case ${1:-} in
  --candidate) [[ $# -eq 1 ]] || usage; make_candidate ;;
  --sign-candidate) [[ $# -eq 2 ]] || usage; sign_candidate "$2" ;;
  *) usage ;;
esac
