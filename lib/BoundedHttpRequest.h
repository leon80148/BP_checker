#ifndef BOUNDED_HTTP_REQUEST_H
#define BOUNDED_HTTP_REQUEST_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bp_http {

enum class RequestState : uint8_t {
  REQUEST_LINE,
  HEADERS,
  WAIT_POLICY,
  BODY,
  READY,
  REJECT,
};

enum class BodyMode : uint8_t {
  NONE,
  SMALL_FORM,
  STREAM,
};

enum class RequestMethod : uint8_t {
  GET,
  POST,
};

enum class RequestError : uint8_t {
  NONE,
  BAD_REQUEST,
  MISSING_HOST,
  TIMEOUT,
  METHOD_NOT_IMPLEMENTED,
  VERSION_NOT_SUPPORTED,
  REQUEST_LINE_TOO_LONG,
  HEADER_FIELDS_TOO_LARGE,
  INVALID_CONTENT_LENGTH,
  TRANSFER_ENCODING_NOT_SUPPORTED,
  EXPECTATION_FAILED,
  LENGTH_REQUIRED,
  PAYLOAD_TOO_LARGE,
  UNSUPPORTED_MEDIA_TYPE,
  INVALID_POLICY,
};

constexpr int httpStatusForError(RequestError error) {
  switch (error) {
    case RequestError::NONE:
      return 0;
    case RequestError::BAD_REQUEST:
    case RequestError::MISSING_HOST:
    case RequestError::INVALID_CONTENT_LENGTH:
      return 400;
    case RequestError::TIMEOUT:
      return 408;
    case RequestError::METHOD_NOT_IMPLEMENTED:
      return 501;
    case RequestError::EXPECTATION_FAILED:
      return 417;
    case RequestError::LENGTH_REQUIRED:
      return 411;
    case RequestError::PAYLOAD_TOO_LARGE:
      return 413;
    case RequestError::UNSUPPORTED_MEDIA_TYPE:
      return 415;
    case RequestError::REQUEST_LINE_TOO_LONG:
      return 414;
    case RequestError::HEADER_FIELDS_TOO_LARGE:
      return 431;
    case RequestError::TRANSFER_ENCODING_NOT_SUPPORTED:
      return 501;
    case RequestError::VERSION_NOT_SUPPORTED:
      return 505;
    case RequestError::INVALID_POLICY:
      return 500;
  }
  return 500;
}

struct RequestView {
  RequestMethod method = RequestMethod::GET;
  char path[257] = {};
  char query[257] = {};
  char host[513] = {};
  char authorization[97] = {};
  char origin[257] = {};
  char referer[257] = {};
  char contentType[129] = {};
  uint32_t contentLength = 0;
  bool hasContentLength = false;
};

struct ConsumeResult {
  size_t consumed;
  RequestState state;
};

class BoundedHttpRequest {
public:
  static constexpr size_t kRequestLineLimit = 256;
  static constexpr size_t kHeaderLineLimit = 512;
  static constexpr size_t kHeaderTotalLimit = 2048;
  static constexpr size_t kHeaderCountLimit = 24;
  static constexpr size_t kByteBudget = 256;
  static constexpr size_t kSmallFormLimit = 1024;
  static constexpr size_t kStreamChunkLimit = 256;
  static constexpr uint32_t kHeaderDeadlineMs = 1500;
  static constexpr uint32_t kBodyDeadlineMs = 1500;

  BoundedHttpRequest() { reset(0); }

  BoundedHttpRequest(const BoundedHttpRequest&) = delete;
  BoundedHttpRequest& operator=(const BoundedHttpRequest&) = delete;

  ~BoundedHttpRequest() {
    secureZero(&_view, sizeof(_view));
    secureZero(_line, sizeof(_line));
    secureZero(_body, sizeof(_body));
    secureZero(_streamChunk, sizeof(_streamChunk));
  }

