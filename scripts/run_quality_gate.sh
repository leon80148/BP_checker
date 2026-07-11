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
bash test/tooling/test_build_contract.sh
bash scripts/check_usb_callback_contract.sh
bash scripts/check_bounded_web_runtime.sh
bash scripts/check_security_runtime_integration.sh
bash scripts/check_firmware_update_runtime.sh
bash test/tooling/test_release_contract.sh
bash test/tooling/test_vulnerability_response.sh
bash scripts/run_host_tests.sh
bash scripts/run_concurrency_stress.sh

echo "== UI/static checks =="
bash scripts/check_ui_markup.sh

echo "== pinned firmware build =="
rm -rf build/firmware
mkdir -p build/firmware
version=$(tr -d '\r\n' < VERSION)
source_sha=$(git rev-parse --verify 'HEAD^{commit}')
compile_log=build/firmware/compile.log
grep -Fxq "#define BP_FIRMWARE_VERSION \"${version}\"" lib/BuildInfo.h
build_properties=$(arduino-cli compile --profile esp32s3 --show-properties=expanded . 2>/dev/null)
platform_path=$(printf '%s\n' "$build_properties" \
  | awk -F= '$1 == "runtime.platform.path" { print $2; exit }')
target_libs_path=$(printf '%s\n' "$build_properties" \
  | awk -F= '$1 == "runtime.tools.esp32s3-libs-3.3.7.path" { print $2; exit }')
[[ "$platform_path" == *esp32_esp32_3.3.7_* ]]
[[ "$target_libs_path" == *esp32_esp32s3-libs_3.3.7_* ]]

extra_flags="-DBP_BUILD_SHA_TOKEN=${source_sha}"
release_anchor=${BP_RELEASE_PUBLIC_KEY_DER_HEX:-}
if [[ -n "$release_anchor" ]]; then
  if [[ ! "$release_anchor" =~ ^[0-9a-f]+$ ||
        $(( ${#release_anchor} % 2 )) -ne 0 ||
        ${#release_anchor} -gt 256 ]]; then
    echo "BP_RELEASE_PUBLIC_KEY_DER_HEX must be even lowercase hex (max 128 DER bytes)" >&2
    exit 1
  fi
  extra_flags+=" -DBP_RELEASE_PUBLIC_KEY_DER_HEX=${release_anchor}"
fi

arduino-cli compile \
  --profile esp32s3 \
  --board-options USBMode=default \
  --warnings all \
  --clean \
  --output-dir build/firmware \
  --build-property "compiler.cpp.extra_flags=${extra_flags}" \
  . 2>&1 | tee "$compile_log"

bash scripts/check_compile_warnings.sh \
  "$compile_log" "$platform_path/" "$target_libs_path/"

nm_tool=$(printf '%s\n' "$build_properties" \
  | awk -F= '$1 == "compiler.path" { print $2 "xtensa-esp32s3-elf-nm"; exit }')
test -x "$nm_tool"
rollback_symbol=$(
  "$nm_tool" build/firmware/BP_checker.ino.elf \
    | awk '$3 == "verifyRollbackLater" { print $2; exit }'
)
if [[ "$rollback_symbol" != "T" ]]; then
  echo "verifyRollbackLater must be a strong linked override; found: ${rollback_symbol:-missing}" >&2
  exit 1
fi

artifact=build/firmware/BP_checker.ino.bin
test -s "$artifact" || {
  echo "expected firmware artifact is missing: $artifact" >&2
  exit 1
}
grep -aFq "$version" "$artifact"
grep -aFq "$source_sha" "$artifact"

echo "== SBOM/build metadata =="
bash scripts/generate_sbom.sh build/sbom.json "$artifact"
jq -e --arg version "$version" --arg sha "$(git rev-parse HEAD)" \
  '.firmware.version == $version and
   .firmware.source_sha == $sha and
   any(.components[]; .name == "espressif/usb_host_cdc_acm")' \
  build/sbom.json >/dev/null
if [[ "${CI:-false}" == "true" ]]; then
  jq -e '.firmware.source_dirty == false' build/sbom.json >/dev/null
fi

echo "== quality gate passed =="
wc -c "$artifact" build/sbom.json
