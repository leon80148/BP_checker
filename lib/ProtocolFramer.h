#ifndef PROTOCOL_FRAMER_H
#define PROTOCOL_FRAMER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class ProtocolFrameMode : uint8_t {
  UNSUPPORTED = 0,
  LINE_CRLF,
  FIXED_LENGTH,
};

using ProtocolFrameValidator = bool (*)(const uint8_t*, size_t);

struct ProtocolFrameContract {
  ProtocolFrameMode mode = ProtocolFrameMode::UNSUPPORTED;
  size_t maximumPayloadLength = 0;
  size_t fixedFrameLength = 0;
  const uint8_t* syncWord = nullptr;
  size_t syncWordLength = 0;
  ProtocolFrameValidator validator = nullptr;

  static ProtocolFrameContract unsupported() {
    return ProtocolFrameContract{};
  }

  static ProtocolFrameContract lineCrlf(size_t maximumPayloadLength) {
    ProtocolFrameContract contract;
    contract.mode = ProtocolFrameMode::LINE_CRLF;
    contract.maximumPayloadLength = maximumPayloadLength;
    return contract;
  }

  static ProtocolFrameContract fixedLengthVerified(
      size_t frameLength, const uint8_t* sync, size_t syncLength,
      ProtocolFrameValidator validator) {
    ProtocolFrameContract contract;
    contract.mode = ProtocolFrameMode::FIXED_LENGTH;
    contract.fixedFrameLength = frameLength;
    contract.syncWord = sync;
    contract.syncWordLength = syncLength;
    contract.validator = validator;
    return contract;
  }
};

enum class ProtocolFrameEvent : uint8_t {
  NONE = 0,
  FRAME,
  REJECTED,
  FRAME_OVERFLOW,
  DISCONTINUITY,
  UNSUPPORTED,
};

// Bounded, nonblocking frame assembly. LINE_CRLF accepts only a complete CRLF
// boundary and never uses elapsed time. Bytes beyond the configured maximum
// are discarded through the next trusted CRLF boundary.
class ProtocolFramer {
public:
  static constexpr size_t kCapacity = 256;

  ProtocolFrameEvent feed(uint8_t byte,
                          const ProtocolFrameContract& contract) {
    if (contract.mode == ProtocolFrameMode::LINE_CRLF) {
      return feedLine(byte, contract);
    }
    if (contract.mode == ProtocolFrameMode::FIXED_LENGTH) {
      return feedFixed(byte, contract);
    }
    reset();
    return ProtocolFrameEvent::UNSUPPORTED;
  }

  void discardUntilBoundary(
      ProtocolFrameEvent event = ProtocolFrameEvent::DISCONTINUITY) {
    beginDiscard(event, false);
  }

  void reset() {
    wipe();
    _length = 0;
    _completedLength = 0;
    _discarding = false;
    _discardSawCr = false;
    _discardEvent = ProtocolFrameEvent::REJECTED;
  }

  void clearCompletedFrame() {
    wipe();
    _completedLength = 0;
  }

  const uint8_t* frameData() const { return _buffer; }
  size_t frameLength() const { return _completedLength; }
  bool pending() const { return _length > 0 || _discarding; }

private:
  uint8_t _buffer[kCapacity] = {};
  size_t _length = 0;
  size_t _completedLength = 0;
  bool _discarding = false;
  bool _discardSawCr = false;
  ProtocolFrameEvent _discardEvent = ProtocolFrameEvent::REJECTED;

  ProtocolFrameEvent feedLine(
      uint8_t byte, const ProtocolFrameContract& contract) {
    if (contract.maximumPayloadLength == 0 ||
        contract.maximumPayloadLength + 1 > kCapacity) {
      reset();
      return ProtocolFrameEvent::UNSUPPORTED;
    }

    if (_discarding) {
      if (_discardSawCr && byte == '\n') {
        ProtocolFrameEvent event = _discardEvent;
        _discarding = false;
        _discardSawCr = false;
        _discardEvent = ProtocolFrameEvent::REJECTED;
        return event;
      }
      _discardSawCr = byte == '\r';
      return ProtocolFrameEvent::NONE;
    }

    if (byte == '\n') {
      if (_length > 0 && _buffer[_length - 1] == '\r') {
        _completedLength = _length - 1;
        _length = 0;
        return _completedLength > 0 ? ProtocolFrameEvent::FRAME
                                    : ProtocolFrameEvent::REJECTED;
      }
      reset();
      return ProtocolFrameEvent::REJECTED;
    }

    if (_length < contract.maximumPayloadLength ||
        (_length == contract.maximumPayloadLength && byte == '\r')) {
      _buffer[_length++] = byte;
      return ProtocolFrameEvent::NONE;
    }

    beginDiscard(ProtocolFrameEvent::FRAME_OVERFLOW, byte == '\r');
    return ProtocolFrameEvent::NONE;
  }

  ProtocolFrameEvent feedFixed(
      uint8_t byte, const ProtocolFrameContract& contract) {
    if (contract.fixedFrameLength == 0 ||
        contract.fixedFrameLength > kCapacity || contract.syncWord == nullptr ||
        contract.syncWordLength == 0 ||
        contract.syncWordLength > contract.fixedFrameLength ||
        contract.validator == nullptr) {
      reset();
      return ProtocolFrameEvent::UNSUPPORTED;
    }

    // A verified header is the fixed protocol's trusted start boundary.
    if (_discarding) {
      wipe();
      _length = 0;
      _completedLength = 0;
      _discarding = false;
      _discardSawCr = false;
    }

    if (_length == 0) {
      if (byte == contract.syncWord[0]) {
        _buffer[0] = byte;
        _length = 1;
      }
      return ProtocolFrameEvent::NONE;
    }

    _buffer[_length++] = byte;
    if (_length <= contract.syncWordLength) {
      if (memcmp(_buffer, contract.syncWord, _length) != 0) {
        retainFixedCandidate(contract, 1);
      }
      return ProtocolFrameEvent::NONE;
    }

    if (_length < contract.fixedFrameLength) {
      return ProtocolFrameEvent::NONE;
    }

    if (contract.validator(_buffer, _length)) {
      _completedLength = _length;
      _length = 0;
      return ProtocolFrameEvent::FRAME;
    }

    retainFixedCandidate(contract, 1);
    return ProtocolFrameEvent::REJECTED;
  }

  void wipe() { memset(_buffer, 0, sizeof(_buffer)); }

  void beginDiscard(ProtocolFrameEvent event, bool sawCr) {
    wipe();
    _length = 0;
    _completedLength = 0;
    _discarding = true;
    _discardSawCr = sawCr;
    _discardEvent = event;
  }

  void retainFixedCandidate(const ProtocolFrameContract& contract,
                            size_t searchStart) {
    for (size_t offset = searchStart;
         offset + contract.syncWordLength <= _length; ++offset) {
      if (memcmp(_buffer + offset, contract.syncWord,
                 contract.syncWordLength) == 0) {
        _length -= offset;
        memmove(_buffer, _buffer + offset, _length);
        memset(_buffer + _length, 0, sizeof(_buffer) - _length);
        return;
      }
    }

    size_t maximumKeep = contract.syncWordLength - 1;
    if (maximumKeep > _length) maximumKeep = _length;
    for (size_t keep = maximumKeep; keep > 0; --keep) {
      if (memcmp(_buffer + _length - keep, contract.syncWord, keep) == 0) {
        memmove(_buffer, _buffer + _length - keep, keep);
        memset(_buffer + keep, 0, sizeof(_buffer) - keep);
        _length = keep;
        return;
      }
    }
    wipe();
    _length = 0;
  }
};

#endif
