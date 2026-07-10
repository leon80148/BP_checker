#!/usr/bin/env bash
set -euo pipefail

if (( $# < 1 )); then
  echo "usage: $0 COMPILE_LOG [ALLOWED_PATH_PREFIX ...]" >&2
  exit 2
fi

log=$1
shift
test -f "$log" || {
  echo "compile log is missing: $log" >&2
  exit 2
}

status=0
while IFS= read -r line; do
  [[ "$line" == *'warning:'* ]] || continue

  allowed=false
  for prefix in "$@"; do
    if [[ -n "$prefix" && "$line" == "$prefix"* ]]; then
      allowed=true
      break
    fi
  done

  if [[ "$allowed" != "true" ]]; then
    echo "unapproved warning: $line" >&2
    status=1
  fi
done < "$log"

exit "$status"
