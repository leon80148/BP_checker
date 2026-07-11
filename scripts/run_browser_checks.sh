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

[[ -x "$BP_BROWSER_EXECUTABLE" ]] || { echo "browser executable is unavailable" >&2; exit 2; }
[[ -x "$BP_BROWSER_RUNNER" ]] || { echo "browser runner is unavailable" >&2; exit 2; }
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

printf '%s\n' "$actual_version" > "$BP_BROWSER_EVIDENCE_DIR/browser-version.txt"
echo "Browser evidence checks passed: $BP_BROWSER_EVIDENCE_DIR"
