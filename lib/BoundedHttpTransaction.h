#ifndef BOUNDED_HTTP_TRANSACTION_H
#define BOUNDED_HTTP_TRANSACTION_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BoundedHttpRequest.h"
#include "BoundedHttpResponse.h"

namespace bp_http {

enum class TransactionState : uint8_t {
  IDLE,
  READING_HEADERS,
  WAIT_POLICY,
  READING_BODY,
  DISPATCH_READY,
  CAPTURING_RESPONSE,
  SENDING_RESPONSE,
  COMPLETE,
  ABORTED,
};

struct TransactionConsume {
  size_t consumed;
  TransactionState state;
};

enum class AllowedMethods : uint8_t {
  GET,
  POST,
  GET_AND_POST,
};

class BoundedHttpTransaction {
public:
  static constexpr size_t kReadBudget = BoundedHttpRequest::kByteBudget;
  static constexpr uint32_t kHandoffDeadlineMs = 1500;

  BoundedHttpTransaction() = default;

  BoundedHttpTransaction(const BoundedHttpTransaction&) = delete;
  BoundedHttpTransaction& operator=(const BoundedHttpTransaction&) = delete;

  void begin(uint32_t nowMs) {
    _request.reset(nowMs);
    _response.begin();
    _queuedStatus = 0;
    _phaseStartedAt = nowMs;
    _state = TransactionState::READING_HEADERS;
  }

  TransactionConsume consume(const uint8_t* data, size_t length,
                             uint32_t nowMs,
                             size_t budget = kReadBudget) {
    if (_state != TransactionState::READING_HEADERS &&
        _state != TransactionState::READING_BODY) {
      return {0, _state};
    }
    if (budget > kReadBudget) budget = kReadBudget;
    const ConsumeResult result = _request.consume(data, length, nowMs, budget);
    synchronizeRequestState(nowMs);
    return {result.consumed, _state};
  }

  bool acceptPolicy(BodyMode mode, size_t routeBodyCap, uint32_t nowMs) {
    if (_state != TransactionState::WAIT_POLICY) return false;
    if (handoffDeadlineElapsed(nowMs)) {
      (void)queueError(408, nowMs);
      return false;
    }
    const bool accepted = _request.acceptPolicy(mode, routeBodyCap, nowMs);
    synchronizeRequestState(nowMs);
    return accepted;
  }

  RequestBodyChunk pendingStreamChunk() const {
    if (_state != TransactionState::READING_BODY) return {nullptr, 0};
    const size_t length = _request.streamChunkLength();
    if (length == 0) return {nullptr, 0};
    return {_request.streamChunk(), length};
  }

  bool drainStreamChunk(uint32_t nowMs) {
    if (_state != TransactionState::READING_BODY) return false;
    (void)_request.consume(nullptr, 0, nowMs);
    synchronizeRequestState(nowMs);
    if (_state != TransactionState::READING_BODY) return false;
    if (!_request.drainStreamChunk()) return false;
    synchronizeRequestState(nowMs);
    return true;
  }

  bool rejectBody(int status, uint32_t nowMs) {
    if (_state != TransactionState::READING_BODY) return false;
    return queueError(status == 405 ? 500 : status, nowMs);
  }

  bool rejectPolicy(int status, uint32_t nowMs) {
    if (_state != TransactionState::WAIT_POLICY) return false;
    if (handoffDeadlineElapsed(nowMs)) return queueError(408, nowMs);
    return queueError(status == 405 ? 500 : status, nowMs,
                      AllowedMethods::GET_AND_POST);
  }

  bool rejectPolicy(int status, uint32_t nowMs,
                    AllowedMethods allowedMethods) {
    if (_state != TransactionState::WAIT_POLICY) return false;
    if (handoffDeadlineElapsed(nowMs)) return queueError(408, nowMs);
    return queueError(status, nowMs, allowedMethods);
  }

  bool beginDispatch(uint32_t nowMs) {
    if (_state != TransactionState::DISPATCH_READY) return false;
    if (handoffDeadlineElapsed(nowMs)) {
      (void)queueError(503, nowMs);
      return false;
    }
    _response.begin();
    _queuedStatus = 0;
    transitionTo(TransactionState::CAPTURING_RESPONSE, nowMs);
    return true;
  }

