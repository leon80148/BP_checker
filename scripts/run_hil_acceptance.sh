#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

: "${BP_HIL_BOARD_ID:?BP_HIL_BOARD_ID is required}"
: "${BP_HIL_MONITOR_ID:?BP_HIL_MONITOR_ID is required}"
: "${BP_HIL_LOG_DIR:?BP_HIL_LOG_DIR is required}"
: "${BP_HIL_SOAK_HOURS:?BP_HIL_SOAK_HOURS is required and must equal 24}"
: "${BP_HIL_EVIDENCE_PUBLIC_KEY:?BP_HIL_EVIDENCE_PUBLIC_KEY is required}"
: "${BP_HIL_RELEASE_BUNDLE:?BP_HIL_RELEASE_BUNDLE is required}"
: "${BP_HIL_RUN_ID:?BP_HIL_RUN_ID is required}"
[[ -s "$BP_HIL_EVIDENCE_PUBLIC_KEY" ]] || { echo "HIL evidence public key is unavailable" >&2; exit 2; }
[[ "$BP_HIL_RUN_ID" =~ ^[A-Za-z0-9._:-]{4,80}$ ]] || { echo "invalid HIL run ID" >&2; exit 2; }
[[ "$BP_HIL_SOAK_HOURS" == "24" ]] || {
  echo "a complete 24-hour soak is required" >&2
  exit 1
}
[[ "$BP_HIL_BOARD_ID" =~ ^[A-Za-z0-9._:-]{4,64}$ ]] || {
  echo "invalid board identifier" >&2; exit 2;
}
[[ "$BP_HIL_MONITOR_ID" =~ ^[A-Za-z0-9._:-]{4,64}$ ]] || {
  echo "invalid monitor identifier" >&2; exit 2;
}
[[ -d "$BP_HIL_RELEASE_BUNDLE" ]] || { echo "signed HIL release bundle is missing" >&2; exit 2; }
release_json="$BP_HIL_RELEASE_BUNDLE/release.json"
firmware="$BP_HIL_RELEASE_BUNDLE/firmware.bin"
[[ -s "$release_json" && -s "$firmware" ]] || { echo "signed HIL release bundle is incomplete" >&2; exit 2; }
source_sha=$(jq -r '.source_sha' "$release_json")
firmware_sha256=$(jq -r '.artifact_sha256' "$release_json")
release_sequence=$(jq -r '.sequence' "$release_json")
[[ "$source_sha" == "$(git rev-parse --verify 'HEAD^{commit}')" ]] || { echo "HIL release source SHA is not current" >&2; exit 1; }
[[ "$firmware_sha256" =~ ^[0-9a-f]{64}$ && "$release_sequence" =~ ^(0|[1-9][0-9]*)$ ]] || { echo "HIL release identity is malformed" >&2; exit 1; }
if command -v sha256sum >/dev/null; then
  actual_firmware_sha=$(sha256sum "$firmware" | awk '{print $1}')
else
  actual_firmware_sha=$(shasum -a 256 "$firmware" | awk '{print $1}')
fi
[[ "$actual_firmware_sha" == "$firmware_sha256" ]] || { echo "HIL firmware hash mismatches signed bundle" >&2; exit 1; }
jq -e '.status == "signed"' "$release_json" >/dev/null

for file in \
  corpus-summary.json transport-faults.json network-security.json \
  signed-update.json rollback.json soak-summary.json operator-approval.json \
  raw-logs.sha256 evidence-manifest.txt evidence-attestation.sig.der
do
  [[ -s "$BP_HIL_LOG_DIR/$file" ]] || {
    echo "missing retained HIL evidence: $file" >&2
    exit 1
  }
done

jq -e --arg board "$BP_HIL_BOARD_ID" --arg monitor "$BP_HIL_MONITOR_ID" '
  .schema == "bp-hil-corpus-v1" and .board_id == $board and
  .monitor_id == $monitor and .deidentified == true and
  all(.cases[]; .passed == true) and
  ([.cases[].kind] | contains(["normal","boundary","error","movement",
    "fragmented","burst","disconnect","reconnect"]))
' "$BP_HIL_LOG_DIR/corpus-summary.json" >/dev/null
jq -e --arg board "$BP_HIL_BOARD_ID" --arg monitor "$BP_HIL_MONITOR_ID" '
  .schema == "bp-hil-transport-v1" and .board_id == $board and
  .monitor_id == $monitor and .usb2_type_b_verified == true and
  .function_32_format_5_verified == true and .power_fault_passed == true and
  .disconnect_reconnect_passed == true and .overflow_recovery_passed == true and
  all(.faults[]; .passed == true)
