#include "../lib/FirmwareUpdateRuntime.h"

#include <cstring>

#include <mbedtls/pk.h>
#include <mbedtls/md.h>

#include "../lib/ReleaseTrustAnchor.h"

constexpr const char* FirmwareUpdateRuntime::kSequenceKeys[2];

namespace {

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

void secureZero(void* target, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
  while (length-- != 0) *bytes++ = 0;
}

}  // namespace

FirmwareUpdateRuntime::FirmwareUpdateRuntime(Preferences* preferences)
  : _preferences(preferences),
    _sequenceStorage{this, &FirmwareUpdateRuntime::readSlotThunk,
                     &FirmwareUpdateRuntime::writeSlotThunk},
    _sequenceStore(_sequenceStorage),
    _pendingPolicy(&_sequenceStore) {
  mbedtls_sha256_init(&_sha);
}

FirmwareUpdateRuntime::~FirmwareUpdateRuntime() {
  abortStream();
  mbedtls_sha256_free(&_sha);
  clearAuthorization();
}

bool FirmwareUpdateRuntime::trustAnchorConfigured() const {
  const size_t encodedLength = strlen(kReleasePublicKeyDerHex);
  if (encodedLength < 2 || (encodedLength & 1U) != 0 ||
      encodedLength / 2U > kMaxTrustAnchorBytes) {
    return false;
  }
  for (size_t i = 0; i < encodedLength; ++i) {
    if (hexNibble(kReleasePublicKeyDerHex[i]) < 0) return false;
  }
  return true;
}

uint64_t FirmwareUpdateRuntime::highestAcceptedSequence() const {
  return _sequenceStore.ready() ? _sequenceStore.sequence() : 0;
}

bool FirmwareUpdateRuntime::readSlotThunk(
    void* context, uint8_t slot, uint8_t* bytes, size_t length,
    bool& present) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->readSlot(
      slot, bytes, length, present);
}

bool FirmwareUpdateRuntime::writeSlotThunk(
    void* context, uint8_t slot, const uint8_t* bytes, size_t length) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->writeSlot(
      slot, bytes, length);
}

bool FirmwareUpdateRuntime::readSlot(
    uint8_t slot, uint8_t* bytes, size_t length, bool& present) {
  present = false;
  if (_preferences == nullptr || slot > 1 || bytes == nullptr ||
      length != bp_update::kSequenceSlotBytes ||
      !_preferences->begin(kNamespace, false)) {
    return false;
  }
  const char* key = kSequenceKeys[slot];
  present = _preferences->isKey(key);
  if (!present) {
    _preferences->end();
    return true;
  }
  if (_preferences->getType(key) != PT_BLOB ||
      _preferences->getBytesLength(key) != length) {
    memset(bytes, 0, length);
    _preferences->end();
    return true;
  }
  const bool read = _preferences->getBytes(key, bytes, length) == length;
  _preferences->end();
  return read;
}

bool FirmwareUpdateRuntime::writeSlot(
    uint8_t slot, const uint8_t* bytes, size_t length) {
  if (_preferences == nullptr || slot > 1 || bytes == nullptr ||
      length != bp_update::kSequenceSlotBytes ||
      !_preferences->begin(kNamespace, false)) {
    return false;
  }
  const bool written =
    _preferences->putBytes(kSequenceKeys[slot], bytes, length) == length;
  _preferences->end();
  return written;
}

bool FirmwareUpdateRuntime::begin(bool allowFirstInitialization) {
  if (_ready || _preferences == nullptr) return false;
  _lastResult = _sequenceStore.load();
  if (_lastResult == bp_update::Result::STORAGE_UNINITIALIZED &&
      allowFirstInitialization) {
    _lastResult = _sequenceStore.initialize(0);
  }
  if (_lastResult != bp_update::Result::OK) return false;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
  if (running == nullptr ||
      esp_ota_get_state_partition(running, &imageState) != ESP_OK) {
    _lastResult = bp_update::Result::STORAGE_FAILURE;
    return false;
  }
  bool receiptPresent = false;
  _lastResult = loadPendingReceipt(receiptPresent);
  if (_lastResult != bp_update::Result::OK) return false;

  _pendingVerify = imageState == ESP_OTA_IMG_PENDING_VERIFY;
  if (_pendingVerify) {
    if (!receiptPresent) {
      _lastResult = bp_update::Result::STORAGE_CORRUPT;
      return false;
    }
    bp_update::SignatureVerifier verifier{
      this, &FirmwareUpdateRuntime::signatureVerifyThunk,
      trustAnchorConfigured()
    };
    _lastResult = bp_update::authorizeManifest(
      reinterpret_cast<const char*>(_receipt.manifest),
      _receipt.manifestLength, _receipt.signature, _receipt.signatureLength,
      "esp32:esp32:esp32s3", _sequenceStore.sequence(), verifier,
      _authorized);
    if (_lastResult != bp_update::Result::OK) return false;
    _lastResult = _pendingPolicy.beginPending(_authorized);
    if (_lastResult != bp_update::Result::OK) return false;
    _hasAuthorization = true;
  } else if (receiptPresent && !clearPendingReceipt()) {
    _lastResult = bp_update::Result::STORAGE_FAILURE;
    return false;
  }
  _ready = true;
  _lastResult = bp_update::Result::OK;
  return true;
}