  bool rejectDispatch(int status, uint32_t nowMs) {
    if (_state != TransactionState::DISPATCH_READY) return false;
    if (handoffDeadlineElapsed(nowMs)) return queueError(503, nowMs);
    return queueError(status == 405 ? 500 : status, nowMs);
  }

  bool rejectCapture(int status, uint32_t nowMs) {
    if (_state != TransactionState::CAPTURING_RESPONSE) return false;
    if (handoffDeadlineElapsed(nowMs)) return queueError(503, nowMs);
    return queueError(status == 405 ? 500 : status, nowMs);
  }

  size_t capture(const uint8_t* data, size_t length) {
    if (_state != TransactionState::CAPTURING_RESPONSE) return 0;
    return _response.append(data, length);
  }

  bool capturedResponseIsValidHttp1() const {
    return _state == TransactionState::CAPTURING_RESPONSE &&
           _response.validHttp1Envelope();
  }

  bool finishDispatch(uint32_t nowMs) {
    if (_state != TransactionState::CAPTURING_RESPONSE) return false;
    if (handoffDeadlineElapsed(nowMs)) {
      (void)queueError(503, nowMs);
      return false;
    }
    if (_response.responseLength() == 0 && !_response.overflowed()) {
      return queueError(500, nowMs);
    }
    if (_response.overflowed()) _queuedStatus = 503;
    if (!_response.finalize(nowMs)) {
      abort();
      return false;
    }
    wipeRequest(nowMs);
    transitionTo(TransactionState::SENDING_RESPONSE, nowMs);
    return true;
  }

  ResponseChunk nextOutput(
      size_t budget = BoundedHttpResponse::kSendBudget) {
    if (_state != TransactionState::SENDING_RESPONSE) return {nullptr, 0};
    return _response.nextChunk(budget);
  }

  bool acknowledgeOutput(size_t length) {
    if (_state != TransactionState::SENDING_RESPONSE) return false;
    const bool acknowledged = _response.acknowledge(length);
    if (_response.state() == ResponseState::COMPLETE) {
      _state = TransactionState::COMPLETE;
    } else if (_response.state() == ResponseState::ABORTED) {
      _state = TransactionState::ABORTED;
    }
    return acknowledged;
  }

  bool poll(uint32_t nowMs) {
    if (_state == TransactionState::READING_HEADERS ||
        _state == TransactionState::READING_BODY) {
      (void)_request.consume(nullptr, 0, nowMs);
      synchronizeRequestState(nowMs);
      return _state != TransactionState::ABORTED;
    }
    if (_state == TransactionState::WAIT_POLICY &&
        handoffDeadlineElapsed(nowMs)) {
      return queueError(408, nowMs);
    }
    if ((_state == TransactionState::DISPATCH_READY ||
         _state == TransactionState::CAPTURING_RESPONSE) &&
        handoffDeadlineElapsed(nowMs)) {
      return queueError(503, nowMs);
    }
    if (_state == TransactionState::SENDING_RESPONSE) {
      if (!_response.enforceDeadline(nowMs)) {
        _state = TransactionState::ABORTED;
        return false;
      }
    }
    return _state != TransactionState::ABORTED;
  }

  void abort() {
    wipeRequest(0);
    _response.abort();
    _queuedStatus = 0;
    _state = TransactionState::ABORTED;
  }

  TransactionState state() const { return _state; }
  const BoundedHttpRequest& request() const { return _request; }
  int queuedStatus() const { return _queuedStatus; }
  bool terminal() const {
    return _state == TransactionState::COMPLETE ||
           _state == TransactionState::ABORTED;
  }

private:
  BoundedHttpRequest _request;
  BoundedHttpResponse _response;
  TransactionState _state = TransactionState::IDLE;
  int _queuedStatus = 0;
  uint32_t _phaseStartedAt = 0;

