#ifndef BOUNDED_HTTP_RESPONSE_H
#define BOUNDED_HTTP_RESPONSE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bp_http {

enum class ResponseState : uint8_t {
  BUILDING,
  SENDING,
  COMPLETE,
  ABORTED,
};

struct ResponseChunk {
  const uint8_t* data;
  size_t length;
};

class BoundedHttpResponse {
public:
  static constexpr size_t kCapacity = 16384;
  static constexpr size_t kSendBudget = 1024;
  static constexpr uint32_t kSendDeadlineMs = 1500;

  BoundedHttpResponse() { begin(); }

  BoundedHttpResponse(const BoundedHttpResponse&) = delete;
  BoundedHttpResponse& operator=(const BoundedHttpResponse&) = delete;

  ~BoundedHttpResponse() { secureZero(_bytes, sizeof(_bytes)); }

  void begin() {
    secureZero(_bytes, sizeof(_bytes));
    _length = 0;
    _offset = 0;
    _offered = 0;
    _sendStartedAt = 0;
    _overflowed = false;
    _state = ResponseState::BUILDING;
  }

  size_t append(const uint8_t* data, size_t length) {
    if (_state != ResponseState::BUILDING || _overflowed) return 0;
    if ((data == nullptr && length != 0) ||
        length > kCapacity - _length) {
      secureZero(_bytes, sizeof(_bytes));
      _length = 0;
      _offset = 0;
      _overflowed = true;
      return 0;
    }
    if (length != 0) std::memcpy(_bytes + _length, data, length);
    _length += length;
    return length;
  }

  bool finalize(uint32_t nowMs) {
    if (_state != ResponseState::BUILDING) return false;
    if (_overflowed) {
      static constexpr char kOverflowResponse[] =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Length: 19\r\n"
        "Cache-Control: no-store, max-age=0\r\n"
        "Connection: close\r\n"
        "\r\n"
        "response_too_large\n";
      static_assert(sizeof(kOverflowResponse) - 1 <= kCapacity,
                    "overflow fallback must fit fixed response buffer");
      secureZero(_bytes, sizeof(_bytes));
      std::memcpy(_bytes, kOverflowResponse, sizeof(kOverflowResponse) - 1);
      _length = sizeof(kOverflowResponse) - 1;
    }
    if (_length == 0) return false;
    _sendStartedAt = nowMs;
    _offered = 0;
    _state = ResponseState::SENDING;
    return true;
  }

  ResponseChunk nextChunk(size_t budget = kSendBudget) {
    if (_state != ResponseState::SENDING) return {nullptr, 0};
    if (_offered != 0) return {_bytes + _offset, _offered};
    if (budget > kSendBudget) budget = kSendBudget;
    size_t pending = _length - _offset;
    if (pending > budget) pending = budget;
    _offered = pending;
    return {_bytes + _offset, pending};
  }

  bool acknowledge(size_t length) {
    if (_state != ResponseState::SENDING || length == 0) {
      return false;
    }
    if (_offered == 0 || length > _offered ||
        length > _length - _offset) {
      abort();
      return false;
    }
    secureZero(_bytes + _offset, length);
    _offset += length;
    _offered = 0;
    if (_offset == _length) {
      secureZero(_bytes, sizeof(_bytes));
      _length = 0;
      _offset = 0;
      _offered = 0;
      _state = ResponseState::COMPLETE;
    }
    return true;
  }

  bool enforceDeadline(uint32_t nowMs) {
    if (_state != ResponseState::SENDING) return false;
    if (static_cast<uint32_t>(nowMs - _sendStartedAt) >=
        kSendDeadlineMs) {
      abort();
      return false;
    }
    return true;
  }

  void abort() {
    if (_state == ResponseState::COMPLETE ||
        _state == ResponseState::ABORTED) {
      return;
    }
    secureZero(_bytes, sizeof(_bytes));
    _length = 0;
    _offset = 0;
    _offered = 0;
    _state = ResponseState::ABORTED;
  }

  ResponseState state() const { return _state; }
  bool overflowed() const { return _overflowed; }
  size_t responseLength() const { return _length; }
  size_t pendingLength() const {
    return _state == ResponseState::SENDING ? _length - _offset : 0;
  }

