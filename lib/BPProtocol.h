#ifndef BP_PROTOCOL_H
#define BP_PROTOCOL_H

#include <Arduino.h>
#include <stdint.h>

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
  DEVICE_ERROR,
  OUT_OF_RANGE,
  UNSUPPORTED_FORMAT,
  UNSUPPORTED_MODEL,
};

struct BPData {
  String timestamp;
  BPTimestampSource timestampSource = BPTimestampSource::UNSYNCED;
  int systolic = -1;
  int diastolic = -1;
  int pulse = -1;
  int movementCount = 0;
  BPMeasurementQuality quality = BPMeasurementQuality::CLEAN;
  String rawData;
  bool valid = false;
};

struct BPParseResult {
  BPData measurement;
  String transientSubjectId;
  BPParseError error = BPParseError::MALFORMED;
  int deviceErrorCode = 0;

  bool ok() const {
    return error == BPParseError::NONE && measurement.valid;
  }
};

#endif