  void reset(uint32_t nowMs) {
    _startedAt = nowMs;
    _state = RequestState::REQUEST_LINE;
    _error = RequestError::NONE;
    _lineLength = 0;
    _sawCarriageReturn = false;
    _hostSeen = false;
    _contentLengthSeen = false;
    _authorizationSeen = false;
    _originSeen = false;
    _refererSeen = false;
    _contentTypeSeen = false;
    _headerBytes = 0;
    _headerCount = 0;
    _bodyLength = 0;
    _bodyMode = BodyMode::NONE;
    _bodyStartedAt = 0;
    _streamChunkLength = 0;
    secureZero(_line, sizeof(_line));
    secureZero(&_view, sizeof(_view));
    secureZero(_body, sizeof(_body));
    secureZero(_streamChunk, sizeof(_streamChunk));
    _view.method = RequestMethod::GET;
  }

  bool acceptPolicy(BodyMode mode, size_t routeBodyCap, uint32_t nowMs) {
    if (_state != RequestState::WAIT_POLICY) return false;
    if (mode == BodyMode::NONE) {
      if (routeBodyCap != 0) {
        reject(RequestError::INVALID_POLICY);
        return false;
      }
      if (_view.hasContentLength && _view.contentLength != 0) {
        reject(RequestError::PAYLOAD_TOO_LARGE);
        return false;
      }
      _bodyMode = mode;
      _state = RequestState::READY;
      return true;
    }
    if (mode != BodyMode::SMALL_FORM && mode != BodyMode::STREAM) {
      reject(RequestError::INVALID_POLICY);
      return false;
    }
    if (mode == BodyMode::SMALL_FORM && routeBodyCap > kSmallFormLimit) {
      reject(RequestError::INVALID_POLICY);
      return false;
    }
    if (!_view.hasContentLength) {
      reject(RequestError::LENGTH_REQUIRED);
      return false;
    }
    if (_view.contentLength > routeBodyCap ||
        (mode == BodyMode::SMALL_FORM &&
         _view.contentLength > kSmallFormLimit)) {
      reject(RequestError::PAYLOAD_TOO_LARGE);
      return false;
    }
    if (mode == BodyMode::SMALL_FORM) {
      static constexpr char kFormContentType[] =
        "application/x-www-form-urlencoded";
      if (!nameEquals(_view.contentType, std::strlen(_view.contentType),
                      kFormContentType)) {
        reject(RequestError::UNSUPPORTED_MEDIA_TYPE);
        return false;
      }
    }
    _bodyMode = mode;
    _bodyStartedAt = nowMs;
    _state = _view.contentLength == 0
      ? RequestState::READY : RequestState::BODY;
    return true;
  }

