#ifndef BP_MEASUREMENT_POLICY_H
#define BP_MEASUREMENT_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <Preferences.h>

#include "BPProtocol.h"

// Presentation-only review policy. It provides deterministic operator cues;
// it does not diagnose a condition or replace clinician review.
struct MeasurementPolicyConfig {
  static constexpr size_t kNameCapacity = 33;

  char policyName[kNameCapacity] = "Clinic adult BP review";
  uint32_t policyVersion = 1;
  int reviewSystolic = 130;
  int reviewDiastolic = 80;
  int reviewPulseLow = 60;
  int reviewPulseHigh = 100;
  int urgentSystolic = 180;
  int urgentDiastolic = 120;
  uint32_t staleAfterMs = 300000U;
};

enum class MeasurementReviewState : uint8_t {
  INVALID = 0,
  WITHIN_REFERENCE,
  REVIEW,
  URGENT,
};

enum class MeasurementFreshnessState : uint8_t {
  INVALID = 0,
  CURRENT,
  STALE,
  HISTORICAL,
  DISCONNECTED,
};

struct MeasurementFreshnessInput {
  bool hasRecord = false;
  bool valid = false;
  bool receivedThisBoot = false;
  bool transportConnected = false;
  uint64_t nowMs = 0;
  uint64_t lastSuccessfulReceiveMs = 0;
  uint32_t staleAfterMs = MeasurementPolicyConfig{}.staleAfterMs;
};

// Extend Arduino's wrapping 32-bit millis clock. Production observes this on
// every main-loop turn, so no browser request is responsible for timekeeping.
class MonotonicMillis64 {
public:
  void observe(uint32_t sampleMs) {
    if (!_observed) {
      _totalMs = sampleMs;
      _lastSampleMs = sampleMs;
      _observed = true;
      return;
    }
    _totalMs += static_cast<uint32_t>(sampleMs - _lastSampleMs);
    _lastSampleMs = sampleMs;
  }

  uint64_t nowMs() const { return _totalMs; }
  bool observed() const { return _observed; }

private:
  uint64_t _totalMs = 0;
  uint32_t _lastSampleMs = 0;
  bool _observed = false;
};

