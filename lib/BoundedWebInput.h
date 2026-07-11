#ifndef BOUNDED_WEB_INPUT_H
#define BOUNDED_WEB_INPUT_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bp_web {

class BoundedIngressBuffer {
public:
  static constexpr size_t kCapacity = 256;

  BoundedIngressBuffer() { clear(); }

  BoundedIngressBuffer(const BoundedIngressBuffer&) = delete;
  BoundedIngressBuffer& operator=(const BoundedIngressBuffer&) = delete;

  ~BoundedIngressBuffer() { clear(); }

  uint8_t* writableData() { return _length == 0 ? _bytes : nullptr; }

  size_t writableCapacity() const {
    return _length == 0 ? kCapacity : 0;
  }

  bool commit(size_t length) {
    if (_length != 0 || _offset != 0 || length > kCapacity) {
      clear();
      return false;
    }
    _length = length;
    return true;
  }

  const uint8_t* data() const { return _bytes + _offset; }
  size_t length() const { return _length; }

  bool consume(size_t length) {
    if (length > _length) {
      clear();
      return false;
    }
    secureZero(_bytes + _offset, length);
    _offset += length;
    _length -= length;
    if (_length == 0) clear();
    return true;
  }

  void clear() {
    secureZero(_bytes, sizeof(_bytes));
    _offset = 0;
    _length = 0;
  }

private:
  uint8_t _bytes[kCapacity] = {};
  size_t _offset = 0;
  size_t _length = 0;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }
};

class BoundedFormValidator {
public:
  static constexpr size_t kMaxFields = 16;
  static constexpr size_t kMaxKeyChars = 32;
  static constexpr size_t kMaxValueBytes = 128;

  BoundedFormValidator() { reset(); }

  BoundedFormValidator(const BoundedFormValidator&) = delete;
  BoundedFormValidator& operator=(const BoundedFormValidator&) = delete;

  ~BoundedFormValidator() { reset(); }

  bool validate(const char* query, size_t queryLength,
                const char* body, size_t bodyLength) {
    reset();
    if ((query == nullptr && queryLength != 0) ||
        (body == nullptr && bodyLength != 0) ||
        !parseSegment(query, queryLength) ||
        !parseSegment(body, bodyLength)) {
      reset();
      return false;
    }
    return true;
  }

  size_t fieldCount() const { return _fieldCount; }

private:
  char _keys[kMaxFields][kMaxKeyChars + 1] = {};
  size_t _keyLengths[kMaxFields] = {};
  size_t _fieldCount = 0;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }

  void reset() {
    secureZero(_keys, sizeof(_keys));
    secureZero(_keyLengths, sizeof(_keyLengths));
    _fieldCount = 0;
  }

  static int hexValue(uint8_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  }

  static bool rawValueByteAllowed(uint8_t value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '*' || value == '-' || value == '.' ||
           value == '_' || value == '~';
  }

  static bool keyByteAllowed(uint8_t value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') ||
           value == '-' || value == '_';
  }

  static bool nextDecodedByte(const char* encoded, size_t length,
                              size_t& offset, uint8_t& decoded,
                              bool key) {
    if (offset >= length) return false;
    const uint8_t value = static_cast<uint8_t>(encoded[offset]);
    if (value == '%') {
      if (length - offset < 3) return false;
      const int high = hexValue(static_cast<uint8_t>(encoded[offset + 1]));
      const int low = hexValue(static_cast<uint8_t>(encoded[offset + 2]));
      if (high < 0 || low < 0) return false;
      decoded = static_cast<uint8_t>((high << 4) | low);
      offset += 3;
    } else if (value == '+') {
      decoded = ' ';
      ++offset;
    } else {
      if (!rawValueByteAllowed(value)) return false;
      decoded = value;
      ++offset;
    }
    if (key) return keyByteAllowed(decoded);
    return decoded >= 0x20U && decoded != 0x7fU;
  }

  bool decodeKey(const char* encoded, size_t length,
                 char decoded[kMaxKeyChars + 1],
                 size_t& decodedLength) const {
    decodedLength = 0;
    size_t offset = 0;
    while (offset < length) {
      uint8_t value = 0;
      if (!nextDecodedByte(encoded, length, offset, value, true) ||
          decodedLength >= kMaxKeyChars) {
        secureZero(decoded, kMaxKeyChars + 1);
        decodedLength = 0;
        return false;
      }
      decoded[decodedLength++] = static_cast<char>(value);
    }
    decoded[decodedLength] = '\0';
    return decodedLength != 0;
  }

  static bool validateValue(const char* encoded, size_t length) {
    size_t decodedLength = 0;
    size_t offset = 0;
    while (offset < length) {
      uint8_t value = 0;
      if (!nextDecodedByte(encoded, length, offset, value, false) ||
          decodedLength >= kMaxValueBytes) {
        return false;
      }
      ++decodedLength;
    }
    return true;
  }

  bool duplicateKey(const char* key, size_t keyLength) const {
    for (size_t i = 0; i < _fieldCount; ++i) {
      if (_keyLengths[i] == keyLength &&
          std::memcmp(_keys[i], key, keyLength) == 0) {
        return true;
      }
    }
    return false;
  }

  bool parseSegment(const char* encoded, size_t length) {
    if (length == 0) return true;
    if (encoded == nullptr) return false;

    size_t fieldStart = 0;
    while (fieldStart < length) {
      size_t fieldEnd = fieldStart;
      while (fieldEnd < length && encoded[fieldEnd] != '&') ++fieldEnd;
      if (fieldEnd == fieldStart) return false;

      size_t equals = fieldEnd;
      for (size_t i = fieldStart; i < fieldEnd; ++i) {
        if (encoded[i] == '=') {
          if (equals != fieldEnd) return false;
          equals = i;
        }
      }
      if (equals == fieldStart || equals == fieldEnd ||
          _fieldCount >= kMaxFields) {
        return false;
      }

      char key[kMaxKeyChars + 1] = {};
      size_t keyLength = 0;
      if (!decodeKey(encoded + fieldStart, equals - fieldStart,
                     key, keyLength) ||
          !validateValue(encoded + equals + 1,
                         fieldEnd - equals - 1) ||
          duplicateKey(key, keyLength)) {
        secureZero(key, sizeof(key));
        return false;
      }
      std::memcpy(_keys[_fieldCount], key, keyLength + 1);
      _keyLengths[_fieldCount] = keyLength;
      ++_fieldCount;
      secureZero(key, sizeof(key));

      if (fieldEnd == length) break;
      fieldStart = fieldEnd + 1;
      if (fieldStart == length) return false;
    }
    return true;
  }
};

}  // namespace bp_web

#endif
