#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

command -v arduino-cli >/dev/null || {
  echo "arduino-cli is required" >&2
  exit 1
}
command -v jq >/dev/null || {
  echo "jq is required" >&2
  exit 1
}

cli_version=$(arduino-cli version --json | jq -r '.VersionString')
if [[ "$cli_version" != "1.4.1" ]]; then
  echo "Arduino CLI 1.4.1 is required; found $cli_version" >&2
  exit 1
fi

grep -Fxq 'default_profile: esp32s3' sketch.yaml
grep -Fxq '      - platform: esp32:esp32 (3.3.7)' sketch.yaml
grep -Fxq '      - ArduinoJson (7.4.2)' sketch.yaml
grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$' VERSION

echo "== host tests =="
bash scripts/run_host_tests.sh

echo "== UI/static checks =="
bash scripts/check_ui_markup.sh

echo "== pinned firmware build =="
rm -rf build/firmware
mkdir -p build/firmware
version=$(tr -d '\r\n' < VERSION)
compile_log=build/firmware/compile.log
grep -Fxq "#define BP_FIRMWARE_VERSION \"${version}\"" lib/BuildInfo.h

arduino-cli compile \
  --profile esp32s3 \
  --board-options USBMode=default \
  --warnings all \
  --clean \
  --output-dir build/firmware \
  . 2>&1 | tee "$compile_log"

if grep -F "$ROOT/" "$compile_log" | grep -F 'warning:' >/dev/null; then
  echo "project warning detected; see $compile_log" >&2
  exit 1
fi

artifact=build/firmware/BP_checker.ino.bin
test -s "$artifact" || {
  echo "expected firmware artifact is missing: $artifact" >&2
  exit 1
}

echo "== SBOM/build metadata =="
bash scripts/generate_sbom.sh build/sbom.json "$artifact"
jq -e --arg version "$version" --arg sha "$(git rev-parse HEAD)" \
  '.firmware.version == $version and .firmware.source_sha == $sha' \
  build/sbom.json >/dev/null
if [[ "${CI:-false}" == "true" ]]; then
  jq -e '.firmware.source_dirty == false' build/sbom.json >/dev/null
fi

echo "== quality gate passed =="
wc -c "$artifact" build/sbom.json
