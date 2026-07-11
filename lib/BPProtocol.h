#ifndef BP_PROTOCOL_H
#define BP_PROTOCOL_H

#include <Arduino.h>
#include <stdint.h>
#include <utility>

enum class BPTimestampSource : uint8_t {
  UNSYNCED = 0,
  DEVICE,
  SYSTEM,
  LEGACY_SYSTEM,
  LEGACY_UNSYNCED,
};

enum class BPMeasurementQuality : uint8_t {
  CLEAN = 0,
  MOTION,
};

enum class BPParseError : uint8_t {
  NONE = 0,
  MALFORMED,
  INVALID_TIMESTAMP,
  DEVICE_ERROR,
  OUT_OF_RANGE,
  UNSUPPORTED_FORMAT,
  UNSUPPORTED_MODEL,
};

struct BPData {
  // Opaque, non-identifying ordering/grouping tokens. Persistence owns
  // recordSequence; a zero sessionSequence becomes a one-record session.
  uint64_t recordSequence = 0;
  uint64_t sessionSequence = 0;
  String timestamp;
  BPTimestampSource timestampSource = BPTimestampSource::UNSYNCED;
  int systolic = -1;
  int diastolic = -1;
  int pulse = -1;
  int movementCount = 0;
  BPMeasurementQuality quality = BPMeasurementQuality::CLEAN;
  bool valid = false;
};

struct BPParseResult {
  BPData measurement;
  String transientSubjectId;
  BPParseError error = BPParseError::MALFORMED;
  int deviceErrorCode = 0;

  BPParseResult() = default;
  BPParseResult(const BPParseResult&) = delete;
  BPParseResult& operator=(const BPParseResult&) = delete;

  BPParseResult(BPParseResult&& other) noexcept
    : measurement(std::move(other.measurement)),
      transientSubjectId(std::move(other.transientSubjectId)),
      error(other.error),
      deviceErrorCode(other.deviceErrorCode) {}

  BPParseResult& operator=(BPParseResult&& other) noexcept {
    if (this == &other) return *this;
    secureClearTransientId();
    measurement = std::move(other.measurement);
    transientSubjectId = std::move(other.transientSubjectId);
    error = other.error;
    deviceErrorCode = other.deviceErrorCode;
    return *this;
  }

  ~BPParseResult() { secureClearTransientId(); }

  bool ok() const {
    return error == BPParseError::NONE && measurement.valid;
  }

private:
  void secureClearTransientId() {
    for (unsigned int i = 0; i < transientSubjectId.length(); ++i) {
      transientSubjectId.setCharAt(i, '\0');
    }
    transientSubjectId.remove(0);
  }
};

inline const char* bpParseErrorCode(BPParseError error) {
  switch (error) {
    case BPParseError::NONE:               return "valid";
    case BPParseError::MALFORMED:          return "malformed";
    case BPParseError::INVALID_TIMESTAMP:  return "invalid_timestamp";
    case BPParseError::DEVICE_ERROR:       return "device_error";
    case BPParseError::OUT_OF_RANGE:       return "out_of_range";
    case BPParseError::UNSUPPORTED_FORMAT: return "unsupported_format";
    case BPParseError::UNSUPPORTED_MODEL:  return "unsupported_model";
    default:                               return "malformed";
  }
}

#endif
