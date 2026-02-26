#!/usr/bin/env bash
set -euo pipefail
FILE="lib/WebHandler.h"
for token in \
  "--bg" "--surface" "--primary" \
  "app-shell" "top-nav" "panel" "btn" \
  "latest-vitals" "kpi-grid" "recent-table" "<details" "raw-data" "last-updated" \
  "history-table" "danger-zone" "btn-danger" \
  "form-shell" "field-label" "helper-text" "scan-refresh"
do
  grep -Fq -- "$token" "$FILE" || { echo "missing token: $token"; exit 1; }
done
echo "UI markup checks passed."