inline bool formatOpaqueSequence(uint64_t value, char* output,
                                 size_t capacity) {
  if (output == nullptr || capacity == 0) return false;
  const int length = snprintf(output, capacity, "%llu",
    static_cast<unsigned long long>(value));
  if (length <= 0 || static_cast<size_t>(length) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

inline bool validMeasurementPolicyName(const char* name) {
  if (name == nullptr) return false;
  size_t length = 0;
  while (length < MeasurementPolicyConfig::kNameCapacity &&
         name[length] != '\0') {
    const uint8_t value = static_cast<uint8_t>(name[length]);
    const bool allowed =
      (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9') || value == ' ' ||
      value == '.' || value == '-' || value == '_' ||
      value == '(' || value == ')';
    if (!allowed) return false;
    ++length;
  }
  return length != 0 && length < MeasurementPolicyConfig::kNameCapacity;
}

inline bool copyMeasurementPolicyName(MeasurementPolicyConfig& policy,
                                      const char* name) {
  if (!validMeasurementPolicyName(name)) return false;
  memset(policy.policyName, 0, sizeof(policy.policyName));
  const size_t length = strlen(name);
  memcpy(policy.policyName, name, length);
  return true;
}

inline bool parseMeasurementPolicyUnsigned(const char* text,
                                           uint32_t& value) {
  if (text == nullptr || *text == '\0') return false;
  uint32_t parsed = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(text[i] - '0');
    if (parsed > (UINT32_MAX - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  value = parsed;
  return true;
}

inline bool validMeasurementPolicy(const MeasurementPolicyConfig& policy) {
  return validMeasurementPolicyName(policy.policyName) &&
         policy.policyVersion != 0 &&
         policy.reviewSystolic >= 60 &&
         policy.reviewSystolic < policy.urgentSystolic &&
         policy.urgentSystolic <= 260 &&
         policy.reviewDiastolic >= 30 &&
         policy.reviewDiastolic < policy.urgentDiastolic &&
         policy.urgentDiastolic <= 215 &&
         policy.reviewPulseLow >= 40 &&
         policy.reviewPulseLow < policy.reviewPulseHigh &&
         policy.reviewPulseHigh <= 180 &&
         policy.staleAfterMs >= 1000U &&
         policy.staleAfterMs <= 86400000U;
}

inline bool measurementPolicyEqual(const MeasurementPolicyConfig& left,
                                   const MeasurementPolicyConfig& right) {
  return strcmp(left.policyName, right.policyName) == 0 &&
         left.policyVersion == right.policyVersion &&
         left.reviewSystolic == right.reviewSystolic &&
         left.reviewDiastolic == right.reviewDiastolic &&
         left.reviewPulseLow == right.reviewPulseLow &&
         left.reviewPulseHigh == right.reviewPulseHigh &&
         left.urgentSystolic == right.urgentSystolic &&
         left.urgentDiastolic == right.urgentDiastolic &&
         left.staleAfterMs == right.staleAfterMs;
}

enum class MeasurementPolicyResult : uint8_t {
  OK = 0,
  INVALID_POLICY,
  STORAGE_FAILURE,
  CORRUPT_STATE,
};

class MeasurementPolicyStore {
public:
  explicit MeasurementPolicyStore(Preferences* preferences)
    : _preferences(preferences) {}

  static constexpr size_t encodedSize() { return kEncodedSize; }

  MeasurementPolicyResult loadOrCreate() {
    uint8_t encoded[kEncodedSize] = {};
    const ReadResult read = readStored(encoded);
    if (read == ReadResult::MISSING) {
      const MeasurementPolicyConfig initial;
      return commit(initial, false);
    }
    if (read != ReadResult::PRESENT) {
      lock();
      return read == ReadResult::INVALID
        ? MeasurementPolicyResult::CORRUPT_STATE
        : MeasurementPolicyResult::STORAGE_FAILURE;
    }
    MeasurementPolicyConfig decoded;
    if (!decode(encoded, sizeof(encoded), decoded)) {
      lock();
      return MeasurementPolicyResult::CORRUPT_STATE;
    }
    apply(decoded);
    return MeasurementPolicyResult::OK;
  }

  MeasurementPolicyResult update(const MeasurementPolicyConfig& candidate) {
    if (!_ready || !validMeasurementPolicy(candidate) ||
        candidate.policyVersion <= _config.policyVersion) {
      return MeasurementPolicyResult::INVALID_POLICY;
    }
    return commit(candidate, true);
  }

  bool ready() const { return _ready; }
  const MeasurementPolicyConfig& config() const { return _config; }

private:
  static constexpr const char* kNamespace = "bp_policy";
  static constexpr const char* kStateKey = "policy_state";
  static constexpr uint8_t kSchemaVersion = 1;
  static constexpr size_t kNameBytes =
    MeasurementPolicyConfig::kNameCapacity - 1;
  static constexpr size_t kCrcOffset = 73;
  static constexpr size_t kEncodedSize = 77;

  enum class ReadResult : uint8_t {
    PRESENT,
    MISSING,
    INVALID,
    STORAGE_ERROR,
  };

  Preferences* _preferences = nullptr;
  MeasurementPolicyConfig _config;
  bool _ready = false;

  static uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0
          ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
      }
    }
    return ~crc;
  }

  static void writeLe32(uint8_t* target, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      target[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
    }
  }

  static uint32_t readLe32(const uint8_t* source) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<uint32_t>(source[i]) << (8U * i);
    }
    return value;
  }

  static void encode(const MeasurementPolicyConfig& policy,
                     uint8_t (&encoded)[kEncodedSize]) {
    memset(encoded, 0, sizeof(encoded));
    encoded[0] = 'B';
    encoded[1] = 'P';
    encoded[2] = 'M';
    encoded[3] = 'P';
    encoded[4] = kSchemaVersion;
    writeLe32(encoded + 8, policy.policyVersion);
    writeLe32(encoded + 12, static_cast<uint32_t>(policy.reviewSystolic));
    writeLe32(encoded + 16, static_cast<uint32_t>(policy.reviewDiastolic));
    writeLe32(encoded + 20, static_cast<uint32_t>(policy.reviewPulseLow));
    writeLe32(encoded + 24, static_cast<uint32_t>(policy.reviewPulseHigh));
    writeLe32(encoded + 28, static_cast<uint32_t>(policy.urgentSystolic));
    writeLe32(encoded + 32, static_cast<uint32_t>(policy.urgentDiastolic));
    writeLe32(encoded + 36, policy.staleAfterMs);
    const size_t nameLength = strlen(policy.policyName);
    encoded[40] = static_cast<uint8_t>(nameLength);
    memcpy(encoded + 41, policy.policyName, nameLength);
    writeLe32(encoded + kCrcOffset, crc32(encoded, kCrcOffset));
  }

  static bool decode(const uint8_t* encoded, size_t length,
                     MeasurementPolicyConfig& policy) {
    if (encoded == nullptr || length != kEncodedSize ||
        encoded[0] != 'B' || encoded[1] != 'P' ||
        encoded[2] != 'M' || encoded[3] != 'P' ||
        encoded[4] != kSchemaVersion || encoded[5] != 0 ||
        encoded[6] != 0 || encoded[7] != 0 ||
        readLe32(encoded + kCrcOffset) != crc32(encoded, kCrcOffset)) {
      return false;
    }
    const size_t nameLength = encoded[40];
    if (nameLength == 0 || nameLength > kNameBytes) return false;
    for (size_t i = nameLength; i < kNameBytes; ++i) {
      if (encoded[41 + i] != 0) return false;
    }
    MeasurementPolicyConfig decoded;
    memset(decoded.policyName, 0, sizeof(decoded.policyName));
    memcpy(decoded.policyName, encoded + 41, nameLength);
    decoded.policyVersion = readLe32(encoded + 8);
    decoded.reviewSystolic = static_cast<int>(readLe32(encoded + 12));
    decoded.reviewDiastolic = static_cast<int>(readLe32(encoded + 16));
    decoded.reviewPulseLow = static_cast<int>(readLe32(encoded + 20));
    decoded.reviewPulseHigh = static_cast<int>(readLe32(encoded + 24));
    decoded.urgentSystolic = static_cast<int>(readLe32(encoded + 28));
    decoded.urgentDiastolic = static_cast<int>(readLe32(encoded + 32));
    decoded.staleAfterMs = readLe32(encoded + 36);
    if (!validMeasurementPolicy(decoded)) return false;
    policy = decoded;
    return true;
  }

  ReadResult readStored(uint8_t encoded[kEncodedSize]) const {
    if (_preferences == nullptr ||
        !_preferences->begin(kNamespace, false)) {
      return ReadResult::STORAGE_ERROR;
    }
    if (!_preferences->isKey(kStateKey)) {
      _preferences->end();
      return ReadResult::MISSING;
    }
    if (_preferences->getType(kStateKey) != PT_BLOB ||
        _preferences->getBytesLength(kStateKey) != kEncodedSize) {
      _preferences->end();
      return ReadResult::INVALID;
    }
    const size_t read = _preferences->getBytes(
      kStateKey, encoded, kEncodedSize);
    _preferences->end();
    return read == kEncodedSize ? ReadResult::PRESENT : ReadResult::INVALID;
  }

  bool writeStored(const uint8_t encoded[kEncodedSize]) {
    if (_preferences == nullptr ||
        !_preferences->begin(kNamespace, false)) {
      return false;
    }
    const size_t written = _preferences->putBytes(
      kStateKey, encoded, kEncodedSize);
    _preferences->end();
    return written == kEncodedSize;
  }

  static bool encodedEqual(const uint8_t* left, const uint8_t* right) {
    uint8_t difference = 0;
    for (size_t i = 0; i < kEncodedSize; ++i) {
      difference |= left[i] ^ right[i];
    }
    return difference == 0;
  }

  void apply(const MeasurementPolicyConfig& policy) {
    _config = policy;
    _ready = true;
  }

  void lock() {
    memset(&_config, 0, sizeof(_config));
    _ready = false;
  }

  MeasurementPolicyResult commit(const MeasurementPolicyConfig& candidate,
                                 bool oldExists) {
    uint8_t oldEncoded[kEncodedSize] = {};
    uint8_t nextEncoded[kEncodedSize] = {};
    uint8_t observed[kEncodedSize] = {};
    if (oldExists) encode(_config, oldEncoded);
    encode(candidate, nextEncoded);
    (void)writeStored(nextEncoded);
    const ReadResult read = readStored(observed);
    if (read == ReadResult::PRESENT &&
        encodedEqual(observed, nextEncoded)) {
      MeasurementPolicyConfig verified;
      if (!decode(observed, sizeof(observed), verified)) {
        lock();
        return MeasurementPolicyResult::CORRUPT_STATE;
      }
      apply(verified);
      return MeasurementPolicyResult::OK;
    }
    if (oldExists && read == ReadResult::PRESENT &&
        encodedEqual(observed, oldEncoded)) {
      return MeasurementPolicyResult::STORAGE_FAILURE;
    }
    if (!oldExists && read == ReadResult::MISSING) {
      lock();
      return MeasurementPolicyResult::STORAGE_FAILURE;
    }
    lock();
    return read == ReadResult::INVALID
      ? MeasurementPolicyResult::CORRUPT_STATE
      : MeasurementPolicyResult::STORAGE_FAILURE;
  }
};

