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
  'MBEDTLS_ECP_DP_SECP256R1' \
  'esp_ota_get_next_update_partition' \
  'esp_ota_begin' 'esp_ota_write' 'esp_ota_end' 'esp_ota_abort' \
  'esp_ota_set_boot_partition' \
  'esp_ota_get_state_partition' \
  'esp_partition_read' \
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

for token in \
  'verifyRunningImage(' \
  'constantTimeEqual(' \
  'allowFirstInitialization && !_pendingVerify' \
  'if (!_ready) {' \
  'rollbackIfPending();'
do
  grep -Fq -- "$token" "$source" "$header" || {
    echo "missing pending-image fail-closed contract: $token" >&2
    exit 1
  }
done

hash_line=$(grep -nF 'if (!verifyRunningImage(' "$source" | head -1 | cut -d: -f1)
pending_line=$(grep -nF '_pendingPolicy.beginPending(' "$source" | head -1 | cut -d: -f1)
clear_line=$(grep -nF 'clearPendingReceipt()' "$source" | tail -1 | cut -d: -f1)
valid_line=$(grep -nF 'esp_ota_mark_app_valid_cancel_rollback()' "$source" | head -1 | cut -d: -f1)
if [[ -z "$hash_line" || -z "$pending_line" || ! "$hash_line" -lt "$pending_line" ]]; then
  echo "running image hash must verify before pending-health state" >&2
  exit 1
fi
if [[ -z "$clear_line" || -z "$valid_line" || ! "$clear_line" -lt "$valid_line" ]]; then
  echo "pending receipt must clear durably before mark-valid" >&2
  exit 1
fi
pending_state_line=$(grep -nF '_pendingVerify = imageState == ESP_OTA_IMG_PENDING_VERIFY;' "$source" | head -1 | cut -d: -f1)
sequence_load_line=$(grep -nF '_sequenceStore.load()' "$source" | head -1 | cut -d: -f1)
if [[ -z "$pending_state_line" || -z "$sequence_load_line" || \
      ! "$pending_state_line" -lt "$sequence_load_line" ]]; then
  echo "pending OTA state must latch before fallible sequence storage" >&2
  exit 1
fi

if rg -n 'PRIVATE KEY|BEGIN EC PRIVATE|BEGIN PRIVATE' "$anchor" "$source" "$header"; then
  echo "private signing key material is forbidden" >&2
  exit 1
fi

echo "signed firmware update runtime contract passed"