  bool validHttp1Envelope() const {
    if (_state != ResponseState::BUILDING || _overflowed || _length == 0) {
      return false;
    }

    const size_t statusEnd = findCrlf(0, _length);
    if (statusEnd == kNotFound || statusEnd <= 13 ||
        !bytesEqual(0, "HTTP/1.1") || _bytes[8] != ' ' ||
        _bytes[9] < '1' || _bytes[9] > '5' ||
        _bytes[10] < '0' || _bytes[10] > '9' ||
        _bytes[11] < '0' || _bytes[11] > '9' || _bytes[12] != ' ') {
      return false;
    }
    for (size_t i = 13; i < statusEnd; ++i) {
      if (_bytes[i] < 0x20 || _bytes[i] > 0x7e) return false;
    }

    size_t contentLength = 0;
    size_t headerCount = 0;
    size_t contentTypeCount = 0;
    size_t contentLengthCount = 0;
    size_t cacheControlCount = 0;
    size_t pragmaCount = 0;
    size_t contentOptionsCount = 0;
    size_t connectionCount = 0;
    size_t cursor = statusEnd + 2;
    size_t bodyOffset = kNotFound;

    while (cursor < _length) {
      if (cursor + 1 < _length && _bytes[cursor] == '\r' &&
          _bytes[cursor + 1] == '\n') {
        bodyOffset = cursor + 2;
        break;
      }
      const size_t lineEnd = findCrlf(cursor, _length);
      if (lineEnd == kNotFound || lineEnd == cursor ||
          ++headerCount > kMaxValidatedHeaders) {
        return false;
      }

      size_t colon = cursor;
      while (colon < lineEnd && _bytes[colon] != ':') ++colon;
      if (colon == cursor || colon == lineEnd) return false;
      for (size_t i = cursor; i < colon; ++i) {
        if (!headerNameByteAllowed(_bytes[i])) return false;
      }

      size_t valueStart = colon + 1;
      while (valueStart < lineEnd &&
             (_bytes[valueStart] == ' ' || _bytes[valueStart] == '\t')) {
        ++valueStart;
      }
      size_t valueEnd = lineEnd;
      while (valueEnd > valueStart &&
             (_bytes[valueEnd - 1] == ' ' ||
              _bytes[valueEnd - 1] == '\t')) {
        --valueEnd;
      }
      if (valueStart == valueEnd) return false;
      for (size_t i = valueStart; i < valueEnd; ++i) {
        if ((_bytes[i] < 0x20 && _bytes[i] != '\t') ||
            _bytes[i] == 0x7f) {
          return false;
        }
      }

      const size_t nameLength = colon - cursor;
      const size_t valueLength = valueEnd - valueStart;
      if (asciiCaseEqual(cursor, nameLength, "Content-Type")) {
        if (++contentTypeCount != 1) return false;
      } else if (asciiCaseEqual(cursor, nameLength, "Content-Length")) {
        if (++contentLengthCount != 1 ||
            !parseDecimal(valueStart, valueLength, contentLength)) {
          return false;
        }
      } else if (asciiCaseEqual(cursor, nameLength, "Cache-Control")) {
        if (++cacheControlCount != 1 ||
            !bytesEqual(valueStart, valueLength,
                        "no-store, max-age=0")) {
          return false;
        }
      } else if (asciiCaseEqual(cursor, nameLength, "Pragma")) {
        if (++pragmaCount != 1 ||
            !bytesEqual(valueStart, valueLength, "no-cache")) {
          return false;
        }
      } else if (asciiCaseEqual(cursor, nameLength,
                                "X-Content-Type-Options")) {
        if (++contentOptionsCount != 1 ||
            !asciiCaseEqual(valueStart, valueLength, "nosniff")) {
          return false;
        }
      } else if (asciiCaseEqual(cursor, nameLength, "Connection")) {
        if (++connectionCount != 1 ||
            !asciiCaseEqual(valueStart, valueLength, "close")) {
          return false;
        }
      } else if (asciiCaseEqual(cursor, nameLength,
                                "Transfer-Encoding")) {
        return false;
      }
      cursor = lineEnd + 2;
    }

    if (bodyOffset == kNotFound || contentTypeCount != 1 ||
        contentLengthCount != 1 || cacheControlCount != 1 ||
        pragmaCount != 1 || contentOptionsCount != 1 ||
        connectionCount != 1) {
      return false;
    }
    return contentLength == _length - bodyOffset;
  }

private:
  static constexpr size_t kNotFound = static_cast<size_t>(-1);
  static constexpr size_t kMaxValidatedHeaders = 32;
  uint8_t _bytes[kCapacity] = {};
  size_t _length = 0;
  size_t _offset = 0;
  size_t _offered = 0;
  uint32_t _sendStartedAt = 0;
  bool _overflowed = false;
  ResponseState _state = ResponseState::BUILDING;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }

  size_t findCrlf(size_t start, size_t limit) const {
    if (limit > _length) limit = _length;
    for (size_t i = start; i + 1 < limit; ++i) {
      if (_bytes[i] == '\r' && _bytes[i + 1] == '\n') return i;
    }
    return kNotFound;
  }

  static uint8_t asciiLower(uint8_t value) {
    return value >= 'A' && value <= 'Z'
      ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
  }

  bool asciiCaseEqual(size_t offset, size_t length,
                      const char* literal) const {
    if (literal == nullptr || std::strlen(literal) != length ||
        offset > _length || length > _length - offset) {
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      if (asciiLower(_bytes[offset + i]) !=
          asciiLower(static_cast<uint8_t>(literal[i]))) {
        return false;
      }
    }
    return true;
  }

  bool bytesEqual(size_t offset, const char* literal) const {
    return literal != nullptr &&
           bytesEqual(offset, std::strlen(literal), literal);
  }

  bool bytesEqual(size_t offset, size_t length,
                  const char* literal) const {
    return literal != nullptr && std::strlen(literal) == length &&
           offset <= _length && length <= _length - offset &&
           std::memcmp(_bytes + offset, literal, length) == 0;
  }

  static bool headerNameByteAllowed(uint8_t value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-';
  }

  bool parseDecimal(size_t offset, size_t length, size_t& value) const {
    value = 0;
    if (length == 0 || offset > _length || length > _length - offset) {
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      const uint8_t byte = _bytes[offset + i];
      if (byte < '0' || byte > '9') return false;
      const size_t digit = static_cast<size_t>(byte - '0');
      if (value > (kCapacity - digit) / 10) return false;
      value = value * 10 + digit;
    }
    return true;
  }
};

}  // namespace bp_http

#endif