bool FirmwareUpdateRuntime::signatureVerifyThunk(
    void* context, const uint8_t* manifest, size_t manifestLength,
    const uint8_t* signature, size_t signatureLength) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->verifySignature(
      manifest, manifestLength, signature, signatureLength);
}

bool FirmwareUpdateRuntime::verifySignature(
    const uint8_t* manifest, size_t manifestLength,
    const uint8_t* signature, size_t signatureLength) {
  if (!trustAnchorConfigured() || manifest == nullptr || manifestLength == 0 ||
      signature == nullptr || signatureLength == 0 ||
      signatureLength > bp_update::kPendingSignatureMaxBytes) {
    return false;
  }
  uint8_t anchor[kMaxTrustAnchorBytes] = {};
  const size_t anchorLength = strlen(kReleasePublicKeyDerHex) / 2U;
  for (size_t i = 0; i < anchorLength; ++i) {
    const int high = hexNibble(kReleasePublicKeyDerHex[i * 2]);
    const int low = hexNibble(kReleasePublicKeyDerHex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      secureZero(anchor, sizeof(anchor));
      return false;
    }
    anchor[i] = static_cast<uint8_t>((high << 4) | low);
  }
  uint8_t digest[32] = {};
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const bool hashed =
    mbedtls_sha256_starts(&sha, 0) == 0 &&
    mbedtls_sha256_update(&sha, manifest, manifestLength) == 0 &&
    mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  if (!hashed) {
    secureZero(anchor, sizeof(anchor));
    secureZero(digest, sizeof(digest));
    return false;
  }
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const bool parsed =
    mbedtls_pk_parse_public_key(&key, anchor, anchorLength) == 0 &&
    mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) &&
    mbedtls_pk_get_bitlen(&key) == 256;
  const bool verified = parsed &&
    mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                      signature, signatureLength) == 0;
  mbedtls_pk_free(&key);
  secureZero(anchor, sizeof(anchor));
  secureZero(digest, sizeof(digest));
  return verified;
}

bp_update::Result FirmwareUpdateRuntime::authorizeUpdate(
    const char* canonicalManifest, size_t manifestLength,
    const uint8_t* signature, size_t signatureLength) {
  clearAuthorization();
  if (!_ready || _pendingVerify || _stagedForReboot ||
      !_sequenceStore.ready()) {
    _lastResult = bp_update::Result::STORAGE_FAILURE;
    return _lastResult;
  }
  bp_update::SignatureVerifier verifier{
    this, &FirmwareUpdateRuntime::signatureVerifyThunk,
    trustAnchorConfigured()
  };
  _lastResult = bp_update::authorizeManifest(
    canonicalManifest, manifestLength, signature, signatureLength,
    "esp32:esp32:esp32s3", _sequenceStore.sequence(), verifier, _authorized);
  if (_lastResult != bp_update::Result::OK) return _lastResult;
  _lastResult = bp_update::makePendingUpdateReceipt(
    reinterpret_cast<const uint8_t*>(canonicalManifest), manifestLength,
    signature, signatureLength, _receipt);
  _hasAuthorization = _lastResult == bp_update::Result::OK;
  return _lastResult;
}

bp_http::StreamConsumerCallbacks FirmwareUpdateRuntime::streamCallbacks() {
  return {this, &FirmwareUpdateRuntime::streamBeginThunk,
          &FirmwareUpdateRuntime::streamWriteThunk,
          &FirmwareUpdateRuntime::streamFinishThunk,
          &FirmwareUpdateRuntime::streamAbortThunk};
}

bool FirmwareUpdateRuntime::streamBeginThunk(void* context,
                                             uint32_t expectedLength) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->beginStream(expectedLength);
}

bool FirmwareUpdateRuntime::streamWriteThunk(
    void* context, const uint8_t* bytes, size_t length) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->writeStream(bytes, length);
}

