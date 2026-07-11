#ifndef BP_BOUNDED_STREAM_CONSUMER_H
#define BP_BOUNDED_STREAM_CONSUMER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bp_http {

enum class StreamConsumerState : uint8_t {
  IDLE = 0,
  ACTIVE,
  COMPLETE,
  FAILED,
};

enum class StreamConsumerResult : uint8_t {
  OK = 0,
  INVALID_STATE,
  INVALID_CALLBACKS,
  INVALID_LENGTH,
  BEGIN_FAILED,
  WRITE_FAILED,
  FINISH_FAILED,
};

using StreamBeginFn = bool (*)(void* context, uint32_t expectedLength);
using StreamWriteFn = bool (*)(void* context, const uint8_t* bytes,
                               size_t length);
using StreamFinishFn = bool (*)(void* context);
using StreamAbortFn = void (*)(void* context);

struct StreamConsumerCallbacks {
  void* context = nullptr;
  StreamBeginFn begin = nullptr;
  StreamWriteFn write = nullptr;
  StreamFinishFn finish = nullptr;
  StreamAbortFn abort = nullptr;
};

class BoundedStreamConsumer {
public:
  static constexpr size_t kChunkLimit = 256;

  BoundedStreamConsumer() = default;
  BoundedStreamConsumer(const BoundedStreamConsumer&) = delete;
  BoundedStreamConsumer& operator=(const BoundedStreamConsumer&) = delete;
  ~BoundedStreamConsumer() { cancel(); }

  StreamConsumerResult start(uint32_t expectedLength,
                             const StreamConsumerCallbacks& callbacks) {
    if (_callbackInProgress) {
      _reentryDetected = true;
      return StreamConsumerResult::INVALID_STATE;
    }
    if (_state == StreamConsumerState::ACTIVE) {
      return StreamConsumerResult::INVALID_STATE;
    }
    clearRuntime();
    _state = StreamConsumerState::IDLE;
    if (expectedLength == 0) return StreamConsumerResult::INVALID_LENGTH;
    if (callbacks.begin == nullptr || callbacks.write == nullptr ||
        callbacks.finish == nullptr || callbacks.abort == nullptr) {
      return StreamConsumerResult::INVALID_CALLBACKS;
    }
    _callbacks = callbacks;
    _expectedLength = expectedLength;
    _receivedLength = 0;
    _state = StreamConsumerState::ACTIVE;
    bool began = false;
    beginCallback();
    try {
      began = _callbacks.begin(_callbacks.context, expectedLength);
    } catch (...) {
      began = false;
    }
    const bool reentered = endCallback();
    if (!began || reentered || _state != StreamConsumerState::ACTIVE) {
      if (_state == StreamConsumerState::ACTIVE) failAndAbort();
      return StreamConsumerResult::BEGIN_FAILED;
    }
    return StreamConsumerResult::OK;
  }

  StreamConsumerResult write(const uint8_t* bytes, size_t length) {
    if (_callbackInProgress) {
      _reentryDetected = true;
      return StreamConsumerResult::INVALID_STATE;
    }
    if (_state != StreamConsumerState::ACTIVE) {
      return StreamConsumerResult::INVALID_STATE;
    }
    if (bytes == nullptr || length == 0 || length > kChunkLimit ||
        length > static_cast<size_t>(_expectedLength - _receivedLength)) {
      failAndAbort();
      return StreamConsumerResult::INVALID_LENGTH;
    }
    bool written = false;
    beginCallback();
    try {
      written = _callbacks.write(_callbacks.context, bytes, length);
    } catch (...) {
      written = false;
    }
    const bool reentered = endCallback();
    if (!written || reentered || _state != StreamConsumerState::ACTIVE) {
      if (_state == StreamConsumerState::ACTIVE) failAndAbort();
      return StreamConsumerResult::WRITE_FAILED;
    }
    _receivedLength += static_cast<uint32_t>(length);
    return StreamConsumerResult::OK;
  }

  StreamConsumerResult finish() {
    if (_callbackInProgress) {
      _reentryDetected = true;
      return StreamConsumerResult::INVALID_STATE;
    }
    if (_state != StreamConsumerState::ACTIVE) {
      return StreamConsumerResult::INVALID_STATE;
    }
    if (_receivedLength != _expectedLength) {
      failAndAbort();
      return StreamConsumerResult::INVALID_LENGTH;
    }
    bool finished = false;
    beginCallback();
    try {
      finished = _callbacks.finish(_callbacks.context);
    } catch (...) {
      finished = false;
    }
    const bool reentered = endCallback();
    if (!finished || reentered || _state != StreamConsumerState::ACTIVE) {
      if (_state == StreamConsumerState::ACTIVE) failAndAbort();
      return StreamConsumerResult::FINISH_FAILED;
    }
    clearRuntime();
    _state = StreamConsumerState::COMPLETE;
    return StreamConsumerResult::OK;
  }

  void cancel() {
    if (_callbackInProgress) {
      // The external callback still owns and may be using its context. Defer
      // abort until the outer operation has completely returned.
      _reentryDetected = true;
      return;
    }
    if (_state == StreamConsumerState::ACTIVE) {
      failAndAbort();
    } else {
      clearRuntime();
    }
  }

  void reset() {
    if (_callbackInProgress) {
      _reentryDetected = true;
      return;
    }
    cancel();
    _state = StreamConsumerState::IDLE;
  }

  StreamConsumerState state() const { return _state; }
  uint32_t expectedLength() const { return _expectedLength; }
  uint32_t receivedLength() const { return _receivedLength; }
  bool active() const { return _state == StreamConsumerState::ACTIVE; }

private:
  StreamConsumerCallbacks _callbacks{};
  uint32_t _expectedLength = 0;
  uint32_t _receivedLength = 0;
  StreamConsumerState _state = StreamConsumerState::IDLE;
  bool _callbackInProgress = false;
  bool _reentryDetected = false;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }

  void clearRuntime() {
    secureZero(&_callbacks, sizeof(_callbacks));
    _expectedLength = 0;
    _receivedLength = 0;
  }

  void beginCallback() {
    _callbackInProgress = true;
    _reentryDetected = false;
  }

  bool endCallback() {
    const bool reentered = _reentryDetected;
    _callbackInProgress = false;
    _reentryDetected = false;
    return reentered;
  }

  void failAndAbort() {
    StreamAbortFn abort = _callbacks.abort;
    void* context = _callbacks.context;
    // Drop ownership before invoking external code so re-entry or an exception
    // cannot cause a second abort of the same sink.
    clearRuntime();
    _state = StreamConsumerState::FAILED;
    if (abort == nullptr) return;
    const bool nested = _callbackInProgress;
    if (!nested) beginCallback();
    try {
      abort(context);
    } catch (...) {
      // Cleanup is best-effort at this boundary; state remains fail closed.
    }
    if (!nested) (void)endCallback();
  }
};

}  // namespace bp_http

#endif
