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

private:
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
};

}  // namespace bp_http

#endif
