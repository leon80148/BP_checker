#ifndef BP_RecordManager_h
#define BP_RecordManager_h

#include <Arduino.h>
#include <Preferences.h>

#include <limits.h>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <utility>

#include "BP_Parser.h"

// Crash-consistent record storage. v3_state is the only activation point;
// each v3_N slot is independently atomic and self-validating. No native C++
// struct is written to NVS, so padding, alignment, and endianness are fixed.
class BP_RecordManager {
private:
  static constexpr const char* kNamespace = "bp_records";
  static constexpr const char* kStateKey = "v3_state";
  static constexpr uint8_t kSchemaVersion = 3;
  static constexpr size_t kStateSize = 17;
  // Canonical device/legacy-system timestamps are exactly 19 bytes; the only
  // shorter accepted value is the fixed legacy-unsynced sentinel.
  static constexpr size_t kMaxTimestampBytes = 19;
  static constexpr size_t kSlotFixedSize = 45;
  static constexpr size_t kMaxSlotSize = kSlotFixedSize + kMaxTimestampBytes;

  const int _maxRecords;
  BPData* _records;
  int _recordCount = 0;
  uint32_t _generation = 1;
  uint64_t _nextSequence = 1;
  bool _stateReady = false;
  bool _storageHealthy = false;
  bool _sequenceExhausted = false;
  uint64_t _lastSuccessfulRecordSequence = 0;
  uint32_t _lastSuccessfulReceiveMs = 0;
  Preferences _preferences;

  struct Candidate {
    BPData record;
    bool duplicate = false;
  };

