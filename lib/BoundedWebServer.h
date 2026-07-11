#ifndef BOUNDED_WEB_SERVER_H
#define BOUNDED_WEB_SERVER_H

#include <WebServer.h>

#include <cstddef>
#include <cstdint>

#include "BoundedHttpTransaction.h"
#include "BoundedSocketRuntime.h"
#include "BoundedWebInput.h"
#include "WebRequestGate.h"

namespace bp_web {

struct BoundedWebRuntimeSnapshot {
  static constexpr size_t kHostCapacity = 65;

  SecurityGateSnapshot security{};
  uint32_t apAddress = 0;
  uint32_t staAddress = 0;
  bool apActive = false;
  bool staActive = false;
  ApPurpose apPurpose = ApPurpose::NONE;
  char apHost[kHostCapacity] = {};
  char staHost[kHostCapacity] = {};
  char mdnsHost[kHostCapacity] = {};
};

using BoundedWebSnapshotProvider = bool (*)(
  void* context, BoundedWebRuntimeSnapshot& snapshot);
using BoundedWebDeferredAction = void (*)(void* context);

class BoundedWebServer final : public WebServer {
public:
  explicit BoundedWebServer(int port = 80);

  BoundedWebServer(const BoundedWebServer&) = delete;
  BoundedWebServer& operator=(const BoundedWebServer&) = delete;

  void configureAccess(WebRequestGate* gate,
                       BoundedWebSnapshotProvider snapshotProvider,
                       void* snapshotContext);

  void handleClient() override;
  void close() override;

  bool deferAfterResponse(BoundedWebDeferredAction action, void* context);
  bool recordClaimResult(bool tokenAccepted, uint32_t nowMs);

  AccessRole currentRole() const { return _currentRole; }
  RequestInterface currentRequestInterface() const {
    return _currentRequestInterface;
  }
  const RoutePolicy* currentRoute() const { return _currentRoute; }
  uint32_t currentRemoteAddress() const { return _remoteAddress; }
  bool hasActiveClient() const { return _clientActive; }

protected:
  size_t _currentClientWrite(const char* bytes, size_t length) override;
  size_t _currentClientWrite_P(PGM_P bytes, size_t length) override;

private:
  WebRequestGate* _gate = nullptr;
  BoundedWebSnapshotProvider _snapshotProvider = nullptr;
  void* _snapshotContext = nullptr;

  BoundedSocketRuntime _socketRuntime;
  bp_http::BoundedHttpTransaction _transaction;
  BoundedIngressBuffer _ingress;
  BoundedFormValidator _formValidator;
  BoundedWebRuntimeSnapshot _runtimeSnapshot{};

  bool _clientActive = false;
  uint32_t _acceptedLocalAddress = 0;
  uint32_t _remoteAddress = 0;
  AccessRole _currentRole = AccessRole::NONE;
  RequestInterface _currentRequestInterface = RequestInterface::UNKNOWN;
  const RoutePolicy* _currentRoute = nullptr;

  BoundedWebDeferredAction _deferredAction = nullptr;
  void* _deferredContext = nullptr;
  bool _deferredFallbackPending = false;

  static void secureZero(void* target, size_t length);
  static void secureWipeString(String& value);
  static bool hasTerminator(const char* value, size_t capacity);
  static size_t boundedLength(const char* value, size_t capacity);
  static bp_http::AllowedMethods allowedMethodsForPath(const char* path,
                                                       bool& known);

  void acceptClient(uint32_t nowMs);
  void processIngress(uint32_t nowMs);
  void processPolicy();
  void dispatchReadyRequest();
  void pumpResponse(uint32_t nowMs);
  void finishActiveClient(bool runDeferredAction);

  bool snapshotIsWellFormed() const;
  bool mandatoryResponseHeadersAreExact() const;
  bool materializeBaseRequest();
  void selectCurrentHandler();
  void setCollectedHeader(const char* name, const char* value);
  void wipeBaseRequestState();
  void wipeResponseHeaders();
};

}  // namespace bp_web

#endif
