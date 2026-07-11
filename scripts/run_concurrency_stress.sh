#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/host_tests

CXX=${CXX:-c++}
SOURCE=test/host/stress_usb_cdc_concurrency.cpp
BASE=( -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -iquote . -Itest/host )
NORMAL=build/host_tests/stress_usb_cdc_concurrency
"$CXX" "${BASE[@]}" -o "$NORMAL" "$SOURCE"
"$NORMAL"

TSAN=build/host_tests/stress_usb_cdc_concurrency_tsan
TSAN_LOG=build/host_tests/tsan.log
if "$CXX" "${BASE[@]}" -fsanitize=thread -fno-omit-frame-pointer \
    -o "$TSAN" "$SOURCE" >"$TSAN_LOG" 2>&1; then
  if "$TSAN" >>"$TSAN_LOG" 2>&1; then
    echo "ThreadSanitizer stress passed."
  elif grep -Eqi 'ThreadSanitizer (is )?not supported|unsupported VMA range|ThreadSanitizer: unexpected memory mapping' "$TSAN_LOG"; then
    echo "ThreadSanitizer runtime unavailable on this platform; normal stress passed."
  else
    cat "$TSAN_LOG" >&2
    exit 1
  fi
else
  if grep -Eqi 'unsupported option.*fsanitize=thread|unknown argument.*fsanitize=thread|unrecognized command-line option.*fsanitize=thread|cannot find.*(clang_rt\.tsan|libtsan)|library not found.*tsan' "$TSAN_LOG"; then
    echo "ThreadSanitizer compiler support unavailable; normal stress passed."
  else
    cat "$TSAN_LOG" >&2
    exit 1
  fi
fi