bool FirmwareUpdateRuntime::streamFinishThunk(void* context) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->finishStream();
}

void FirmwareUpdateRuntime::streamAbortThunk(void* context) {
  if (context != nullptr) static_cast<FirmwareUpdateRuntime*>(context)->abortStream();
}

bool FirmwareUpdateRuntime::beginStream(uint32_t expectedLength) {
  if (!_ready || !_hasAuthorization || _pendingVerify || _stagedForReboot ||
      expectedLength != _authorized.manifest().artifactSize) {
    clearAuthorization();
    return false;
  }
  bp_update::ArtifactCallbacks callbacks{
    this, &FirmwareUpdateRuntime::artifactBeginThunk,
    &FirmwareUpdateRuntime::artifactWriteThunk,
    &FirmwareUpdateRuntime::artifactFinishThunk,
    &FirmwareUpdateRuntime::artifactAbortThunk
  };
  _lastResult = _artifactPolicy.begin(_authorized, callbacks);
  return _lastResult == bp_update::Result::OK;
}

bool FirmwareUpdateRuntime::writeStream(const uint8_t* bytes, size_t length) {
  _lastResult = _artifactPolicy.write(bytes, length);
  return _lastResult == bp_update::Result::OK;
}

bool FirmwareUpdateRuntime::finishStream() {
  _lastResult = _artifactPolicy.finish();
  if (_lastResult != bp_update::Result::OK || _updatePartition == nullptr ||
      !_otaEnded || !storePendingReceipt()) {
    abortStream();
    return false;
  }
  if (esp_ota_set_boot_partition(_updatePartition) != ESP_OK) {
    (void)clearPendingReceipt();
    abortStream();
    _lastResult = bp_update::Result::ARTIFACT_WRITE;
    return false;
  }
  _stagedForReboot = true;
  _updatePartition = nullptr;
  clearAuthorization();
  _lastResult = bp_update::Result::OK;
  return true;
}

void FirmwareUpdateRuntime::abortStream() {
  _artifactPolicy.abort();
  abortArtifact();
  if (!_stagedForReboot) clearAuthorization();
}

bool FirmwareUpdateRuntime::artifactBeginThunk(void* context,
                                               uint32_t expectedLength) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->beginArtifact(expectedLength);
}

bool FirmwareUpdateRuntime::artifactWriteThunk(
    void* context, const uint8_t* bytes, size_t length) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->writeArtifact(bytes, length);
}

bool FirmwareUpdateRuntime::artifactFinishThunk(void* context,
                                                uint8_t digest[32]) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->finishArtifact(digest);
}

void FirmwareUpdateRuntime::artifactAbortThunk(void* context) {
  if (context != nullptr) static_cast<FirmwareUpdateRuntime*>(context)->abortArtifact();
}

bool FirmwareUpdateRuntime::beginArtifact(uint32_t expectedLength) {
  abortArtifact();
  _updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (_updatePartition == nullptr || expectedLength == 0 ||
      expectedLength > _updatePartition->size ||
      _updatePartition == esp_ota_get_running_partition()) {
    _updatePartition = nullptr;
    return false;
  }
  mbedtls_sha256_init(&_sha);
  if (mbedtls_sha256_starts(&_sha, 0) != 0) {
    _updatePartition = nullptr;
    return false;
  }
  _shaActive = true;
  if (esp_ota_begin(_updatePartition, expectedLength, &_otaHandle) != ESP_OK) {
    abortArtifact();
    return false;
  }
  _otaActive = true;
  _otaEnded = false;
  return true;
}

bool FirmwareUpdateRuntime::writeArtifact(const uint8_t* bytes, size_t length) {
  if (!_otaActive || !_shaActive || bytes == nullptr || length == 0) return false;
  if (esp_ota_write(_otaHandle, bytes, length) != ESP_OK) return false;
  return mbedtls_sha256_update(&_sha, bytes, length) == 0;
}

bool FirmwareUpdateRuntime::finishArtifact(uint8_t digest[32]) {
  if (!_otaActive || !_shaActive || digest == nullptr) return false;
  const bool hashed = mbedtls_sha256_finish(&_sha, digest) == 0;
  mbedtls_sha256_free(&_sha);
  _shaActive = false;
  const esp_err_t ended = esp_ota_end(_otaHandle);
  _otaActive = false;
  _otaHandle = 0;
  _otaEnded = ended == ESP_OK;
  return hashed && _otaEnded;
}

