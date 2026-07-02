#!/usr/bin/env bash
set -euo pipefail
FILE="lib/WebHandler.h"
for token in \
  "--bg" "--surface" "--primary" \
  "app-shell" "top-nav" "panel" "btn" \
  "latest-vitals" "kpi-grid" "recent-table" "<details" "raw-data" "last-updated" \
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
echo "UI markup checks passed."
