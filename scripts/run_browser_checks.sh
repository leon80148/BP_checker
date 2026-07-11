#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

: "${BP_BROWSER_EXECUTABLE:?BP_BROWSER_EXECUTABLE is required}"
: "${BP_BROWSER_VERSION:?BP_BROWSER_VERSION is required}"
: "${BP_BROWSER_EVIDENCE_DIR:?BP_BROWSER_EVIDENCE_DIR is required}"
: "${BP_BROWSER_RUN_ID:?BP_BROWSER_RUN_ID is required}"
: "${BP_BROWSER_BASE_URL:?BP_BROWSER_BASE_URL is required}"
: "${BP_BROWSER_RUNNER:?BP_BROWSER_RUNNER is required}"
: "${BP_BROWSER_EVIDENCE_PUBLIC_KEY:?BP_BROWSER_EVIDENCE_PUBLIC_KEY is required}"

[[ -x "$BP_BROWSER_EXECUTABLE" ]] || { echo "browser executable is unavailable" >&2; exit 2; }
[[ -x "$BP_BROWSER_RUNNER" ]] || { echo "browser runner is unavailable" >&2; exit 2; }
[[ -s "$BP_BROWSER_EVIDENCE_PUBLIC_KEY" ]] || { echo "browser evidence public key is unavailable" >&2; exit 2; }
[[ "$BP_BROWSER_RUN_ID" =~ ^[A-Za-z0-9._:-]{4,80}$ ]] || {
  echo "invalid browser run ID" >&2; exit 2;
}
[[ ! -e "$BP_BROWSER_EVIDENCE_DIR" ]] || {
  echo "browser evidence directory must be new for every run" >&2; exit 1;
}
actual_version=$($BP_BROWSER_EXECUTABLE --version 2>&1)
[[ "$actual_version" == *"$BP_BROWSER_VERSION"* ]] || {
  echo "browser version mismatch: $actual_version" >&2
  exit 1
}

mkdir -p "$BP_BROWSER_EVIDENCE_DIR"
source_sha=$(git rev-parse --verify 'HEAD^{commit}')
"$BP_BROWSER_RUNNER" \
  "$BP_BROWSER_EXECUTABLE" "$BP_BROWSER_BASE_URL" \
  "$BP_BROWSER_EVIDENCE_DIR" "$BP_BROWSER_RUN_ID" "$source_sha"

routes="$BP_BROWSER_EVIDENCE_DIR/routes.json"
accessibility="$BP_BROWSER_EVIDENCE_DIR/accessibility.json"
screenshots="$BP_BROWSER_EVIDENCE_DIR/screenshots"
manifest="$BP_BROWSER_EVIDENCE_DIR/evidence-manifest.txt"
attestation="$BP_BROWSER_EVIDENCE_DIR/evidence-attestation.sig.der"
jq -e --arg run_id "$BP_BROWSER_RUN_ID" --arg source_sha "$source_sha" \
  --arg base_url "$BP_BROWSER_BASE_URL" --arg browser_version "$BP_BROWSER_VERSION" '
  .schema == "bp-browser-routes-v1" and
  .run_id == $run_id and .source_sha == $source_sha and
  .base_url == $base_url and .browser_version == $browser_version and
  .anonymous_denied == true and .staff_matrix_passed == true and
  .admin_matrix_passed == true and .host_rejection_passed == true and
  .refresh_wrap_passed == true and .stale_disconnect_passed == true
' "$routes" >/dev/null
jq -e --arg run_id "$BP_BROWSER_RUN_ID" --arg source_sha "$source_sha" '
  .schema == "bp-browser-accessibility-v1" and .violations == 0 and
  .run_id == $run_id and .source_sha == $source_sha and
  .keyboard_passed == true and .responsive_passed == true
' "$accessibility" >/dev/null
[[ -d "$screenshots" ]] && find "$screenshots" -type f -name '*.png' -size +0c \
  | grep -q . || { echo "browser screenshots are missing" >&2; exit 1; }

sha256_file() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
sha256_stream() {
  if command -v sha256sum >/dev/null; then sha256sum | awk '{print $1}';
  else shasum -a 256 | awk '{print $1}'; fi
}
screenshots_sha=$(
  while IFS= read -r file; do
    name=${file#"$screenshots/"}
    [[ "$name" =~ ^[A-Za-z0-9._/-]+$ ]] || exit 1
    printf '%s\0%s\n' "$name" "$(sha256_file "$file")"
  done < <(find "$screenshots" -type f -name '*.png' | LC_ALL=C sort) | sha256_stream
)
expected_manifest=$(mktemp)
printf '%s\n' \
  'schema=bp-browser-evidence-v1' \
  "run_id=$BP_BROWSER_RUN_ID" "source_sha=$source_sha" \
  "base_url=$BP_BROWSER_BASE_URL" "browser_version=$BP_BROWSER_VERSION" \
  "routes_sha256=$(sha256_file "$routes")" \
  "accessibility_sha256=$(sha256_file "$accessibility")" \
  "screenshots_sha256=$screenshots_sha" > "$expected_manifest"
if ! cmp -s "$expected_manifest" "$manifest"; then
  rm -f "$expected_manifest"
  echo "browser evidence manifest binding failed" >&2
  exit 1
fi
rm -f "$expected_manifest"

expected_key_sha=$(jq -r '.browser_public_key_der_sha256' config/evidence-trust-anchors.json)
actual_key_sha=$(openssl pkey -pubin -in "$BP_BROWSER_EVIDENCE_PUBLIC_KEY" \
  -outform DER 2>/dev/null | sha256_stream)
[[ "$expected_key_sha" =~ ^[0-9a-f]{64}$ && "$actual_key_sha" == "$expected_key_sha" ]] || {
  echo "browser evidence trust anchor is not configured or mismatched" >&2; exit 1;
}
openssl dgst -sha256 -verify "$BP_BROWSER_EVIDENCE_PUBLIC_KEY" \
  -signature "$attestation" "$manifest" >/dev/null || {
    echo "browser evidence attestation failed" >&2; exit 1;
  }

printf '%s\n' "$actual_version" > "$BP_BROWSER_EVIDENCE_DIR/browser-version.txt"
echo "Browser evidence checks passed: $BP_BROWSER_EVIDENCE_DIR"