void FirmwareUpdateRuntime::abortArtifact() {
  if (_otaActive) (void)esp_ota_abort(_otaHandle);
  _otaActive = false;
  _otaHandle = 0;
  if (_shaActive) mbedtls_sha256_free(&_sha);
  _shaActive = false;
  if (_updatePartition != nullptr && !_stagedForReboot) {
    (void)esp_partition_erase_range(_updatePartition, 0, 4096);
  }
  _otaEnded = false;
  if (!_stagedForReboot) _updatePartition = nullptr;
}

bool FirmwareUpdateRuntime::storePendingReceipt() {
  const bp_update::PendingUpdateReceipt candidate = _receipt;
  uint8_t encoded[bp_update::kPendingReceiptBytes] = {};
  if (!bp_update::encodePendingUpdateReceipt(
        _receipt, encoded, sizeof(encoded)) ||
      !_preferences->begin(kNamespace, false)) {
    secureZero(encoded, sizeof(encoded));
    return false;
  }
  const bool wrote =
    _preferences->putBytes(kPendingKey, encoded, sizeof(encoded)) ==
      sizeof(encoded);
  _preferences->end();
  bool present = false;
  const bp_update::Result loaded = loadPendingReceipt(present);
  const bool same = present &&
    _receipt.manifestLength == candidate.manifestLength &&
    _receipt.signatureLength == candidate.signatureLength &&
    memcmp(_receipt.manifest, candidate.manifest,
           candidate.manifestLength) == 0 &&
    memcmp(_receipt.signature, candidate.signature,
           candidate.signatureLength) == 0;
  secureZero(encoded, sizeof(encoded));
  return (wrote || same) && loaded == bp_update::Result::OK && same;
}

bp_update::Result FirmwareUpdateRuntime::loadPendingReceipt(bool& present) {
  present = false;
  secureZero(&_receipt, sizeof(_receipt));
  if (_preferences == nullptr || !_preferences->begin(kNamespace, false)) {
    return bp_update::Result::STORAGE_FAILURE;
  }
  present = _preferences->isKey(kPendingKey);
  if (!present) {
    _preferences->end();
    return bp_update::Result::OK;
  }
  if (_preferences->getType(kPendingKey) != PT_BLOB ||
      _preferences->getBytesLength(kPendingKey) !=
        bp_update::kPendingReceiptBytes) {
    _preferences->end();
    return bp_update::Result::STORAGE_CORRUPT;
  }
  uint8_t encoded[bp_update::kPendingReceiptBytes] = {};
  const bool read = _preferences->getBytes(
    kPendingKey, encoded, sizeof(encoded)) == sizeof(encoded);
  _preferences->end();
  const bp_update::Result result = read
    ? bp_update::decodePendingUpdateReceipt(
        encoded, sizeof(encoded), _receipt)
    : bp_update::Result::STORAGE_FAILURE;
  secureZero(encoded, sizeof(encoded));
  return result;
}

bool FirmwareUpdateRuntime::clearPendingReceipt() {
  if (_preferences == nullptr || !_preferences->begin(kNamespace, false)) {
    return false;
  }
  const bool cleared = !_preferences->isKey(kPendingKey) ||
                       _preferences->remove(kPendingKey);
  const bool absent = !_preferences->isKey(kPendingKey);
  _preferences->end();
  if (cleared && absent) secureZero(&_receipt, sizeof(_receipt));
  return cleared && absent;
}

bool FirmwareUpdateRuntime::confirmBootThunk(void* context) {
  return context != nullptr &&
    static_cast<FirmwareUpdateRuntime*>(context)->confirmBoot();
}

bool FirmwareUpdateRuntime::confirmBoot() {
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

bool FirmwareUpdateRuntime::confirmPendingBoot(
    const bp_update::HealthSnapshot& health) {
  if (!_ready || !_pendingVerify) return true;
  _lastResult = _pendingPolicy.evaluate(
    health, this, &FirmwareUpdateRuntime::confirmBootThunk);
  if (_lastResult != bp_update::Result::OK) {
    rollbackIfPending();
    return false;
  }
  if (!clearPendingReceipt()) {
    _lastResult = bp_update::Result::STORAGE_FAILURE;
    return false;
  }
  _pendingVerify = false;
  clearAuthorization();
  return true;
}

void FirmwareUpdateRuntime::rollbackIfPending() {
  if (_pendingVerify) {
    (void)esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void FirmwareUpdateRuntime::clearAuthorization() {
  _authorized = bp_update::AuthorizedManifest{};
  secureZero(&_receipt, sizeof(_receipt));
  _hasAuthorization = false;
}