' "$BP_HIL_LOG_DIR/transport-faults.json" >/dev/null
jq -e --arg board "$BP_HIL_BOARD_ID" '
  .schema == "bp-hil-network-v1" and .board_id == $board and
  .ap_shutdown_passed == true and .physical_recovery_passed == true and
  .recovery_expiry_passed == true and .old_credentials_rejected == true and
  .sdk_erase_passed == true and .isolated_vlan_verified == true and
  .http_residual_accepted == true
' "$BP_HIL_LOG_DIR/network-security.json" >/dev/null
jq -e '.schema == "bp-hil-update-v1" and .signed_update_passed == true and
       .wrong_signature_rejected == true and .downgrade_rejected == true' \
  "$BP_HIL_LOG_DIR/signed-update.json" >/dev/null
jq -e '.schema == "bp-hil-rollback-v1" and .failed_health_rolled_back == true and
       .previous_image_booted == true' "$BP_HIL_LOG_DIR/rollback.json" >/dev/null
jq -e '.schema == "bp-hil-soak-v1" and
       (.duration_hours | type) == "number" and .duration_hours >= 24 and
       .watchdog_resets == 0 and .leak_trend == false and
       .stack_exhaustion == false and .record_order_valid == true and
       .record_checksums_valid == true and
       (.heap_start_bytes | type) == "number" and .heap_start_bytes > 0 and
       (.heap_end_bytes | type) == "number" and .heap_end_bytes > 0 and
       (.heap_min_bytes | type) == "number" and .heap_min_bytes > 0 and
       (.heap_slope_bytes_per_hour | type) == "number" and
       (.stack_min_watermark_bytes | type) == "number" and .stack_min_watermark_bytes > 0 and
       (.reset_reasons | type) == "array" and (.reset_reasons | length) > 0 and
       .unexpected_reset_count == 0 and
       (.throughput_measurements | type) == "number" and .throughput_measurements > 0 and
       (.data_loss_count | type) == "number" and .data_loss_count >= 0 and
       (.reconnect_count | type) == "number" and .reconnect_count >= 1' \
  "$BP_HIL_LOG_DIR/soak-summary.json" >/dev/null
jq -e '.schema == "bp-hil-approval-v1" and .approved == true and
       (.reviewer | length) > 0 and (.approved_at | length) > 0' \
  "$BP_HIL_LOG_DIR/operator-approval.json" >/dev/null

if ! (cd "$BP_HIL_LOG_DIR" && {
  if command -v sha256sum >/dev/null; then sha256sum -c raw-logs.sha256;
  else shasum -a 256 -c raw-logs.sha256; fi
}) >/dev/null 2>&1; then
  echo "HIL raw log digest verification failed" >&2
  exit 1
fi
sha256_file() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | awk '{print $1}';
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
sha256_stream() {
  if command -v sha256sum >/dev/null; then sha256sum | awk '{print $1}';
  else shasum -a 256 | awk '{print $1}'; fi
}
expected_manifest=$(mktemp)
{
  printf '%s\n' 'schema=bp-hil-evidence-v1' \
    "run_id=$BP_HIL_RUN_ID" "source_sha=$source_sha" \
    "firmware_sha256=$firmware_sha256" "release_sequence=$release_sequence" \
    "board_id=$BP_HIL_BOARD_ID" "monitor_id=$BP_HIL_MONITOR_ID" \
    "soak_hours=$BP_HIL_SOAK_HOURS"
  for file in corpus-summary.json transport-faults.json network-security.json \
              signed-update.json rollback.json soak-summary.json \
              operator-approval.json raw-logs.sha256; do
    printf '%s_sha256=%s\n' "${file//[-.]/_}" "$(sha256_file "$BP_HIL_LOG_DIR/$file")"
  done
} > "$expected_manifest"
if ! cmp -s "$expected_manifest" "$BP_HIL_LOG_DIR/evidence-manifest.txt"; then
  rm -f "$expected_manifest"
  echo "HIL evidence manifest binding failed" >&2
  exit 1
fi
rm -f "$expected_manifest"
expected_key_sha=$(jq -r '.hil_public_key_der_sha256' config/evidence-trust-anchors.json)
actual_key_sha=$(openssl pkey -pubin -in "$BP_HIL_EVIDENCE_PUBLIC_KEY" \
  -outform DER 2>/dev/null | sha256_stream)
[[ "$expected_key_sha" =~ ^[0-9a-f]{64}$ && "$actual_key_sha" == "$expected_key_sha" ]] || {
  echo "HIL evidence trust anchor is not configured or mismatched" >&2; exit 1;
}
openssl dgst -sha256 -verify "$BP_HIL_EVIDENCE_PUBLIC_KEY" \
  -signature "$BP_HIL_LOG_DIR/evidence-attestation.sig.der" \
  "$BP_HIL_LOG_DIR/evidence-manifest.txt" >/dev/null || {
    echo "HIL evidence attestation failed" >&2; exit 1;
  }

echo "HIL acceptance and 24-hour soak evidence passed: $BP_HIL_LOG_DIR"