  ConsumeResult consume(const uint8_t* data, size_t length, uint32_t nowMs,
                        size_t budget = kByteBudget) {
    size_t consumed = 0;
    if (_state == RequestState::WAIT_POLICY ||
        _state == RequestState::READY ||
        _state == RequestState::REJECT) {
      return {0, _state};
    }
    if (_state == RequestState::BODY) {
      if (static_cast<uint32_t>(nowMs - _bodyStartedAt) >=
          kBodyDeadlineMs) {
        reject(RequestError::TIMEOUT);
        return {0, _state};
      }
      if (data == nullptr && length != 0) {
        reject(RequestError::BAD_REQUEST);
        return {0, _state};
      }
      if (budget > kByteBudget) budget = kByteBudget;
      size_t remaining = static_cast<size_t>(_view.contentLength) -
                         _bodyLength;
      if (_bodyMode == BodyMode::STREAM && _streamChunkLength != 0) {
        return {0, _state};
      }
      size_t take = length;
      if (take > budget) take = budget;
      if (take > remaining) take = remaining;
      if (_bodyMode == BodyMode::STREAM && take > kStreamChunkLimit) {
        take = kStreamChunkLimit;
      }
      if (take != 0) {
        for (size_t i = 0; i < take; ++i) {
          const uint8_t value = data[i];
          if (_bodyMode == BodyMode::SMALL_FORM &&
              (value < 0x20U || value == 0x7fU)) {
            consumed = i + 1;
            reject(RequestError::BAD_REQUEST);
            return {consumed, _state};
          }
        }
        if (_bodyMode == BodyMode::STREAM) {
          std::memcpy(_streamChunk, data, take);
          _streamChunkLength = take;
          _bodyLength += take;
          consumed = take;
        } else {
          std::memcpy(_body + _bodyLength, data, take);
          _bodyLength += take;
          consumed = take;
          _body[_bodyLength] = '\0';
        }
      }
      if (_bodyMode != BodyMode::STREAM &&
          _bodyLength == static_cast<size_t>(_view.contentLength)) {
        _state = RequestState::READY;
      }
      return {consumed, _state};
    }
    if ((_state == RequestState::REQUEST_LINE ||
         _state == RequestState::HEADERS) &&
        static_cast<uint32_t>(nowMs - _startedAt) >= kHeaderDeadlineMs) {
      reject(RequestError::TIMEOUT);
      return {0, _state};
    }
    if (data == nullptr && length != 0) {
      reject(RequestError::BAD_REQUEST);
      return {0, _state};
    }
    if (budget > kByteBudget) budget = kByteBudget;

    while (consumed < length && consumed < budget &&
           _state != RequestState::WAIT_POLICY &&
           _state != RequestState::REJECT) {
      const bool isHeaderByte = _state == RequestState::HEADERS;
      const uint8_t byte = data[consumed++];
      if (isHeaderByte) {
        if (_headerBytes >= kHeaderTotalLimit) {
          reject(RequestError::HEADER_FIELDS_TOO_LARGE);
          break;
        }
        ++_headerBytes;
      }
      if (_sawCarriageReturn) {
        if (byte != '\n') {
          reject(RequestError::BAD_REQUEST);
          break;
        }
        _sawCarriageReturn = false;
        _line[_lineLength] = '\0';
        const size_t completedLength = _lineLength;
        finishLine();
        secureZero(_line, completedLength + 1);
        _lineLength = 0;
        continue;
      }
      if (byte == '\r') {
        _sawCarriageReturn = true;
        continue;
      }
      if (byte == '\n' || byte == 0) {
        reject(RequestError::BAD_REQUEST);
        break;
      }
      const size_t limit = _state == RequestState::REQUEST_LINE
        ? kRequestLineLimit : kHeaderLineLimit;
      if (_lineLength >= limit) {
        reject(_state == RequestState::REQUEST_LINE
                 ? RequestError::REQUEST_LINE_TOO_LONG
                 : RequestError::HEADER_FIELDS_TOO_LARGE);
        break;
      }
      _line[_lineLength++] = static_cast<char>(byte);
    }
    return {consumed, _state};
  }

  RequestState state() const { return _state; }
  RequestError error() const { return _error; }
  const RequestView& view() const { return _view; }
  const char* body() const { return _body; }
  size_t bodyLength() const {
    return _bodyMode == BodyMode::SMALL_FORM ? _bodyLength : 0;
  }
  size_t receivedBodyLength() const { return _bodyLength; }
  const uint8_t* streamChunk() const { return _streamChunk; }
  size_t streamChunkLength() const { return _streamChunkLength; }

  bool drainStreamChunk() {
    if (_state != RequestState::BODY || _bodyMode != BodyMode::STREAM ||
        _streamChunkLength == 0) {
      return false;
    }
    secureZero(_streamChunk, sizeof(_streamChunk));
    _streamChunkLength = 0;
    if (_bodyLength == static_cast<size_t>(_view.contentLength)) {
      _state = RequestState::READY;
    }
    return true;
  }

private:
  RequestState _state = RequestState::REQUEST_LINE;
  RequestError _error = RequestError::NONE;
  RequestView _view{};
  char _line[kHeaderLineLimit + 1] = {};
  size_t _lineLength = 0;
  bool _sawCarriageReturn = false;
  bool _hostSeen = false;
  bool _contentLengthSeen = false;
  bool _authorizationSeen = false;
  bool _originSeen = false;
  bool _refererSeen = false;
  bool _contentTypeSeen = false;
  size_t _headerBytes = 0;
  size_t _headerCount = 0;
  uint32_t _startedAt = 0;
  char _body[kSmallFormLimit + 1] = {};
  size_t _bodyLength = 0;
  BodyMode _bodyMode = BodyMode::NONE;
  uint32_t _bodyStartedAt = 0;
  uint8_t _streamChunk[kStreamChunkLimit] = {};
  size_t _streamChunkLength = 0;

