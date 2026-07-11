#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

header=lib/FirmwareUpdateRuntime.h
source=src/FirmwareUpdateRuntime.cpp
anchor=lib/ReleaseTrustAnchor.h

for file in "$header" "$source" "$anchor"; do
  test -f "$file" || { echo "missing signed update runtime: $file" >&2; exit 1; }
done

for token in \
  'BP_RELEASE_PUBLIC_KEY_DER_HEX' \
  'kReleasePublicKeyDerHex'
do
  grep -Fq -- "$token" "$anchor" || { echo "missing trust anchor contract: $token" >&2; exit 1; }
done

for token in \
  'mbedtls_pk_parse_public_key' \
  'mbedtls_pk_verify' \
  'MBEDTLS_MD_SHA256' \
  'esp_ota_get_next_update_partition' \
  'esp_ota_begin' 'esp_ota_write' 'esp_ota_end' 'esp_ota_abort' \
  'esp_ota_set_boot_partition' \
  'esp_ota_get_state_partition' \
  'ESP_OTA_IMG_PENDING_VERIFY' \
  'esp_ota_mark_app_valid_cancel_rollback' \
  'esp_ota_mark_app_invalid_rollback_and_reboot' \
  'PendingUpdateReceipt' \
  'authorizeManifest(' \
  'MonotonicSequenceStore'
do
  grep -Fq -- "$token" "$source" "$header" || {
    echo "missing signed OTA runtime contract: $token" >&2
    exit 1
  }
done

if rg -n 'PRIVATE KEY|BEGIN EC PRIVATE|BEGIN PRIVATE' "$anchor" "$source" "$header"; then
  echo "private signing key material is forbidden" >&2
  exit 1
fi

echo "signed firmware update runtime contract passed"