inline bool validMeasurementForReview(const BPData& value) {
  return value.valid &&
         value.systolic >= 60 && value.systolic <= 260 &&
         value.diastolic >= 30 && value.diastolic <= 215 &&
         value.pulse >= 40 && value.pulse <= 180;
}

inline MeasurementReviewState classifyMeasurement(
    const BPData& value,
    const MeasurementPolicyConfig& policy) {
  if (!validMeasurementPolicy(policy) ||
      !validMeasurementForReview(value)) {
    return MeasurementReviewState::INVALID;
  }
  if (value.systolic > policy.urgentSystolic ||
      value.diastolic > policy.urgentDiastolic) {
    return MeasurementReviewState::URGENT;
  }
  if (value.systolic >= policy.reviewSystolic ||
      value.diastolic >= policy.reviewDiastolic ||
      value.pulse < policy.reviewPulseLow ||
      value.pulse > policy.reviewPulseHigh) {
    return MeasurementReviewState::REVIEW;
  }
  return MeasurementReviewState::WITHIN_REFERENCE;
}

inline const char* measurementReviewCode(MeasurementReviewState state) {
  switch (state) {
    case MeasurementReviewState::WITHIN_REFERENCE: return "within_reference";
    case MeasurementReviewState::REVIEW:           return "review";
    case MeasurementReviewState::URGENT:           return "urgent";
    case MeasurementReviewState::INVALID:
    default:                                       return "invalid";
  }
}

