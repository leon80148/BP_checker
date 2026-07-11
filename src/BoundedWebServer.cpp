#include "../lib/BoundedWebServer.h"

#include <cerrno>
#include <cstring>
#include <new>

#include <lwip/sockets.h>

namespace bp_web {

namespace {

uint32_t socketClock(void*) {
  return millis();
}

SocketReadResult socketReceive(void*, int socket, uint8_t* target,
                               size_t capacity) {
  const ssize_t received = ::recv(socket, target, capacity, MSG_DONTWAIT);
  if (received > 0) {
    return {SocketReadStatus::DATA, static_cast<size_t>(received)};
  }
  if (received == 0) return {SocketReadStatus::PEER_CLOSED, 0};
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return {SocketReadStatus::WOULD_BLOCK, 0};
  }
  return {SocketReadStatus::ERROR, 0};
}

SocketWriteResult socketSend(void*, int socket, const uint8_t* bytes,
                             size_t length) {
  const ssize_t sent = ::send(socket, bytes, length, MSG_DONTWAIT);
  if (sent > 0) {
    return {SocketWriteStatus::PROGRESS, static_cast<size_t>(sent)};
  }
  if (sent < 0 &&
      (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    return {SocketWriteStatus::WOULD_BLOCK, 0};
  }
  return {SocketWriteStatus::ERROR, 0};
}

SocketShutdownStatus socketShutdownWrite(void*, int socket) {
  if (::shutdown(socket, SHUT_WR) == 0) {
    return SocketShutdownStatus::COMPLETE;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return SocketShutdownStatus::WOULD_BLOCK;
  }
  return SocketShutdownStatus::ERROR;
}

BoundedSocketOps defaultSocketOps() {
  return {nullptr, socketClock, socketReceive, socketSend,
          socketShutdownWrite};
}

}  // namespace

BoundedWebServer::BoundedWebServer(int port)
  : WebServer(port), _socketRuntime(defaultSocketOps()) {}

void BoundedWebServer::secureZero(void* target, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
  while (length-- != 0) *bytes++ = 0;
}

void BoundedWebServer::secureWipeString(String& value) {
  volatile char* bytes = value.begin();
  size_t length = value.length();
  while (length-- != 0) *bytes++ = 0;
  value = String();
}

bool BoundedWebServer::hasTerminator(const char* value, size_t capacity) {
  if (value == nullptr || capacity == 0) return false;
  for (size_t i = 0; i < capacity; ++i) {
    if (value[i] == '\0') return true;
  }
  return false;
}

size_t BoundedWebServer::boundedLength(const char* value, size_t capacity) {
  if (value == nullptr) return capacity + 1;
  size_t length = 0;
  while (length < capacity && value[length] != '\0') ++length;
  return length;
}

bp_http::AllowedMethods BoundedWebServer::allowedMethodsForPath(
    const char* path, bool& known) {
  bool allowsGet = false;
  bool allowsPost = false;
  for (size_t i = 0; i < kRoutePolicyCount; ++i) {
    if (!cStringEquals(kRoutePolicies[i].path, path)) continue;
    allowsGet = allowsGet || kRoutePolicies[i].method == HttpMethod::GET;
    allowsPost = allowsPost || kRoutePolicies[i].method == HttpMethod::POST;
  }
  known = allowsGet || allowsPost;
  if (allowsGet && allowsPost) return bp_http::AllowedMethods::GET_AND_POST;
  return allowsGet ? bp_http::AllowedMethods::GET
                   : bp_http::AllowedMethods::POST;
}

void BoundedWebServer::configureAccess(
    WebRequestGate* gate, BoundedWebSnapshotProvider snapshotProvider,
    void* snapshotContext) {
  _gate = gate;
  _snapshotProvider = snapshotProvider;
  _snapshotContext = snapshotContext;
}

void BoundedWebServer::acceptClient(uint32_t nowMs) {
  _currentClient = _server.accept();
  if (!_currentClient) return;

  _clientActive = true;
  _currentStatus = HC_WAIT_READ;
  _acceptedLocalAddress = static_cast<uint32_t>(_currentClient.localIP());
  _remoteAddress = static_cast<uint32_t>(_currentClient.remoteIP());
  _currentClient.setNoDelay(true);
  _transaction.begin(nowMs);
  _ingress.clear();
  _formValidator.clear();
  _currentRole = AccessRole::NONE;
  _currentRequestInterface = RequestInterface::UNKNOWN;
  _currentRoute = nullptr;
  _deferredAction = nullptr;
  _deferredContext = nullptr;
  _deferredFallbackPending = false;
}

void BoundedWebServer::processIngress(uint32_t nowMs) {
  const bp_http::TransactionState state = _transaction.state();
  if (state != bp_http::TransactionState::READING_HEADERS &&
      state != bp_http::TransactionState::READING_BODY) {
    return;
  }

  if (_ingress.length() == 0) {
    const int socket = _currentClient.fd();
    if (socket < 0) {
      _transaction.abort();
      return;
    }
    const IngressIoResult received = _socketRuntime.receiveInto(
      socket, _ingress);
    if (received == IngressIoResult::WOULD_BLOCK) {
      return;
    }
    if (received != IngressIoResult::PROGRESS) {
      _transaction.abort();
      return;
    }
  }

  const bp_http::TransactionConsume consumed = _transaction.consume(
    _ingress.data(), _ingress.length(), nowMs);
  if (consumed.consumed != 0) {
    if (!_ingress.consume(consumed.consumed)) _transaction.abort();
  } else if (_transaction.state() ==
               bp_http::TransactionState::READING_HEADERS ||
             _transaction.state() ==
               bp_http::TransactionState::READING_BODY) {
    _ingress.clear();
    _transaction.abort();
  }

  if (_transaction.state() == bp_http::TransactionState::SENDING_RESPONSE ||
      _transaction.state() == bp_http::TransactionState::ABORTED) {
    _ingress.clear();
  }
}

bool BoundedWebServer::snapshotIsWellFormed() const {
  if (!hasTerminator(_runtimeSnapshot.apHost,
                     sizeof(_runtimeSnapshot.apHost)) ||
      !hasTerminator(_runtimeSnapshot.staHost,
                     sizeof(_runtimeSnapshot.staHost)) ||
      !hasTerminator(_runtimeSnapshot.mdnsHost,
                     sizeof(_runtimeSnapshot.mdnsHost))) {
    return false;
  }
  if (_runtimeSnapshot.apActive && _runtimeSnapshot.apHost[0] == '\0') {
    return false;
  }
  if (_runtimeSnapshot.staActive &&
      _runtimeSnapshot.staHost[0] == '\0' &&
      _runtimeSnapshot.mdnsHost[0] == '\0') {
    return false;
  }
  return true;
}

bool BoundedWebServer::mandatoryResponseHeadersAreExact() const {
  struct RequiredHeader {
    const char* name;
    const char* value;
  };
  static constexpr RequiredHeader kRequired[] = {
    {"Cache-Control", "no-store, max-age=0"},
    {"Pragma", "no-cache"},
    {"X-Content-Type-Options", "nosniff"},
  };
  for (const RequiredHeader& required : kRequired) {
    size_t matches = 0;
    for (RequestArgument* header = _responseHeaders;
         header != nullptr; header = header->next) {
      if (!header->key.equalsIgnoreCase(required.name)) continue;
      ++matches;
      if (header->value != required.value) return false;
    }
    if (matches != 1) return false;
  }
  return true;
}

void BoundedWebServer::processPolicy() {
  if (_transaction.state() != bp_http::TransactionState::WAIT_POLICY) return;
  secureZero(&_runtimeSnapshot, sizeof(_runtimeSnapshot));
  GateResult decision{};
  bool evaluated = false;
  try {
    if (_gate != nullptr && _snapshotProvider != nullptr &&
        _snapshotProvider(_snapshotContext, _runtimeSnapshot) &&
        snapshotIsWellFormed()) {
      InterfaceSnapshot network{};
      network.acceptedLocalAddress = _acceptedLocalAddress;
      network.apAddress = _runtimeSnapshot.apAddress;
      network.staAddress = _runtimeSnapshot.staAddress;
      network.apActive = _runtimeSnapshot.apActive;
      network.staActive = _runtimeSnapshot.staActive;
      network.apPurpose = _runtimeSnapshot.apPurpose;
      network.apHost = _runtimeSnapshot.apHost;
      network.staHost = _runtimeSnapshot.staHost;
      network.mdnsHost = _runtimeSnapshot.mdnsHost;

      decision = _gate->evaluate(
        _transaction.request(), _runtimeSnapshot.security, network,
        _remoteAddress, _socketRuntime.nowMs());
      evaluated = true;
    }
  } catch (...) {
    evaluated = false;
  }
  secureZero(&_runtimeSnapshot, sizeof(_runtimeSnapshot));

  if (!evaluated) {
    secureZero(&_runtimeSnapshot, sizeof(_runtimeSnapshot));
    (void)_transaction.rejectPolicy(503, _socketRuntime.nowMs());
    _ingress.clear();
    return;
  }
  const uint32_t policyCompletedAt = _socketRuntime.nowMs();

  if (!decision.allowed) {
    if (decision.status == 405) {
      bool known = false;
      const bp_http::AllowedMethods allowed = allowedMethodsForPath(
        _transaction.request().view().path, known);
      if (known) {
        (void)_transaction.rejectPolicy(405, policyCompletedAt, allowed);
      } else {
        (void)_transaction.rejectPolicy(500, policyCompletedAt);
      }
    } else {
      (void)_transaction.rejectPolicy(decision.status, policyCompletedAt);
    }
    _ingress.clear();
    return;
  }

  _currentRole = decision.role;
  _currentRequestInterface = decision.requestInterface;
  _currentRoute = decision.route;
  const bool accepted = _transaction.acceptPolicy(
    decision.bodyMode, decision.bodyCap, policyCompletedAt);
  if (!accepted ||
      _transaction.state() == bp_http::TransactionState::DISPATCH_READY) {
    _ingress.clear();
  }
  if (!accepted) {
    _currentRole = AccessRole::NONE;
    _currentRequestInterface = RequestInterface::UNKNOWN;
    _currentRoute = nullptr;
  }
}

void BoundedWebServer::setCollectedHeader(const char* name,
                                          const char* value) {
  if (name == nullptr || value == nullptr || value[0] == '\0') return;
  for (RequestArgument* header = _currentHeaders;
       header != nullptr; header = header->next) {
    if (header->key.equalsIgnoreCase(name)) {
      header->value = value;
      return;
    }
  }
}

void BoundedWebServer::selectCurrentHandler() {
  _currentHandler = nullptr;
  for (RequestHandler* handler = _firstHandler;
       handler != nullptr; handler = handler->next()) {
    if (handler->canHandle(*this, _currentMethod, _currentUri)) {
      _currentHandler = handler;
      return;
    }
  }
}

bool BoundedWebServer::materializeBaseRequest() {
  wipeBaseRequestState();
  const bp_http::RequestView& view = _transaction.request().view();

  const size_t pathLength = boundedLength(view.path, sizeof(view.path));
  const size_t hostLength = boundedLength(view.host, sizeof(view.host));
  if (pathLength >= sizeof(view.path) || hostLength >= sizeof(view.host)) {
    return false;
  }

  _currentMethod = view.method == bp_http::RequestMethod::GET
    ? HTTP_GET : HTTP_POST;
  _currentVersion = 1;
  _clientContentLength = static_cast<int>(view.contentLength);
  _currentUri = view.path;
  _hostHeader = view.host;
  if (_currentUri.length() != pathLength ||
      _hostHeader.length() != hostLength) {
    wipeBaseRequestState();
    return false;
  }

  const size_t fieldCount = _formValidator.fieldCount();
  if (fieldCount != 0) {
    _currentArgs = new (std::nothrow) RequestArgument[fieldCount + 1];
    if (_currentArgs == nullptr) {
      wipeBaseRequestState();
      return false;
    }
    _currentArgCount = static_cast<int>(fieldCount);
    for (size_t i = 0; i < fieldCount; ++i) {
      _currentArgs[i].key = _formValidator.key(i);
      _currentArgs[i].value = _formValidator.value(i);
      if (_currentArgs[i].key.length() != _formValidator.keyLength(i) ||
          _currentArgs[i].value.length() != _formValidator.valueLength(i)) {
        wipeBaseRequestState();
        return false;
      }
    }
  }
  _currentArgCount = static_cast<int>(fieldCount);

  for (RequestArgument* header = _currentHeaders;
       header != nullptr; header = header->next) {
    secureWipeString(header->value);
  }
  setCollectedHeader("Origin", view.origin);
  setCollectedHeader("Referer", view.referer);
  selectCurrentHandler();
  return true;
}

void BoundedWebServer::wipeResponseHeaders() {
  for (RequestArgument* header = _responseHeaders;
       header != nullptr; header = header->next) {
    secureWipeString(header->key);
    secureWipeString(header->value);
  }
  _clearResponseHeaders();
}

void BoundedWebServer::wipeBaseRequestState() {
  if (_currentArgs != nullptr) {
    for (int i = 0; i < _currentArgCount; ++i) {
      secureWipeString(_currentArgs[i].key);
      secureWipeString(_currentArgs[i].value);
    }
    delete[] _currentArgs;
    _currentArgs = nullptr;
  }
  _currentArgCount = 0;
  if (_postArgs != nullptr) {
    for (int i = 0; i < _postArgsLen; ++i) {
      secureWipeString(_postArgs[i].key);
      secureWipeString(_postArgs[i].value);
    }
    delete[] _postArgs;
    _postArgs = nullptr;
  }
  _postArgsLen = 0;
  for (RequestArgument* header = _currentHeaders;
       header != nullptr; header = header->next) {
    secureWipeString(header->value);
  }
  secureWipeString(_currentUri);
  secureWipeString(_hostHeader);
  _currentHandler = nullptr;
  _currentMethod = HTTP_ANY;
  _currentVersion = 0;
  _clientContentLength = 0;
  _currentUpload.reset();
  _currentRaw.reset();
}

void BoundedWebServer::dispatchReadyRequest() {
  if (_transaction.state() != bp_http::TransactionState::DISPATCH_READY) {
    return;
  }
  int dispatchFailureStatus = 0;
  bool captureStarted = false;
  try {
    const bp_http::RequestView& view = _transaction.request().view();
    const size_t queryLength = boundedLength(view.query, sizeof(view.query));
    if (queryLength >= sizeof(view.query) ||
        !_formValidator.validate(view.query, queryLength,
                                 _transaction.request().body(),
                                 _transaction.request().bodyLength())) {
      dispatchFailureStatus = 400;
    } else if (!materializeBaseRequest()) {
      dispatchFailureStatus = 503;
    } else {
      const uint32_t dispatchStartedAt = _socketRuntime.nowMs();
      captureStarted = _transaction.beginDispatch(dispatchStartedAt);
      if (captureStarted) {
        wipeResponseHeaders();
        _contentLength = CONTENT_LENGTH_NOT_SET;
        _responseCode = 0;
        _chunked = false;
        sendHeader("Cache-Control", "no-store, max-age=0");
        sendHeader("Pragma", "no-cache");
        sendHeader("X-Content-Type-Options", "nosniff");

        if (!mandatoryResponseHeadersAreExact()) {
          dispatchFailureStatus = 503;
        } else if (_chain != nullptr) {
          (void)_chain->runChain(*this,
                                 [this]() { return _handleRequest(); });
        } else {
          (void)_handleRequest();
        }
        if (dispatchFailureStatus == 0 &&
            (!mandatoryResponseHeadersAreExact() ||
             !_transaction.capturedResponseIsValidHttp1())) {
          dispatchFailureStatus = 503;
        }
      } else if (_transaction.state() ==
                   bp_http::TransactionState::DISPATCH_READY) {
        dispatchFailureStatus = 503;
      }
    }
  } catch (const std::bad_alloc&) {
    dispatchFailureStatus = 503;
  } catch (...) {
    dispatchFailureStatus = 500;
  }

  const uint32_t finishedAt = _socketRuntime.nowMs();
  wipeBaseRequestState();
  wipeResponseHeaders();
  _formValidator.clear();
  _ingress.clear();
  if (dispatchFailureStatus == 0 && captureStarted) {
    (void)_transaction.finishDispatch(finishedAt);
  } else if (_transaction.state() ==
               bp_http::TransactionState::CAPTURING_RESPONSE) {
    (void)_transaction.rejectCapture(dispatchFailureStatus, finishedAt);
  } else if (_transaction.state() ==
               bp_http::TransactionState::DISPATCH_READY) {
    (void)_transaction.rejectDispatch(dispatchFailureStatus, finishedAt);
  }
  _currentRole = AccessRole::NONE;
  _currentRequestInterface = RequestInterface::UNKNOWN;
  _currentRoute = nullptr;
}

void BoundedWebServer::pumpResponse(uint32_t nowMs) {
  if (_transaction.state() != bp_http::TransactionState::SENDING_RESPONSE) {
    return;
  }
  if (!_transaction.poll(nowMs)) return;
  const bp_http::ResponseChunk chunk = _transaction.nextOutput();
  if (chunk.data == nullptr || chunk.length == 0) {
    _transaction.abort();
    return;
  }

  const int socket = _currentClient.fd();
  if (socket < 0) {
    _transaction.abort();
    return;
  }
  const SocketWriteResult sent = _socketRuntime.sendSome(
    socket, chunk.data, chunk.length);
  if (sent.status == SocketWriteStatus::WOULD_BLOCK) {
    return;
  }
  if (sent.status != SocketWriteStatus::PROGRESS ||
      !_transaction.acknowledgeOutput(sent.length)) {
    _transaction.abort();
  }
}

void BoundedWebServer::finishActiveClient(bool runDeferredAction) {
  BoundedWebDeferredAction action = runDeferredAction
    ? _deferredAction : nullptr;
  void* actionContext = runDeferredAction ? _deferredContext : nullptr;
  _deferredAction = nullptr;
  _deferredContext = nullptr;
  _deferredFallbackPending = false;

  if (!_transaction.terminal()) _transaction.abort();
  _socketRuntime.cancelDrain();
  _ingress.clear();
  _formValidator.clear();
  secureZero(&_runtimeSnapshot, sizeof(_runtimeSnapshot));
  wipeBaseRequestState();
  wipeResponseHeaders();
  _currentClient.stop();
  _currentClient = NetworkClient();
  _currentStatus = HC_NONE;
  _clientActive = false;
  _acceptedLocalAddress = 0;
  _remoteAddress = 0;
  _currentRole = AccessRole::NONE;
  _currentRequestInterface = RequestInterface::UNKNOWN;
  _currentRoute = nullptr;

  if (action != nullptr) action(actionContext);
}

void BoundedWebServer::handleClient() {
  const uint32_t nowMs = _socketRuntime.nowMs();
  if (!_clientActive) {
    try {
      acceptClient(nowMs);
    } catch (...) {
      _transaction.abort();
      _socketRuntime.cancelDrain();
      _ingress.clear();
      _formValidator.clear();
      secureZero(&_runtimeSnapshot, sizeof(_runtimeSnapshot));
      wipeBaseRequestState();
      wipeResponseHeaders();
      _currentClient.stop();
      _currentClient = NetworkClient();
      _currentStatus = HC_NONE;
      _clientActive = false;
      _acceptedLocalAddress = 0;
      _remoteAddress = 0;
      _currentRole = AccessRole::NONE;
      _currentRequestInterface = RequestInterface::UNKNOWN;
      _currentRoute = nullptr;
      _deferredAction = nullptr;
      _deferredContext = nullptr;
      _deferredFallbackPending = false;
      return;
    }
    if (!_clientActive) return;
  }

  if (_deferredFallbackPending) {
    _deferredFallbackPending = false;
    finishActiveClient(true);
    return;
  }

  if (_socketRuntime.drainActive()) {
    const DrainIoResult drain = _socketRuntime.pollDrain(
      _currentClient.fd());
    if (drain != DrainIoResult::WAITING) finishActiveClient(true);
    return;
  }

  (void)_transaction.poll(nowMs);
  processIngress(nowMs);
  processPolicy();
  dispatchReadyRequest();
  pumpResponse(_socketRuntime.nowMs());

  if (_transaction.state() == bp_http::TransactionState::COMPLETE &&
      _deferredAction != nullptr) {
    if (!_socketRuntime.beginDrain()) _deferredFallbackPending = true;
    return;
  }
  if (_transaction.terminal()) finishActiveClient(true);
}

void BoundedWebServer::close() {
  if (_clientActive) finishActiveClient(true);
  WebServer::close();
}

bool BoundedWebServer::deferAfterResponse(
    BoundedWebDeferredAction action, void* context) {
  if (!_clientActive || action == nullptr || _deferredAction != nullptr ||
      _transaction.state() !=
        bp_http::TransactionState::CAPTURING_RESPONSE) {
    return false;
  }
  _deferredAction = action;
  _deferredContext = context;
  return true;
}

bool BoundedWebServer::recordClaimResult(bool tokenAccepted,
                                         uint32_t nowMs) {
  if (_gate == nullptr || _currentRoute == nullptr ||
      !isClaimRoute(*_currentRoute) || !_currentRoute->mutation ||
      _transaction.state() !=
        bp_http::TransactionState::CAPTURING_RESPONSE) {
    return false;
  }
  return _gate->recordClaimResult(_remoteAddress, tokenAccepted, nowMs);
}

size_t BoundedWebServer::_currentClientWrite(const char* bytes,
                                             size_t length) {
  const size_t captured = _transaction.capture(
    reinterpret_cast<const uint8_t*>(bytes), length);
  return captured == length || _transaction.state() ==
           bp_http::TransactionState::CAPTURING_RESPONSE
    ? length : 0;
}

size_t BoundedWebServer::_currentClientWrite_P(PGM_P bytes,
                                               size_t length) {
  return _currentClientWrite(reinterpret_cast<const char*>(bytes), length);
}

}  // namespace bp_web
