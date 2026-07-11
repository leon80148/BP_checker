#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CHECKER="$ROOT/scripts/check_compile_warnings.sh"
GATE="$ROOT/scripts/run_quality_gate.sh"
SKETCH="$ROOT/BP_checker.ino"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

test -x "$CHECKER"
grep -Fq -- '--build-property "compiler.cpp.extra_flags=' "$GATE"
if grep -Fq -- '--build-property "build.extra_flags=' "$GATE"; then
  echo "build.extra_flags must not replace the platform defaults" >&2
  exit 1
fi

allowed="$TMP/pinned-platform"
project="$TMP/project"
mkdir -p "$allowed" "$project"

printf '%s\n' "$allowed/core.cpp:1: warning: pinned dependency warning" > "$TMP/allowed.log"
"$CHECKER" "$TMP/allowed.log" "$allowed/"

printf '%s\n' 'cc1plus: warning: unsafe compiler flag' > "$TMP/compiler.log"
if "$CHECKER" "$TMP/compiler.log" "$allowed/" 2> "$TMP/compiler.err"; then
  echo "compiler warnings without an allowlisted path must fail" >&2
  exit 1
fi

printf '%s\n' "$project/source.cpp:2: warning: project warning" > "$TMP/project.log"
if "$CHECKER" "$TMP/project.log" "$allowed/" 2> "$TMP/project.err"; then
  echo "project warnings must fail" >&2
  exit 1
fi

printf '%s\n' 'clean compile output' > "$TMP/clean.log"
"$CHECKER" "$TMP/clean.log" "$allowed/"

for token in \
  'const bool historyLoaded = recordManager.loadFromStorage();' \
  'if (!historyLoaded)' \
  'history_load_failed' \
  "data-status='storage_error'" \
  '請聯絡管理人員檢查儲存空間' \
  'history_load_succeeded count=' \
  'loadHistoryFromStorage();'
do
  grep -Fq -- "$token" "$SKETCH" || {
    echo "missing startup storage contract: $token" >&2
    exit 1
  }
done

load_line=$(grep -nF 'const bool historyLoaded = recordManager.loadFromStorage();' "$SKETCH" | head -1 | cut -d: -f1)
failure_line=$(grep -nF 'history_load_failed' "$SKETCH" | head -1 | cut -d: -f1)
return_line=$(awk -v start="$failure_line" 'NR > start && /return;/ { print NR; exit }' "$SKETCH")
success_line=$(grep -nF 'history_load_succeeded count=' "$SKETCH" | head -1 | cut -d: -f1)
if [[ -z "$return_line" || ! (load_line -lt failure_line && failure_line -lt return_line && return_line -lt success_line) ]]; then
  echo "startup storage failure must return before the normal loaded claim" >&2
  exit 1
fi

echo "Build contract checks passed."