  static uint32_t crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1U) != 0
          ? (crc >> 1U) ^ 0xedb88320U
          : crc >> 1U;
      }
    }
    return ~crc;
  }

  static void writeLe32(uint8_t* target, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      target[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
    }
  }

  static void writeLe64(uint8_t* target, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
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

  static uint64_t readLe64(const uint8_t* source) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<uint64_t>(source[i]) << (8U * i);
    }
    return value;
  }

  static int32_t readLeI32(const uint8_t* source) {
    const uint32_t raw = readLe32(source);
    if (raw <= static_cast<uint32_t>(INT32_MAX)) {
      return static_cast<int32_t>(raw);
    }
    return -1 - static_cast<int32_t>(UINT32_MAX - raw);
  }

  static void encodeState(uint32_t generation, uint64_t nextSequenceFloor,
                          uint8_t (&encoded)[kStateSize]) {
    // Offsets: version[0], generation LE32[1..4], next-sequence floor
    // LE64[5..12], CRC32 LE[13..16] covering bytes [0..12]. The floor is
    // the next allocatable sequence lower bound: fresh=1; clear retains the
    // current _nextSequence; load derives max(floor, maxSlot+1).
    encoded[0] = kSchemaVersion;
    writeLe32(encoded + 1, generation);
    writeLe64(encoded + 5, nextSequenceFloor);
    writeLe32(encoded + 13, crc32(encoded, 13));
  }

  static bool decodeState(const uint8_t* encoded, size_t length,
                          uint32_t& generation, uint64_t& nextSequenceFloor) {
    if (encoded == nullptr || length != kStateSize ||
        encoded[0] != kSchemaVersion ||
        readLe32(encoded + 13) != crc32(encoded, 13)) {
      return false;
    }
    generation = readLe32(encoded + 1);
    nextSequenceFloor = readLe64(encoded + 5);
    return generation != 0 && nextSequenceFloor != 0;
  }

  static bool isDigit(char value) {
    return value >= '0' && value <= '9';
  }

  static bool leapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  }

  static bool validStructuredTimestamp(const String& timestamp) {
    if (timestamp.length() != 19) return false;
    const int separators[] = {4, 7, 10, 13, 16};
    const char expected[] = {'-', '-', ' ', ':', ':'};
    for (int i = 0; i < 5; ++i) {
      if (timestamp.charAt(separators[i]) != expected[i]) return false;
    }
    for (unsigned int i = 0; i < timestamp.length(); ++i) {
      bool separator = false;
      for (int j = 0; j < 5; ++j) {
        if (i == static_cast<unsigned int>(separators[j])) separator = true;
      }
      if (!separator && !isDigit(timestamp.charAt(i))) return false;
    }
    const int year = static_cast<int>(timestamp.substring(0, 4).toInt());
    const int month = static_cast<int>(timestamp.substring(5, 7).toInt());
    const int day = static_cast<int>(timestamp.substring(8, 10).toInt());
    const int hour = static_cast<int>(timestamp.substring(11, 13).toInt());
    const int minute = static_cast<int>(timestamp.substring(14, 16).toInt());
    const int second = static_cast<int>(timestamp.substring(17, 19).toInt());
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
      return false;
    }
    static const int days[] = {0, 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
    const int maxDay = month == 2 && leapYear(year) ? 29 : days[month];
    return day >= 1 && day <= maxDay;
  }

  static bool validMeasurementFields(const BPData& record,
                                     bool requireSequences) {
    if (requireSequences &&
        (record.recordSequence == 0 || record.sessionSequence == 0)) {
      return false;
    }
    const bool timestampValid =
      (record.timestampSource == BPTimestampSource::DEVICE ||
       record.timestampSource == BPTimestampSource::LEGACY_SYSTEM)
        ? validStructuredTimestamp(record.timestamp)
        : record.timestampSource == BPTimestampSource::LEGACY_UNSYNCED &&
          record.timestamp == "時間未同步";
    if (!timestampValid || record.timestamp.length() > kMaxTimestampBytes ||
        record.movementCount < 0 || record.movementCount > 9) {
      return false;
    }
    const bool clean = record.quality == BPMeasurementQuality::CLEAN &&
                       record.movementCount == 0;
    const bool motion = record.quality == BPMeasurementQuality::MOTION &&
                        record.movementCount > 0;
    if (!clean && !motion) return false;

    const bool vitalsInRange =
      record.systolic >= 60 && record.systolic <= 260 &&
      record.diastolic >= 30 && record.diastolic <= 215 &&
      record.pulse >= 40 && record.pulse <= 180;
    const bool emptyVitals = record.systolic == -1 &&
                             record.diastolic == -1 && record.pulse == -1;
    return record.valid ? vitalsInRange : (vitalsInRange || emptyVitals);
  }

  static size_t encodeSlot(const BPData& record, uint32_t generation,
                           uint8_t (&encoded)[kMaxSlotSize]) {
    if (generation == 0 || !validMeasurementFields(record, true)) return 0;
    const size_t timestampLength = record.timestamp.length();
    const size_t length = kSlotFixedSize + timestampLength;
    // Offsets: v[0], gen[1..4], record seq[5..12], session seq[13..20],
    // timestamp length[21], bytes[22..], source, four LE32 values, quality,
    // valid byte, then CRC32 LE covering every preceding byte.
    size_t offset = 0;
    encoded[offset++] = kSchemaVersion;
    writeLe32(encoded + offset, generation);
    offset += 4;
    writeLe64(encoded + offset, record.recordSequence);
    offset += 8;
    writeLe64(encoded + offset, record.sessionSequence);
    offset += 8;
    encoded[offset++] = static_cast<uint8_t>(timestampLength);
    for (size_t i = 0; i < timestampLength; ++i) {
      encoded[offset++] = static_cast<uint8_t>(record.timestamp.charAt(i));
    }
    encoded[offset++] = static_cast<uint8_t>(record.timestampSource);
    writeLe32(encoded + offset, static_cast<uint32_t>(record.systolic));
    offset += 4;
    writeLe32(encoded + offset, static_cast<uint32_t>(record.diastolic));
    offset += 4;
    writeLe32(encoded + offset, static_cast<uint32_t>(record.pulse));
    offset += 4;
    writeLe32(encoded + offset, static_cast<uint32_t>(record.movementCount));
    offset += 4;
    encoded[offset++] = static_cast<uint8_t>(record.quality);
    encoded[offset++] = record.valid ? 1 : 0;
    writeLe32(encoded + offset, crc32(encoded, offset));
    offset += 4;
    return offset == length ? length : 0;
  }

  static bool decodeSlot(const uint8_t* encoded, size_t length,
                         uint32_t& generation, BPData& record) {
    if (encoded == nullptr || length < kSlotFixedSize ||
        length > kMaxSlotSize || encoded[0] != kSchemaVersion ||
        readLe32(encoded + length - 4) != crc32(encoded, length - 4)) {
      return false;
    }
    const size_t timestampLength = encoded[21];
    if (timestampLength == 0 || timestampLength > kMaxTimestampBytes ||
        length != kSlotFixedSize + timestampLength) {
      return false;
    }

    generation = readLe32(encoded + 1);
    if (generation == 0) return false;
    record = BPData{};
    record.recordSequence = readLe64(encoded + 5);
    record.sessionSequence = readLe64(encoded + 13);
    if (!record.timestamp.reserve(static_cast<unsigned int>(timestampLength)) ||
        !record.timestamp.concat(
          reinterpret_cast<const char*>(encoded + 22),
          static_cast<unsigned int>(timestampLength))) {
      return false;
    }
    size_t offset = 22 + timestampLength;
    const uint8_t source = encoded[offset++];
    if (source > static_cast<uint8_t>(BPTimestampSource::LEGACY_UNSYNCED)) {
      return false;
    }
    record.timestampSource = static_cast<BPTimestampSource>(source);
    record.systolic = readLeI32(encoded + offset);
    offset += 4;
    record.diastolic = readLeI32(encoded + offset);
    offset += 4;
    record.pulse = readLeI32(encoded + offset);
    offset += 4;
    record.movementCount = readLeI32(encoded + offset);
    offset += 4;
    const uint8_t quality = encoded[offset++];
    if (quality > static_cast<uint8_t>(BPMeasurementQuality::MOTION)) return false;
    record.quality = static_cast<BPMeasurementQuality>(quality);
    const uint8_t valid = encoded[offset++];
    if (valid > 1 || offset + 4 != length) return false;
    record.valid = valid == 1;
    return validMeasurementFields(record, true);
  }

  static bool strictInt(const String& value, int32_t& parsed) {
    if (value.length() == 0) return false;
    unsigned int offset = 0;
    bool negative = false;
    if (value.charAt(0) == '-') {
      negative = true;
      offset = 1;
      if (value.length() == 1) return false;
    }
    uint64_t magnitude = 0;
    const uint64_t limit = negative
      ? static_cast<uint64_t>(INT32_MAX) + 1ULL
      : static_cast<uint64_t>(INT32_MAX);
    for (; offset < value.length(); ++offset) {
      const char digit = value.charAt(offset);
      if (!isDigit(digit)) return false;
      magnitude = magnitude * 10ULL + static_cast<uint64_t>(digit - '0');
      if (magnitude > limit) return false;
    }
    if (negative && magnitude == static_cast<uint64_t>(INT32_MAX) + 1ULL) {
      parsed = INT32_MIN;
    } else {
      parsed = negative ? -static_cast<int32_t>(magnitude)
                        : static_cast<int32_t>(magnitude);
    }
    return true;
  }

  static bool parseLegacyRecord(const String& serialized, BPData& record) {
    const int sep1 = serialized.indexOf('|');
    const int sep2 = sep1 < 0 ? -1 : serialized.indexOf('|', sep1 + 1);
    const int sep3 = sep2 < 0 ? -1 : serialized.indexOf('|', sep2 + 1);
    const int sep4 = sep3 < 0 ? -1 : serialized.indexOf('|', sep3 + 1);
    const int sep5 = sep4 < 0 ? -1 : serialized.indexOf('|', sep4 + 1);
    if (sep1 <= 0 || sep2 <= sep1 + 1 || sep3 <= sep2 + 1 || sep5 >= 0) {
      return false;
    }
    const String timestamp = serialized.substring(0, sep1);
    int32_t systolic = 0;
    int32_t diastolic = 0;
    int32_t pulse = 0;
    if (!strictInt(serialized.substring(sep1 + 1, sep2), systolic) ||
        !strictInt(serialized.substring(sep2 + 1, sep3), diastolic)) {
      return false;
    }
    const int pulseEnd = sep4 >= 0 ? sep4 : static_cast<int>(serialized.length());
    if (!strictInt(serialized.substring(sep3 + 1, pulseEnd), pulse)) return false;

    bool valid = systolic > 0 && diastolic > 0 && pulse > 0;
    if (sep4 >= 0) {
      const String encodedValid = serialized.substring(sep4 + 1);
      if (encodedValid == "0") valid = false;
      else if (encodedValid == "1") valid = true;
      else return false;
    }

    record = BPData{};
    record.timestamp = timestamp;
    record.timestampSource = timestamp == "時間未同步"
      ? BPTimestampSource::LEGACY_UNSYNCED
      : BPTimestampSource::LEGACY_SYSTEM;
    record.systolic = systolic;
    record.diastolic = diastolic;
    record.pulse = pulse;
    record.movementCount = 0;
    record.quality = BPMeasurementQuality::CLEAN;
    record.valid = valid;
    return validMeasurementFields(record, false);
  }

  static bool makeIndexedKey(char* key, size_t keySize,
                             const char* prefix, int index) {
    const int written = snprintf(key, keySize, "%s%d", prefix, index);
    return written > 0 && static_cast<size_t>(written) < keySize && written <= 15;
  }

  void resetRecords() {
    for (int i = 0; i < _maxRecords; ++i) _records[i] = BPData{};
    _recordCount = 0;
  }

  void appendChronological(BPData record) {
    if (_recordCount < _maxRecords) {
      _records[_recordCount++] = std::move(record);
      return;
    }
    for (int i = 1; i < _maxRecords; ++i) {
      _records[i - 1] = std::move(_records[i]);
    }
    _records[_maxRecords - 1] = std::move(record);
  }

  bool removeIfPresent(Preferences& preferences, const char* key) const {
    return !preferences.isKey(key) || preferences.remove(key);
  }

  bool cleanupLegacyKeys(Preferences& preferences) const {
    const char* metadata[] = {"schema", "count", "index"};
    for (const char* key : metadata) {
      if (!removeIfPresent(preferences, key)) return false;
    }
    char key[16];
    for (int i = 0; i < _maxRecords; ++i) {
      if (!makeIndexedKey(key, sizeof(key), "slot_", i) ||
          !removeIfPresent(preferences, key) ||
          !makeIndexedKey(key, sizeof(key), "rec_", i) ||
          !removeIfPresent(preferences, key)) {
        return false;
      }
    }
    return true;
  }

  bool removeAllV3Slots(Preferences& preferences) const {
    char key[16];
    for (int i = 0; i < _maxRecords; ++i) {
      if (!makeIndexedKey(key, sizeof(key), "v3_", i) ||
          !removeIfPresent(preferences, key)) {
        return false;
      }
    }
    return true;
  }

  bool putState(Preferences& preferences, uint32_t generation,
                uint64_t nextSequenceFloor) const {
    uint8_t encoded[kStateSize];
    encodeState(generation, nextSequenceFloor, encoded);
    return preferences.putBytes(kStateKey, encoded, sizeof(encoded)) ==
           sizeof(encoded);
  }

  bool putSlot(Preferences& preferences, int slot, const BPData& record,
               uint32_t generation) const {
    char key[16];
    uint8_t encoded[kMaxSlotSize];
    if (!makeIndexedKey(key, sizeof(key), "v3_", slot)) return false;
    const size_t length = encodeSlot(record, generation, encoded);
    return length != 0 && preferences.putBytes(key, encoded, length) == length;
  }

  bool loadV3Opened(Preferences& preferences) {
    if (preferences.getType(kStateKey) != PT_BLOB ||
        preferences.getBytesLength(kStateKey) != kStateSize) {
      return false;
    }
    uint8_t stateBytes[kStateSize];
    if (preferences.getBytes(kStateKey, stateBytes, sizeof(stateBytes)) !=
        sizeof(stateBytes)) {
      return false;
    }
    uint32_t generation = 0;
    uint64_t floor = 0;
    if (!decodeState(stateBytes, sizeof(stateBytes), generation, floor)) {
      return false;
    }

    Candidate* candidates = new (std::nothrow) Candidate[_maxRecords];
    if (candidates == nullptr) return false;
    int candidateCount = 0;
    bool healthy = true;
    char key[16];
    for (int slot = 0; slot < _maxRecords; ++slot) {
      if (!makeIndexedKey(key, sizeof(key), "v3_", slot)) {
        healthy = false;
        continue;
      }
      if (!preferences.isKey(key)) continue;
      if (preferences.getType(key) != PT_BLOB) {
        healthy = false;
        continue;
      }
      const size_t length = preferences.getBytesLength(key);
      if (length < kSlotFixedSize || length > kMaxSlotSize) {
        healthy = false;
        continue;
      }
      uint8_t encoded[kMaxSlotSize];
      if (preferences.getBytes(key, encoded, sizeof(encoded)) != length) {
        healthy = false;
        continue;
      }
      uint32_t slotGeneration = 0;
      BPData record;
      if (!decodeSlot(encoded, length, slotGeneration, record)) {
        healthy = false;
        continue;
      }
      if (slotGeneration != generation) continue;
      candidates[candidateCount++].record = std::move(record);
    }

    for (int i = 0; i < candidateCount; ++i) {
      for (int j = i + 1; j < candidateCount; ++j) {
        if (candidates[i].record.recordSequence ==
            candidates[j].record.recordSequence) {
          candidates[i].duplicate = true;
          candidates[j].duplicate = true;
          healthy = false;
        }
      }
    }
    for (int i = 0; i < candidateCount; ++i) {
      for (int j = i + 1; j < candidateCount; ++j) {
        if (candidates[j].record.recordSequence <
            candidates[i].record.recordSequence) {
          std::swap(candidates[i], candidates[j]);
        }
      }
    }

    resetRecords();
    uint64_t next = floor;
    bool exhausted = floor == UINT64_MAX;
    for (int i = 0; i < candidateCount; ++i) {
      if (candidates[i].duplicate) continue;
      const uint64_t sequence = candidates[i].record.recordSequence;
      appendChronological(std::move(candidates[i].record));
      if (sequence >= next) {
        if (sequence == UINT64_MAX) {
          exhausted = true;
          next = UINT64_MAX;
        } else {
          next = sequence + 1;
        }
      }
    }
    delete[] candidates;

    _generation = generation;
    _nextSequence = next;
    _sequenceExhausted = exhausted;
    _stateReady = true;
    if (!cleanupLegacyKeys(preferences)) healthy = false;
    _storageHealthy = healthy;
    return healthy;
  }

  bool legacyArtifactsPresent(Preferences& preferences) const {
    if (preferences.isKey("count") || preferences.isKey("index") ||
        preferences.isKey("schema")) {
      return true;
    }
    char key[16];
    for (int i = 0; i < _maxRecords; ++i) {
      if ((makeIndexedKey(key, sizeof(key), "rec_", i) &&
           preferences.isKey(key)) ||
          (makeIndexedKey(key, sizeof(key), "slot_", i) &&
           preferences.isKey(key))) {
        return true;
      }
    }
    return false;
  }

  // Loads v2/legacy into chronological RAM. fatal distinguishes ambiguous
  // metadata from malformed individual records; either blocks activation.
  // Nonfatal malformed sources retain only validated survivors for diagnosis
  // while leaving the original source authoritative and untouched.
  bool loadLegacyOpened(Preferences& preferences, bool& fatal,
                        bool& sourceHealthy) {
    fatal = false;
    sourceHealthy = true;
    const bool schemaPresent = preferences.isKey("schema");
    bool v2 = false;
    if (schemaPresent) {
      if (preferences.getType("schema") != PT_STR ||
          !(preferences.getString("schema", "") == "v2")) {
        fatal = true;
        return false;
      }
      v2 = true;
    }

    const bool countPresent = preferences.isKey("count");
    if (!countPresent) {
      if (schemaPresent || legacyArtifactsPresent(preferences)) {
        fatal = true;
        return false;
      }
      resetRecords();
      return true;
    }
    if (preferences.getType("count") != PT_I32) {
      fatal = true;
      return false;
    }
    const int storedCount = preferences.getInt("count", -1);
    if (storedCount < 0 || storedCount > _maxRecords) {
      fatal = true;
      return false;
    }

    int storedIndex = 0;
    if (v2) {
      if (!preferences.isKey("index") ||
          preferences.getType("index") != PT_I32) {
        fatal = true;
        return false;
      }
      storedIndex = preferences.getInt("index", -1);
      if (storedIndex < 0 || storedIndex >= _maxRecords) {
        fatal = true;
        return false;
      }
    } else if (preferences.isKey("index")) {
      fatal = true;
      return false;
    }

    resetRecords();
    char key[16];
    for (int chronological = 0; chronological < storedCount; ++chronological) {
      int physical = 0;
      if (v2) {
        physical = (storedIndex - storedCount + chronological) % _maxRecords;
        if (physical < 0) physical += _maxRecords;
        if (!makeIndexedKey(key, sizeof(key), "slot_", physical)) {
          sourceHealthy = false;
          continue;
        }
      } else {
        const int newestIndex = storedCount - 1 - chronological;
        if (!makeIndexedKey(key, sizeof(key), "rec_", newestIndex)) {
          sourceHealthy = false;
          continue;
        }
      }
      if (!preferences.isKey(key) || preferences.getType(key) != PT_STR) {
        sourceHealthy = false;
        continue;
      }
      BPData record;
      if (!parseLegacyRecord(preferences.getString(key, ""), record)) {
        sourceHealthy = false;
        continue;
      }
      appendChronological(std::move(record));
    }

    // Old formats wrote metadata and payloads separately. A count/index that
    // under-reports still-present source keys is degraded, not permission to
    // activate a smaller v3 set and erase the unexpected durable record.
    for (int physical = 0; physical < _maxRecords; ++physical) {
      bool expected = false;
      if (v2) {
        for (int chronological = 0; chronological < storedCount;
             ++chronological) {
          int selected =
            (storedIndex - storedCount + chronological) % _maxRecords;
          if (selected < 0) selected += _maxRecords;
          if (selected == physical) expected = true;
        }
        if (!makeIndexedKey(key, sizeof(key), "slot_", physical) ||
            preferences.isKey(key) != expected) {
          sourceHealthy = false;
        }
      } else {
        expected = physical < storedCount;
        if (!makeIndexedKey(key, sizeof(key), "rec_", physical) ||
            preferences.isKey(key) != expected) {
          sourceHealthy = false;
        }
        if (!makeIndexedKey(key, sizeof(key), "slot_", physical) ||
            preferences.isKey(key)) {
          sourceHealthy = false;
        }
      }
    }
    return true;
  }

  bool durableStatePresent() {
    if (!_preferences.begin(kNamespace, false)) return false;
    const bool present = _preferences.isKey(kStateKey);
    _preferences.end();
    return present;
  }

  bool migrateLoadedRecords(bool sourceHealthy) {
    if (!_preferences.begin(kNamespace, false)) return false;
    if (!removeAllV3Slots(_preferences)) {
      _preferences.end();
      return false;
    }

    const uint32_t generation = 1;
    for (int i = 0; i < _recordCount; ++i) {
      const uint64_t sequence = static_cast<uint64_t>(i) + 1ULL;
      _records[i].recordSequence = sequence;
      if (_records[i].sessionSequence == 0) {
        // No subject identifier/hash is retained: migrated records default to
        // conservative one-record sessions.
        _records[i].sessionSequence = sequence;
      }
      if (!putSlot(_preferences, i, _records[i], generation)) {
        _preferences.end();
        return false;
      }
    }
    const uint64_t next = static_cast<uint64_t>(_recordCount) + 1ULL;
    if (!putState(_preferences, generation, next)) {
      _preferences.end();
      if (durableStatePresent()) (void)loadFromStorage();
      return false;
    }

    _generation = generation;
    _nextSequence = next;
    _sequenceExhausted = false;
    _stateReady = true;
    const bool cleanupHealthy = cleanupLegacyKeys(_preferences);
    _preferences.end();
    _storageHealthy = sourceHealthy && cleanupHealthy;
    return _storageHealthy;
  }