inline const char* measurementReviewLabel(MeasurementReviewState state) {
  switch (state) {
    case MeasurementReviewState::WITHIN_REFERENCE:
      return "未達設定複核門檻";
    case MeasurementReviewState::REVIEW:
      return "達到設定複核門檻";
    case MeasurementReviewState::URGENT:
      return "達到緊急處置提示門檻";
    case MeasurementReviewState::INVALID:
    default:
      return "量測值不可用";
  }
}

inline MeasurementFreshnessState measurementFreshness(
    const MeasurementFreshnessInput& input) {
  if (!input.hasRecord || !input.valid) {
    return MeasurementFreshnessState::INVALID;
  }
  // A reboot removes receipt-time provenance. Durable records remain useful
  // history but can never masquerade as a live measurement.
  if (!input.receivedThisBoot) {
    return MeasurementFreshnessState::HISTORICAL;
  }
  if (!input.transportConnected) {
    return MeasurementFreshnessState::DISCONNECTED;
  }
  if (input.nowMs < input.lastSuccessfulReceiveMs) {
    return MeasurementFreshnessState::STALE;
  }
  const uint64_t age = input.nowMs - input.lastSuccessfulReceiveMs;
  if (input.staleAfterMs == 0 || age >= input.staleAfterMs) {
    return MeasurementFreshnessState::STALE;
  }
  return MeasurementFreshnessState::CURRENT;
}

inline const char* measurementFreshnessCode(
    MeasurementFreshnessState state) {
  switch (state) {
    case MeasurementFreshnessState::CURRENT:      return "current";
    case MeasurementFreshnessState::STALE:        return "stale";
    case MeasurementFreshnessState::HISTORICAL:   return "historical";
    case MeasurementFreshnessState::DISCONNECTED: return "disconnected";
    case MeasurementFreshnessState::INVALID:
    default:                                      return "invalid";
  }
}

inline const char* measurementFreshnessLabel(
    MeasurementFreshnessState state) {
  switch (state) {
    case MeasurementFreshnessState::CURRENT:
      return "本次開機的新量測";
    case MeasurementFreshnessState::STALE:
      return "量測已逾時，請重新量測";
    case MeasurementFreshnessState::HISTORICAL:
      return "開機前歷史量測";
    case MeasurementFreshnessState::DISCONNECTED:
      return "資料通道已中斷，顯示最後量測";
    case MeasurementFreshnessState::INVALID:
    default:
      return "尚無可用量測";
  }
}

inline const char* timestampSourceCode(BPTimestampSource source) {
  switch (source) {
    case BPTimestampSource::DEVICE:          return "device";
    case BPTimestampSource::SYSTEM:          return "system";
    case BPTimestampSource::LEGACY_SYSTEM:   return "legacy_system";
    case BPTimestampSource::LEGACY_UNSYNCED: return "legacy_unsynced";
    case BPTimestampSource::UNSYNCED:
    default:                                 return "unsynced";
  }
}

inline const char* measurementQualityCode(BPMeasurementQuality quality) {
  return quality == BPMeasurementQuality::MOTION ? "movement" : "clean";
}

inline const char* measurementReferencePolicyName() {
  return "血壓參考：2025 AHA/ACC 成人高血壓指引；實際門檻與脈搏規則由診所設定";
}

inline const char* repeatedMeasurementGuidance() {
  return "請依診所流程靜坐後再量測一次，兩次量測至少間隔 1 分鐘；"
         "系統會分別保留結果，不會自動平均。";
}

inline const char* supportedMeasurementProtocol() {
  return "OMRON HBP-9030 USB function item 32 format 5";
}

#endif
