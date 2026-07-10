#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CHECKER="$ROOT/scripts/check_compile_warnings.sh"
GATE="$ROOT/scripts/run_quality_gate.sh"
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

echo "Build contract checks passed."