public:
  explicit BP_RecordManager(int maxRecords = 10)
    : _maxRecords(maxRecords > 0 ? maxRecords : 1),
      _records(new BPData[maxRecords > 0 ? maxRecords : 1]) {}

  ~BP_RecordManager() { delete[] _records; }

  BP_RecordManager(const BP_RecordManager&) = delete;
  BP_RecordManager& operator=(const BP_RecordManager&) = delete;

  bool addRecord(BPData record) {
    if (!_stateReady || !_storageHealthy) {
      if (!loadFromStorage()) return false;
    }
    if (!_storageHealthy || _sequenceExhausted ||
        _nextSequence == UINT64_MAX) {
      return false;
    }

    record.recordSequence = _nextSequence;
    if (record.sessionSequence == 0) {
      // Privacy-first default: never infer grouping from transient identity.
      // A zero value becomes an opaque one-record session.
      record.sessionSequence = record.recordSequence;
    }
    if (!validMeasurementFields(record, true)) return false;
    const int slot = static_cast<int>(
      (record.recordSequence - 1ULL) % static_cast<uint64_t>(_maxRecords));
    if (!_preferences.begin(kNamespace, false)) return false;
    const bool written = putSlot(_preferences, slot, record, _generation);
    _preferences.end();
    if (!written) {
      (void)loadFromStorage();
      return false;
    }

    const uint64_t acceptedSequence = record.recordSequence;
    appendChronological(std::move(record));
    _lastSuccessfulRecordSequence = acceptedSequence;
    _lastSuccessfulReceiveMs = static_cast<uint32_t>(millis());
    _nextSequence++;
    return true;
  }

  const BPData& getRecord(int index) const {
    if (index < 0 || index >= _recordCount) {
      static const BPData kEmpty;
      return kEmpty;
    }
    return _records[_recordCount - index - 1];
  }

  const BPData& getLatestRecord() const { return getRecord(0); }
  int getRecordCount() const { return _recordCount; }
  int getMaxRecords() const { return _maxRecords; }

  // recordSequence is the durable, opaque revision. It remains monotonic when
  // the storage ring wraps and does not introduce a second rollover domain.
  uint64_t getRevision() const {
    return _recordCount > 0 ? getLatestRecord().recordSequence : 0;
  }

  bool latestReceivedThisBoot() const {
    return _recordCount > 0 && _lastSuccessfulRecordSequence != 0 &&
      getLatestRecord().recordSequence == _lastSuccessfulRecordSequence;
  }

  bool lastSuccessfulReceiveAgeMs(uint32_t nowMs, uint32_t& ageMs) const {
    if (!latestReceivedThisBoot()) return false;
    ageMs = nowMs - _lastSuccessfulReceiveMs;
    return true;
  }

  bool clearRecords() {
    if (!_stateReady || !_storageHealthy) {
      if (!loadFromStorage()) return false;
    }
    if (!_storageHealthy || _generation == UINT32_MAX) return false;
    const uint32_t nextGeneration = _generation + 1U;
    if (nextGeneration == 0 || !_preferences.begin(kNamespace, false)) {
      return false;
    }
    const bool tombstoneWritten =
      putState(_preferences, nextGeneration, _nextSequence);
    _preferences.end();
    if (!tombstoneWritten) {
      (void)loadFromStorage();
      return false;
    }

    _generation = nextGeneration;
    resetRecords();
    _stateReady = true;
    _storageHealthy = true;
    if (!_preferences.begin(kNamespace, false)) {
      _storageHealthy = false;
      return false;
    }
    const bool cleaned = removeAllV3Slots(_preferences) &&
                         cleanupLegacyKeys(_preferences);
    _preferences.end();
    if (!cleaned) _storageHealthy = false;
    return cleaned;
  }

  bool loadFromStorage() {
    _lastSuccessfulRecordSequence = 0;
    _lastSuccessfulReceiveMs = 0;
    resetRecords();
    _stateReady = false;
    _storageHealthy = false;
    _sequenceExhausted = false;
    if (!_preferences.begin(kNamespace, false)) return false;

    if (_preferences.isKey(kStateKey)) {
      const bool loaded = loadV3Opened(_preferences);
      _preferences.end();
      if (!loaded && !_stateReady) resetRecords();
      return loaded;
    }

    bool fatal = false;
    bool sourceHealthy = true;
    const bool loadedLegacy =
      loadLegacyOpened(_preferences, fatal, sourceHealthy);
    _preferences.end();
    if (!loadedLegacy || fatal) {
      resetRecords();
      return false;
    }
    if (!sourceHealthy) {
      // Never make a lossy migration authoritative. Validated survivors stay
      // visible for diagnosis, but the original source remains intact and all
      // mutations stay blocked until the source is repaired or reset safely.
      _stateReady = false;
      _storageHealthy = false;
      return false;
    }
    return migrateLoadedRecords(true);
  }
};

#endif
