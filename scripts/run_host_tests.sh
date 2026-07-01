#!/usr/bin/env bash
# Host-side unit tests：用本機 clang++ 編譯 header-only lib 並執行測試。
# test/host/Arduino.h 提供最小 Arduino String shim。
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/host_tests"
mkdir -p "$BUILD_DIR"

c++ -std=c++17 -Wall -Wextra -I. -Itest/host \
  -o "$BUILD_DIR/test_bp_parser" test/host/test_bp_parser.cpp

"$BUILD_DIR/test_bp_parser"
