#ifndef BP_PARSER_H
#define BP_PARSER_H

#include <Arduino.h>
#include <string.h>

#include "BPProtocol.h"

class BP_Parser {
public:
  explicit BP_Parser(const String& model) : _model(model) {}

  void setModel(const String& model) {
    _model = model;
  }

  const String& getModel() const {
    return _model;
  }

  bool isLineDelimited() const {
    return _model == "OMRON-HBP9030";
  }

  BPParseResult parseResult(const uint8_t* buffer, int length) const {
    if (_model != "OMRON-HBP9030") {
      BPParseResult result;
      result.error = BPParseError::UNSUPPORTED_MODEL;
      return result;
    }
    if (isUnsupportedHbpFormat(buffer, length)) {
      BPParseResult result;
      result.error = BPParseError::UNSUPPORTED_FORMAT;
      return result;
    }
    return parseHbp9030Format5(buffer, length);
  }

  // Compatibility for consumers migrated in Task 4. Identity and error detail
  // remain available only through parseResult().
  BPData parse(const uint8_t* buffer, int length) const {
    return parseResult(buffer, length).measurement;
  }

private:
  static constexpr int kPayloadLength = 53;
  String _model;

  static bool startsWith(const uint8_t* buffer, int length,
                         const char* prefix, int prefixLength) {
    return buffer != nullptr && length >= prefixLength &&
           memcmp(buffer, prefix, static_cast<size_t>(prefixLength)) == 0;
  }

  static bool isUnsupportedHbpFormat(const uint8_t* buffer, int length) {
    return startsWith(buffer, length, "MMBP203N", 8) ||
           startsWith(buffer, length, "ID", 2) ||
           startsWith(buffer, length, "bp,", 3);
  }

  static bool allDigits(const uint8_t* buffer, int offset, int width) {
    for (int i = 0; i < width; ++i) {
      if (buffer[offset + i] < '0' || buffer[offset + i] > '9') {
        return false;
      }
    }
    return true;
  }

  static bool allSpaces(const uint8_t* buffer, int offset, int width) {
    for (int i = 0; i < width; ++i) {
      if (buffer[offset + i] != ' ') return false;
    }
    return true;
  }

  static int parseDigits(const uint8_t* buffer, int offset, int width) {
    int value = 0;
    for (int i = 0; i < width; ++i) {
      value = value * 10 + (buffer[offset + i] - '0');
    }
    return value;
  }

  static bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  }

  static bool validCalendar(int year, int month, int day, int hour, int minute) {
    if (year < 1 || month < 1 || month > 12 || day < 1 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      return false;
    }
    static const uint8_t kDaysInMonth[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int maxDay = kDaysInMonth[month - 1];
    if (month == 2 && isLeapYear(year)) maxDay = 29;
    return day <= maxDay;
  }

  static bool validIdByte(uint8_t value) {
    return value >= 0x20 && value <= 0x7E && value != ',';
  }

  static BPParseResult parseHbp9030Format5(const uint8_t* buffer, int length) {
    BPParseResult result;
    if (buffer == nullptr || length != kPayloadLength) return result;

    static const uint8_t kCommaOffsets[] = {4, 7, 10, 13, 16, 37, 39, 43, 47, 51};
    for (uint8_t offset : kCommaOffsets) {
      if (buffer[offset] != ',') return result;
    }

    const int numericOffsets[] = {0, 5, 8, 11, 14};
    const int numericWidths[] = {4, 2, 2, 2, 2};
    for (int i = 0; i < 5; ++i) {
      if (!allDigits(buffer, numericOffsets[i], numericWidths[i])) return result;
    }
    for (int i = 17; i <= 36; ++i) {
      if (!validIdByte(buffer[i])) return result;
    }
    if (!allDigits(buffer, 38, 1) || !allDigits(buffer, 52, 1)) return result;

    const int year = parseDigits(buffer, 0, 4);
    const int month = parseDigits(buffer, 5, 2);
    const int day = parseDigits(buffer, 8, 2);
    const int hour = parseDigits(buffer, 11, 2);
    const int minute = parseDigits(buffer, 14, 2);
    if (!validCalendar(year, month, day, hour, minute)) return result;

    if (!result.transientSubjectId.reserve(20) ||
        !result.transientSubjectId.concat(
          reinterpret_cast<const char*>(buffer + 17), 20)) {
      return BPParseResult{};
    }

    char timestamp[20];
    memcpy(timestamp, buffer, 4);
    timestamp[4] = '-';
    memcpy(timestamp + 5, buffer + 5, 2);
    timestamp[7] = '-';
    memcpy(timestamp + 8, buffer + 8, 2);
    timestamp[10] = ' ';
    memcpy(timestamp + 11, buffer + 11, 2);
    timestamp[13] = ':';
    memcpy(timestamp + 14, buffer + 14, 2);
    timestamp[16] = ':';
    timestamp[17] = '0';
    timestamp[18] = '0';
    timestamp[19] = '\0';
    if (!result.measurement.timestamp.reserve(19) ||
        !result.measurement.timestamp.concat(timestamp, 19)) {
      return BPParseResult{};
    }
    result.measurement.timestampSource = BPTimestampSource::DEVICE;
    result.measurement.movementCount = parseDigits(buffer, 52, 1);
    result.measurement.quality = result.measurement.movementCount > 0
      ? BPMeasurementQuality::MOTION
      : BPMeasurementQuality::CLEAN;

    result.deviceErrorCode = parseDigits(buffer, 38, 1);
    const bool vitalDigits = allDigits(buffer, 40, 3) &&
                             allDigits(buffer, 44, 3) &&
                             allDigits(buffer, 48, 3);
    const bool vitalSpaces = allSpaces(buffer, 40, 3) &&
                             allSpaces(buffer, 44, 3) &&
                             allSpaces(buffer, 48, 3);

    if (result.deviceErrorCode != 0) {
      if (!vitalDigits && !vitalSpaces) return result;
      if (vitalDigits) {
        result.measurement.systolic = parseDigits(buffer, 40, 3);
        result.measurement.diastolic = parseDigits(buffer, 44, 3);
        result.measurement.pulse = parseDigits(buffer, 48, 3);
      }
      result.error = BPParseError::DEVICE_ERROR;
      return result;
    }

    if (!vitalDigits) return result;
    result.measurement.systolic = parseDigits(buffer, 40, 3);
    result.measurement.diastolic = parseDigits(buffer, 44, 3);
    result.measurement.pulse = parseDigits(buffer, 48, 3);

    if (result.measurement.systolic < 60 || result.measurement.systolic > 260 ||
        result.measurement.diastolic < 30 || result.measurement.diastolic > 215 ||
        result.measurement.pulse < 40 || result.measurement.pulse > 180) {
      result.error = BPParseError::OUT_OF_RANGE;
      return result;
    }

    result.measurement.valid = true;
    result.error = BPParseError::NONE;
    return result;
  }
};

#endif
