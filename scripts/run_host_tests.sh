#!/usr/bin/env bash
# Host-side unit tests：用本機 clang++ 編譯 header-only lib 並執行測試。
# test/host/Arduino.h、Preferences.h 提供最小 Arduino shim。
# 每個 test/host/test_*.cpp 是一個獨立測試 binary。
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/host_tests"
mkdir -p "$BUILD_DIR"

status=0
for src in test/host/test_*.cpp; do
  name=$(basename "$src" .cpp)
  c++ -std=c++17 -Wall -Wextra -I. -Itest/host -o "$BUILD_DIR/$name" "$src"
  echo "== $name =="
  "$BUILD_DIR/$name" || status=1
done
exit $status
