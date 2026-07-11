#!/usr/bin/env bash
set -euo pipefail
FILE="lib/WebHandler.h"
for token in \
  "--bg" "--surface" "--primary" \
  "app-shell" "top-nav" "panel" "btn" \
  "latest-vitals" "kpi-grid" "recent-table" "<details" "diagnostic-data" "last-updated" \
  "history-table" "danger-zone" "btn-danger" "/export.csv" "匯出 CSV" \
  "form-shell" "field-label" "helper-text" "scan-refresh" "掃描中" \
  "/set_pin" "管理密碼" "current_pin" "new_pin" \
  "value-na" "state-na" \
  "kpi-sys" "kpi-dia" "kpi-pul" \
  "conn-transport" "conn-status" "conn-ip" \
  "/api/latest" "bpRefresh"
do
  grep -Fq -- "$token" "$FILE" || { echo "missing token: $token"; exit 1; }
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

for forbidden in "rawData" "transientSubjectId" "/raw_data" "查看原始數據" "量測原始資料"; do
  if grep -Fq -- "$forbidden" "$FILE"; then
    echo "forbidden identity-bearing diagnostic sink: $forbidden"
    exit 1
  fi
done
echo "UI markup checks passed."
