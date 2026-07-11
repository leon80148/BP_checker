#!/usr/bin/env bash
set -euo pipefail
FILE="lib/WebHandler.h"
for token in \
  "--bg" "--surface" "--primary" \
  "app-shell" "top-nav" "panel" "btn" \
  "latest-vitals" "kpi-grid" "recent-table" "<details" "diagnostic-data" "last-updated" \
  "history-table" "danger-zone" "btn-danger" "/export.csv" "匯出 CSV" \
  "form-shell" "field-label" "helper-text" "scan-refresh" "掃描中" \
  "/claim" "/security" "/rotate_credentials" "一次性啟用碼" "管理者帳號" "工作人員帳號" \
  "value-na" "state-na" \
  "kpi-sys" "kpi-dia" "kpi-pul" \
  "conn-transport" "conn-status" "conn-ip" \
  "/api/latest" "bpRefresh" \
  "lang='zh-Hant'" ":focus-visible" "aria-live='polite'" "role='status'" \
  "<caption>" "scope='col'" "table-scroll" \
  "measurement-freshness" "freshness_state" "last_successful_receive_age_ms" \
  "revision" "record_sequence" "session_sequence" "timestamp_source" \
  "quality" "movement_count" "firmware_version" "protocol" \
  "data_loss_count" "reconnect_count" "diagnostic_state" \
  "measurementReferencePolicyName()" "repeatedMeasurementGuidance()" \
  "bpRevision" "location.reload()" "poll-failure" "最後成功更新" \
  "server->currentRole() == bp_web::AccessRole::ADMIN"
do
  grep -Fq -- "$token" "$FILE" || { echo "missing token: $token"; exit 1; }
done

for token in \
  'setUInt64Json(doc["revision"], recordManager->getRevision())' \
  'setUInt64Json(doc["record_sequence"], latest.recordSequence)' \
  'setUInt64Json(doc["session_sequence"], latest.sessionSequence)' \
  'setUInt64Json(recordObj["record_sequence"], record.recordSequence)' \
  'setUInt64Json(recordObj["session_sequence"], record.sessionSequence)'
do
  grep -Fq -- "$token" "$FILE" || {
    echo "opaque uint64 API field is not an exact decimal JSON string: $token"
    exit 1
  }
done

for forbidden_numeric in \
  'doc["revision"] = recordManager->getRevision()' \
  'doc["record_sequence"] = latest.recordSequence' \
  'doc["session_sequence"] = latest.sessionSequence' \
  'recordObj["record_sequence"] = record.recordSequence' \
  'recordObj["session_sequence"] = record.sessionSequence'
do
  if grep -Fq -- "$forbidden_numeric" "$FILE"; then
    echo "opaque uint64 API field would lose precision in JSON: $forbidden_numeric"
    exit 1
  fi
done

POLICY_FILE="lib/MeasurementPolicy.h"
for token in \
  "血壓參考：2025 AHA/ACC 成人高血壓指引" \
  "實際門檻與脈搏規則由診所設定" \
  "1 分鐘" "不會自動平均"
do
  grep -Fq -- "$token" "$POLICY_FILE" || {
    echo "missing named measurement policy: $token"
    exit 1
  }
done

admin_surface_guards=$(grep -Fc -- \
  "server->currentRole() == bp_web::AccessRole::ADMIN" "$FILE")
if (( admin_surface_guards < 3 )); then
  echo "admin surfaces are not separately guarded"
  exit 1
fi

for forbidden in "正常" "異常" "patient_id" "patientId" "病患編號" "病歷號"; do
  if grep -Fq -- "$forbidden" "$FILE"; then
    echo "forbidden diagnostic or identity UI copy: $forbidden"
    exit 1
  fi
done

# There is no host WebServer seam yet, so keep the storage-failure contract in
# a static gate: clear may report success and erase diagnostics only after the
# crash-consistent tombstone and cleanup both report success.
for token in \
  "if (!recordManager->clearRecords())" \
  "history_clear_failed" \
  "server->send(503" \
  "儲存系統未能確認清除完成"
do
  grep -Fq -- "$token" "$FILE" || {
    echo "missing storage-failure contract: $token"
    exit 1
  }
done

for forbidden in \
  "rawData" "transientSubjectId" "/raw_data" "查看原始數據" "量測原始資料" \
  "/set_pin" "adminPin" "current_pin" "new_pin"
do
  if grep -Fq -- "$forbidden" "$FILE"; then
    echo "forbidden identity-bearing diagnostic sink: $forbidden"
    exit 1
  fi
done
echo "UI markup checks passed."
