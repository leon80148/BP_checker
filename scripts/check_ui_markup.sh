#!/usr/bin/env bash
set -euo pipefail
FILE="lib/WebHandler.h"
SKETCH="BP_checker.ino"
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
  "/measurement_policy" "/set_measurement_policy" \
  "policy-name" "policy-version" "review-systolic" "stale-seconds" \
  "surfaceVisible(server->currentRole()" \
  "data-loss-count" "reconnect-count" \
  "loss.textContent=d.data_loss_count" "reconnect.textContent=d.reconnect_count" \
  "時間來源"
do
  grep -Fq -- "$token" "$FILE" || { echo "missing token: $token"; exit 1; }
done

for token in \
  "html += \"';let bpPolicyVersion='\";" \
  "if(String(d.policy_version)!==bpPolicyVersion){location.reload();return;}"
do
  grep -Fq -- "$token" "$FILE" || {
    echo "missing policy-version dashboard reload contract: $token"
    exit 1
  }
done

policy_snapshot_line=$(grep -nF "html += \"';let bpPolicyVersion='\";" "$FILE" \
  | head -1 | cut -d: -f1)
policy_snapshot_value_line=$(grep -nF \
  "html += activePolicy().policyVersion;" "$FILE" | tail -1 | cut -d: -f1)
if [[ -z "$policy_snapshot_line" || -z "$policy_snapshot_value_line" || \
      ! "$policy_snapshot_value_line" -eq $((policy_snapshot_line + 1)) ]]; then
  echo "dashboard policy snapshot must use the active policy version"
  exit 1
fi

latest_json_line=$(grep -nF "const d=await r.json();" "$FILE" \
  | head -1 | cut -d: -f1)
policy_reload_line=$(grep -nF \
  "if(String(d.policy_version)!==bpPolicyVersion){location.reload();return;}" \
  "$FILE" | head -1 | cut -d: -f1)
poll_success_line=$(grep -nF "bpLastPollSuccess=new Date()" "$FILE" \
  | head -1 | cut -d: -f1)
if [[ -z "$latest_json_line" || -z "$policy_reload_line" || \
      -z "$poll_success_line" || ! "$latest_json_line" -lt "$policy_reload_line" || \
      ! "$policy_reload_line" -lt "$poll_success_line" ]]; then
  echo "policy version must trigger reload before dashboard state updates"
  exit 1
fi

for token in \
  "MonotonicMillis64 uptimeClock" \
  "uptimeClock.observe(static_cast<uint32_t>(millis()))" \
  "MeasurementPolicyStore measurementPolicyStore" \
  "measurementPolicyStore.loadOrCreate()"
do
  grep -Fq -- "$token" "$SKETCH" || {
    echo "missing main-loop monotonic clock contract: $token"
    exit 1
  }
done

observe_line=$(grep -nF \
  "uptimeClock.observe(static_cast<uint32_t>(millis()))" "$SKETCH" \
  | tail -1 | cut -d: -f1)
web_line=$(grep -nF "server.handleClient();" "$SKETCH" | tail -1 | cut -d: -f1)
if [[ -z "$observe_line" || -z "$web_line" || ! "$observe_line" -lt "$web_line" ]]; then
  echo "main-loop clock must be observed before browser polling"
  exit 1
fi

policy_load_line=$(grep -nF "measurementPolicyStore.loadOrCreate()" "$SKETCH" \
  | head -1 | cut -d: -f1)
runtime_ready_line=$(grep -nF "runtimeReady = true;" "$SKETCH" \
  | head -1 | cut -d: -f1)
if [[ -z "$policy_load_line" || -z "$runtime_ready_line" || \
      ! "$policy_load_line" -lt "$runtime_ready_line" ]]; then
  echo "validated measurement policy must load before runtime readiness"
  exit 1
fi

for forbidden_default in \
  "classifyMeasurement(latest)" \
  "classifyMeasurement(record)" \
  "MeasurementPolicyConfig{}.staleAfterMs"
do
  if grep -Fq -- "$forbidden_default" "$FILE"; then
    echo "production Web path still uses an implicit default policy: $forbidden_default"
    exit 1
  fi
done

for required_injection in \
  "classifyMeasurement(latest, activePolicy())" \
  "classifyMeasurement(record, activePolicy())" \
  "input.staleAfterMs = activePolicy().staleAfterMs" \
  "policy_name" "policy_version"
do
  grep -Fq -- "$required_injection" "$FILE" || {
    echo "active persisted policy is not injected everywhere: $required_injection"
    exit 1
  }
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
