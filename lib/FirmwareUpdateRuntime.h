#ifndef BP_FIRMWARE_UPDATE_RUNTIME_H
#define BP_FIRMWARE_UPDATE_RUNTIME_H

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "BoundedStreamConsumer.h"
#include "FirmwareUpdatePolicy.h"

class FirmwareUpdateRuntime {
public:
  explicit FirmwareUpdateRuntime(Preferences* preferences);
  FirmwareUpdateRuntime(const FirmwareUpdateRuntime&) = delete;
  FirmwareUpdateRuntime& operator=(const FirmwareUpdateRuntime&) = delete;
  ~FirmwareUpdateRuntime();

  bool begin(bool allowFirstInitialization);
  bool ready() const { return _ready; }
  bool trustAnchorConfigured() const;
  bool pendingVerify() const { return _pendingVerify; }
  bool stagedForReboot() const { return _stagedForReboot; }
  uint64_t highestAcceptedSequence() const;
  bp_update::Result lastResult() const { return _lastResult; }

  bp_update::Result authorizeUpdate(
    const char* canonicalManifest, size_t manifestLength,
    const uint8_t* signature, size_t signatureLength);
  bp_http::StreamConsumerCallbacks streamCallbacks();

  bool confirmPendingBoot(const bp_update::HealthSnapshot& health);
  void rollbackIfPending();

private:
  static constexpr const char* kNamespace = "bp_update";
  static constexpr const char* kSequenceKeys[2] = {"seq_a", "seq_b"};
  static constexpr const char* kPendingKey = "pending";
  static constexpr size_t kMaxTrustAnchorBytes = 128;

  Preferences* _preferences = nullptr;
  bp_update::SequenceStorage _sequenceStorage{};
  bp_update::MonotonicSequenceStore _sequenceStore;
  bp_update::PendingBootPolicy _pendingPolicy;
  bp_update::AuthorizedManifest _authorized{};
  bp_update::PendingUpdateReceipt _receipt{};
  bp_update::ArtifactStreamPolicy _artifactPolicy;
  mbedtls_sha256_context _sha{};
  const esp_partition_t* _updatePartition = nullptr;
  esp_ota_handle_t _otaHandle = 0;
  bp_update::Result _lastResult = bp_update::Result::STORAGE_UNINITIALIZED;
  bool _shaActive = false;
  bool _otaActive = false;
  bool _otaEnded = false;
  bool _ready = false;
  bool _hasAuthorization = false;
  bool _pendingVerify = false;
  bool _stagedForReboot = false;

  static bool readSlotThunk(void* context, uint8_t slot, uint8_t* bytes,
                            size_t length, bool& present);
  static bool writeSlotThunk(void* context, uint8_t slot,
                             const uint8_t* bytes, size_t length);
  bool readSlot(uint8_t slot, uint8_t* bytes, size_t length, bool& present);
  bool writeSlot(uint8_t slot, const uint8_t* bytes, size_t length);

  static bool signatureVerifyThunk(void* context, const uint8_t* manifest,
                                   size_t manifestLength,
                                   const uint8_t* signature,
                                   size_t signatureLength);
  bool verifySignature(const uint8_t* manifest, size_t manifestLength,
                       const uint8_t* signature, size_t signatureLength);

  static bool streamBeginThunk(void* context, uint32_t expectedLength);
  static bool streamWriteThunk(void* context, const uint8_t* bytes,
                               size_t length);
  static bool streamFinishThunk(void* context);
  static void streamAbortThunk(void* context);
  bool beginStream(uint32_t expectedLength);
  bool writeStream(const uint8_t* bytes, size_t length);
  bool finishStream();
  void abortStream();

  static bool artifactBeginThunk(void* context, uint32_t expectedLength);
  static bool artifactWriteThunk(void* context, const uint8_t* bytes,
                                 size_t length);
  static bool artifactFinishThunk(void* context, uint8_t digest[32]);
  static void artifactAbortThunk(void* context);
  bool beginArtifact(uint32_t expectedLength);
  bool writeArtifact(const uint8_t* bytes, size_t length);
  bool finishArtifact(uint8_t digest[32]);
  void abortArtifact();

  bool storePendingReceipt();
  bp_update::Result loadPendingReceipt(bool& present);
  bool clearPendingReceipt();
  static bool confirmBootThunk(void* context);
  bool confirmBoot();
  void clearAuthorization();
};

#endif