  static const char* reasonPhrase(int status) {
    switch (status) {
      case 400: return "Bad Request";
      case 401: return "Unauthorized";
      case 403: return "Forbidden";
      case 404: return "Not Found";
      case 405: return "Method Not Allowed";
      case 408: return "Request Timeout";
      case 411: return "Length Required";
      case 413: return "Payload Too Large";
      case 414: return "URI Too Long";
      case 415: return "Unsupported Media Type";
      case 417: return "Expectation Failed";
      case 429: return "Too Many Requests";
      case 431: return "Request Header Fields Too Large";
      case 500: return "Internal Server Error";
      case 501: return "Not Implemented";
      case 503: return "Service Unavailable";
      case 505: return "HTTP Version Not Supported";
      default: return nullptr;
    }
  }

  void wipeRequest(uint32_t nowMs) { _request.reset(nowMs); }

  bool handoffDeadlineElapsed(uint32_t nowMs) const {
    return static_cast<uint32_t>(nowMs - _phaseStartedAt) >=
           kHandoffDeadlineMs;
  }

  void transitionTo(TransactionState state, uint32_t nowMs) {
    if (_state != state) {
      _state = state;
      _phaseStartedAt = nowMs;
    }
  }

  void synchronizeRequestState(uint32_t nowMs) {
    switch (_request.state()) {
      case RequestState::REQUEST_LINE:
      case RequestState::HEADERS:
        transitionTo(TransactionState::READING_HEADERS, nowMs);
        return;
      case RequestState::WAIT_POLICY:
        transitionTo(TransactionState::WAIT_POLICY, nowMs);
        return;
      case RequestState::BODY:
        transitionTo(TransactionState::READING_BODY, nowMs);
        return;
      case RequestState::READY:
        transitionTo(TransactionState::DISPATCH_READY, nowMs);
        return;
      case RequestState::REJECT:
        {
          const int status = httpStatusForError(_request.error());
          (void)queueError(status, nowMs, AllowedMethods::GET_AND_POST);
        }
        return;
    }
    abort();
  }

  bool queueError(
      int status, uint32_t nowMs,
      AllowedMethods allowedMethods = AllowedMethods::GET_AND_POST) {
    const bool knownAllowedMethods =
      allowedMethods == AllowedMethods::GET ||
      allowedMethods == AllowedMethods::POST ||
      allowedMethods == AllowedMethods::GET_AND_POST;
    if (status == 405 && !knownAllowedMethods) status = 500;
    const char* reason = reasonPhrase(status);
    if (reason == nullptr) {
      status = 500;
      reason = reasonPhrase(status);
    }
    const bool needsChallenge = status == 401;
    const char* body = needsChallenge
      ? "authentication_required\n" : "request_rejected\n";
    const char* challenge = needsChallenge
      ? "WWW-Authenticate: Basic realm=\"BP Checker\"\r\n" : "";
    const char* allow = "";
    if (status == 405) {
      switch (allowedMethods) {
        case AllowedMethods::GET:
          allow = "Allow: GET\r\n";
          break;
        case AllowedMethods::POST:
          allow = "Allow: POST\r\n";
          break;
        case AllowedMethods::GET_AND_POST:
          allow = "Allow: GET, POST\r\n";
          break;
      }
    }
    char response[512] = {};
    const int written = std::snprintf(
      response, sizeof(response),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: text/plain; charset=UTF-8\r\n"
      "Content-Length: %zu\r\n"
      "Cache-Control: no-store, max-age=0\r\n"
      "Pragma: no-cache\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "%s"
      "%s"
      "Connection: close\r\n"
      "\r\n"
      "%s",
      status, reason, std::strlen(body), challenge, allow, body);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(response)) {
      abort();
      return false;
    }

    _response.begin();
    const size_t length = static_cast<size_t>(written);
    if (_response.append(reinterpret_cast<const uint8_t*>(response), length) !=
        length || !_response.finalize(nowMs)) {
      abort();
      return false;
    }
    wipeRequest(nowMs);
    _queuedStatus = status;
    transitionTo(TransactionState::SENDING_RESPONSE, nowMs);
    return true;
  }
};

}  // namespace bp_http

#endif