  void reject(RequestError error) {
    _error = error;
    _state = RequestState::REJECT;
    secureZero(&_view, sizeof(_view));
    secureZero(_line, sizeof(_line));
    secureZero(_body, sizeof(_body));
    secureZero(_streamChunk, sizeof(_streamChunk));
    _lineLength = 0;
    _bodyLength = 0;
    _streamChunkLength = 0;
    _sawCarriageReturn = false;
  }

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }

  void finishLine() {
    if (_state == RequestState::REQUEST_LINE) {
      parseRequestLine();
      return;
    }
    if (_lineLength == 0) {
      if (!_hostSeen) {
        reject(RequestError::MISSING_HOST);
      } else if (_view.method == RequestMethod::GET &&
                 _view.hasContentLength && _view.contentLength != 0) {
        reject(RequestError::BAD_REQUEST);
      } else {
        _state = RequestState::WAIT_POLICY;
      }
      return;
    }
    if (_headerCount >= kHeaderCountLimit) {
      reject(RequestError::HEADER_FIELDS_TOO_LARGE);
      return;
    }
    ++_headerCount;
    parseHeaderLine();
  }

  void parseRequestLine() {
    static constexpr char kSuffix[] = " HTTP/1.1";
    const size_t suffixLength = sizeof(kSuffix) - 1;
    const char* target = nullptr;
    size_t prefixLength = 0;
    if (_lineLength >= 4 && std::memcmp(_line, "GET ", 4) == 0) {
      _view.method = RequestMethod::GET;
      prefixLength = 4;
      target = _line + prefixLength;
    } else if (_lineLength >= 5 &&
               std::memcmp(_line, "POST ", 5) == 0) {
      _view.method = RequestMethod::POST;
      prefixLength = 5;
      target = _line + prefixLength;
    }
    if (target == nullptr) {
      reject(RequestError::METHOD_NOT_IMPLEMENTED);
      return;
    }
    if (_lineLength <= prefixLength + suffixLength ||
        std::memcmp(_line + _lineLength - suffixLength,
                    kSuffix, suffixLength) != 0) {
      reject(RequestError::VERSION_NOT_SUPPORTED);
      return;
    }
    const size_t targetLength = _lineLength - prefixLength - suffixLength;
    if (targetLength == 0 || target[0] != '/') {
      reject(RequestError::BAD_REQUEST);
      return;
    }
    size_t queryOffset = targetLength;
    for (size_t i = 0; i < targetLength; ++i) {
      const uint8_t value = static_cast<uint8_t>(target[i]);
      if (value <= 0x20U || value == 0x7fU || target[i] == '#') {
        reject(RequestError::BAD_REQUEST);
        return;
      }
      if (target[i] == '?') {
        queryOffset = i;
        break;
      }
    }
    for (size_t i = queryOffset + (queryOffset < targetLength ? 1 : 0);
         i < targetLength; ++i) {
      const uint8_t value = static_cast<uint8_t>(target[i]);
      if (value <= 0x20U || value == 0x7fU || target[i] == '#') {
        reject(RequestError::BAD_REQUEST);
        return;
      }
    }
    const size_t pathLength = queryOffset;
    const size_t queryLength = queryOffset < targetLength
      ? targetLength - queryOffset - 1 : 0;
    if (pathLength == 0 || pathLength >= sizeof(_view.path) ||
        queryLength >= sizeof(_view.query)) {
      reject(RequestError::BAD_REQUEST);
      return;
    }
    std::memcpy(_view.path, target, pathLength);
    _view.path[pathLength] = '\0';
    if (queryLength != 0) {
      std::memcpy(_view.query, target + queryOffset + 1, queryLength);
      _view.query[queryLength] = '\0';
    }
    _state = RequestState::HEADERS;
  }

  static bool isTokenCharacter(uint8_t value) {
    if ((value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9')) {
      return true;
    }
    switch (value) {
      case '!': case '#': case '$': case '%': case '&': case '\'':
      case '*': case '+': case '-': case '.': case '^': case '_':
      case '`': case '|': case '~':
        return true;
      default:
        return false;
    }
  }

  static uint8_t asciiLower(uint8_t value) {
    return value >= 'A' && value <= 'Z'
      ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
  }

  static bool nameEquals(const char* name, size_t nameLength,
                         const char* expected) {
    const size_t expectedLength = std::strlen(expected);
    if (nameLength != expectedLength) return false;
    for (size_t i = 0; i < nameLength; ++i) {
      if (asciiLower(static_cast<uint8_t>(name[i])) !=
          asciiLower(static_cast<uint8_t>(expected[i]))) {
        return false;
      }
    }
    return true;
  }

  bool copyUniqueHeader(char* destination, size_t capacity, bool& seen,
                        const char* value, size_t valueLength,
                        bool allowEmpty = false) {
    if (seen || (!allowEmpty && valueLength == 0)) {
      reject(RequestError::BAD_REQUEST);
      return false;
    }
    if (valueLength >= capacity) {
      reject(RequestError::HEADER_FIELDS_TOO_LARGE);
      return false;
    }
    if (valueLength != 0) std::memcpy(destination, value, valueLength);
    destination[valueLength] = '\0';
    seen = true;
    return true;
  }

  void parseHeaderLine() {
    size_t colon = _lineLength;
    for (size_t i = 0; i < _lineLength; ++i) {
      if (_line[i] == ':') {
        colon = i;
        break;
      }
      if (!isTokenCharacter(static_cast<uint8_t>(_line[i]))) {
        reject(RequestError::BAD_REQUEST);
        return;
      }
    }
    if (colon == 0 || colon == _lineLength) {
      reject(RequestError::BAD_REQUEST);
      return;
    }

    size_t valueStart = colon + 1;
    while (valueStart < _lineLength && _line[valueStart] == ' ') {
      ++valueStart;
    }
    size_t valueEnd = _lineLength;
    while (valueEnd > valueStart && _line[valueEnd - 1] == ' ') {
      --valueEnd;
    }
    for (size_t i = valueStart; i < valueEnd; ++i) {
      const uint8_t value = static_cast<uint8_t>(_line[i]);
      if (value < 0x20U || value == 0x7fU) {
        reject(RequestError::BAD_REQUEST);
        return;
      }
    }
    const char* value = _line + valueStart;
    const size_t valueLength = valueEnd - valueStart;

    if (nameEquals(_line, colon, "transfer-encoding")) {
      reject(RequestError::TRANSFER_ENCODING_NOT_SUPPORTED);
      return;
    }
    if (nameEquals(_line, colon, "expect")) {
      reject(RequestError::EXPECTATION_FAILED);
      return;
    }
    if (nameEquals(_line, colon, "host")) {
      copyUniqueHeader(_view.host, sizeof(_view.host), _hostSeen,
                       value, valueLength);
      return;
    }
    if (nameEquals(_line, colon, "authorization")) {
      copyUniqueHeader(_view.authorization, sizeof(_view.authorization),
                       _authorizationSeen, value, valueLength);
      return;
    }
    if (nameEquals(_line, colon, "origin")) {
      copyUniqueHeader(_view.origin, sizeof(_view.origin), _originSeen,
                       value, valueLength);
      return;
    }
    if (nameEquals(_line, colon, "referer")) {
      copyUniqueHeader(_view.referer, sizeof(_view.referer), _refererSeen,
                       value, valueLength);
      return;
    }
    if (nameEquals(_line, colon, "content-type")) {
      copyUniqueHeader(_view.contentType, sizeof(_view.contentType),
                       _contentTypeSeen, value, valueLength);
      return;
    }
    if (nameEquals(_line, colon, "content-length")) {
      if (_contentLengthSeen || valueLength == 0) {
        reject(RequestError::INVALID_CONTENT_LENGTH);
        return;
      }
      uint32_t parsed = 0;
      for (size_t i = 0; i < valueLength; ++i) {
        if (value[i] < '0' || value[i] > '9') {
          reject(RequestError::INVALID_CONTENT_LENGTH);
          return;
        }
        const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
          reject(RequestError::INVALID_CONTENT_LENGTH);
          return;
        }
        parsed = parsed * 10U + digit;
      }
      _view.contentLength = parsed;
      _view.hasContentLength = true;
      _contentLengthSeen = true;
    }
  }
};

}  // namespace bp_http

#endif
