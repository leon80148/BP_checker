#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUTPUT=${1:-build/sbom.json}
ARTIFACT=${2:-build/firmware/BP_checker.ino.bin}

command -v arduino-cli >/dev/null || {
  echo "arduino-cli is required" >&2
  exit 1
}
command -v jq >/dev/null || {
  echo "jq is required" >&2
  exit 1
}
test -f VERSION || {
  echo "VERSION is missing" >&2
  exit 1
}
test -f sketch.yaml || {
  echo "sketch.yaml is missing" >&2
  exit 1
}
test -f "$ARTIFACT" || {
  echo "firmware artifact is missing: $ARTIFACT" >&2
  exit 1
}

sha256_file() {
  if command -v sha256sum >/dev/null; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

sha256_stream() {
  if command -v sha256sum >/dev/null; then
    sha256sum | awk '{print $1}'
  else
    shasum -a 256 | awk '{print $1}'
  fi
}

version=$(tr -d '\r\n' < VERSION)
source_sha=$(git rev-parse --verify 'HEAD^{commit}')
if [[ ! "$source_sha" =~ ^[0-9a-f]{40}$ ]]; then
  echo "source revision is not a full Git commit SHA: $source_sha" >&2
  exit 1
fi
source_dirty=false
if [[ -n "$(git status --porcelain --untracked-files=normal 2>/dev/null)" ]]; then
  source_dirty=true
fi
artifact_sha=$(sha256_file "$ARTIFACT")
artifact_size=$(wc -c < "$ARTIFACT" | tr -d ' ')
release_anchor=${BP_RELEASE_PUBLIC_KEY_DER_HEX:-}
trust_anchor_configured=false
trust_anchor_sha256=""
if [[ -n "$release_anchor" ]]; then
  trust_anchor_configured=true
  trust_anchor_sha256=$(printf '%s' "$release_anchor" | sha256_stream)
fi
vendor_source=src/third_party/espressif_usb_host_cdc_acm
test -d "$vendor_source" || {
  echo "vendored CDC source is missing: $vendor_source" >&2
  exit 1
}
vendor_sha=$(
  while IFS= read -r file; do
    printf '%s\0' "$file"
    sha256_file "$file"
  done < <(find "$vendor_source" -type f -print | LC_ALL=C sort) | sha256_stream
)
vendor_version="snapshot-${vendor_sha:0:12}"
cli_json=$(arduino-cli version --json)
build_properties=$(arduino-cli compile --profile esp32s3 --show-properties=expanded . 2>/dev/null)
compiler_path=$(printf '%s\n' "$build_properties" \
  | awk -F= '$1 == "compiler.path" { print $2; exit }')
compiler_command=$(printf '%s\n' "$build_properties" \
  | awk -F= '$1 == "compiler.cpp.cmd" { print $2; exit }')
compiler_version="unknown"
compiler_binary="${compiler_path}${compiler_command}"
if [[ -x "$compiler_binary" ]]; then
  compiler_version=$("$compiler_binary" --version | head -n 1)
fi

mkdir -p "$(dirname "$OUTPUT")"
jq -n \
  --arg schema "bp-checker-sbom-v1" \
  --arg version "$version" \
  --arg source_sha "$source_sha" \
  --argjson source_dirty "$source_dirty" \
  --argjson trust_anchor_configured "$trust_anchor_configured" \
  --arg trust_anchor_sha256 "$trust_anchor_sha256" \
  --arg profile "esp32s3" \
  --arg fqbn "esp32:esp32:esp32s3" \
  --arg platform "esp32:esp32" \
  --arg platform_version "3.3.7" \
  --arg library "ArduinoJson" \
  --arg library_version "7.4.2" \
  --arg vendor "espressif/usb_host_cdc_acm" \
  --arg vendor_version "$vendor_version" \
  --arg vendor_license "Apache-2.0" \
  --arg vendor_source "$vendor_source" \
  --arg vendor_upstream "https://components.espressif.com/components/espressif/usb_host_cdc_acm" \
  --arg vendor_sha "$vendor_sha" \
  --arg compiler "$compiler_version" \
  --arg artifact "$ARTIFACT" \
  --arg artifact_sha "$artifact_sha" \
  --argjson artifact_size "$artifact_size" \
  --argjson cli "$cli_json" \
  '{
    schema: $schema,
    firmware: {
      version: $version,
      source_sha: $source_sha,
      source_dirty: $source_dirty,
      trust_anchor_configured: $trust_anchor_configured,
      trust_anchor_sha256: $trust_anchor_sha256
    },
    build: {
      profile: $profile,
      fqbn: $fqbn,
      arduino_cli: $cli,
      compiler: $compiler
    },
    components: [
      {type: "platform", name: $platform, version: $platform_version},
      {type: "library", name: $library, version: $library_version},
      {
        type: "vendored-library",
        name: $vendor,
        version: $vendor_version,
        license: $vendor_license,
        source_path: $vendor_source,
        upstream: $vendor_upstream,
        sha256: $vendor_sha
      }
    ],
    artifact: {
      path: $artifact,
      size_bytes: $artifact_size,
      sha256: $artifact_sha
    }
  }' > "$OUTPUT"

jq -e '
  .schema == "bp-checker-sbom-v1" and
  .firmware.version != "" and
  (.firmware.source_sha | test("^[0-9a-f]{40}$")) and
  ((.firmware.trust_anchor_configured == false and
    .firmware.trust_anchor_sha256 == "") or
   (.firmware.trust_anchor_configured == true and
    (.firmware.trust_anchor_sha256 | length) == 64)) and
  .build.compiler != "unknown" and
  .components[0].version == "3.3.7" and
  .components[1].version == "7.4.2" and
  .components[2].name == "espressif/usb_host_cdc_acm" and
  .components[2].license == "Apache-2.0" and
  (.components[2].sha256 | length) == 64 and
  (.artifact.sha256 | length) == 64
' "$OUTPUT" >/dev/null

echo "SBOM: $OUTPUT"
