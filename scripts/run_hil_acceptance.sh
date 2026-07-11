#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

: "${BP_HIL_BOARD_ID:?BP_HIL_BOARD_ID is required}"
: "${BP_HIL_MONITOR_ID:?BP_HIL_MONITOR_ID is required}"
: "${BP_HIL_LOG_DIR:?BP_HIL_LOG_DIR is required}"
: "${BP_HIL_SOAK_HOURS:?BP_HIL_SOAK_HOURS is required and must equal 24}"
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

for file in \
  corpus-summary.json transport-faults.json network-security.json \
  signed-update.json rollback.json soak-summary.json operator-approval.json
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
jq -e '.schema == "bp-hil-soak-v1" and .duration_hours >= 24 and
       .watchdog_resets == 0 and .leak_trend == false and
       .stack_exhaustion == false and .record_order_valid == true and
       .record_checksums_valid == true' "$BP_HIL_LOG_DIR/soak-summary.json" >/dev/null
jq -e '.schema == "bp-hil-approval-v1" and .approved == true and
       (.reviewer | length) > 0 and (.approved_at | length) > 0' \
  "$BP_HIL_LOG_DIR/operator-approval.json" >/dev/null

echo "HIL acceptance and 24-hour soak evidence passed: $BP_HIL_LOG_DIR"
