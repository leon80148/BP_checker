#ifndef WEB_REQUEST_GATE_H
#define WEB_REQUEST_GATE_H

#include <cstddef>
#include <cstdint>

#include "BoundedHttpRequest.h"
#include "WebAccessPolicy.h"

namespace bp_web {

enum class ApPurpose : uint8_t {
  NONE,
  PROVISIONING,
  RECOVERY,
};

struct InterfaceSnapshot {
  uint32_t acceptedLocalAddress = 0;
  uint32_t apAddress = 0;
  uint32_t staAddress = 0;
  bool apActive = false;
  bool staActive = false;
  ApPurpose apPurpose = ApPurpose::NONE;
  const char* apHost = nullptr;
  const char* staHost = nullptr;
  const char* mdnsHost = nullptr;
};

constexpr RequestInterface classifyRequestInterface(
    const InterfaceSnapshot& network) {
  if (network.acceptedLocalAddress == 0) return RequestInterface::UNKNOWN;
  const bool apMatches = network.apActive && network.apAddress != 0 &&
                         network.acceptedLocalAddress == network.apAddress;
  const bool staMatches = network.staActive && network.staAddress != 0 &&
                          network.acceptedLocalAddress == network.staAddress;
  if (apMatches == staMatches) return RequestInterface::UNKNOWN;
  if (staMatches) return RequestInterface::STA;
  if (network.apPurpose == ApPurpose::PROVISIONING) {
    return RequestInterface::PROVISIONING_AP;
  }
  if (network.apPurpose == ApPurpose::RECOVERY) {
    return RequestInterface::RECOVERY_AP;
  }
  return RequestInterface::UNKNOWN;
}

inline uint8_t gateAsciiLower(uint8_t value) {
  return value >= 'A' && value <= 'Z'
    ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
}

inline size_t boundedCStringLength(const char* value, size_t limit) {
  if (value == nullptr) return limit + 1;
  size_t length = 0;
  while (length <= limit && value[length] != '\0') ++length;
  return length;
}

inline bool normalizedHostEquals(const char* candidate,
                                 const char* expected) {
  constexpr size_t kHostLimit = 512;
  size_t candidateLength = boundedCStringLength(candidate, kHostLimit);
  size_t expectedLength = boundedCStringLength(expected, kHostLimit);
  if (candidateLength == 0 || candidateLength > kHostLimit ||
      expectedLength == 0 || expectedLength > kHostLimit) {
    return false;
  }
  if (candidateLength >= 3 &&
      candidate[candidateLength - 3] == ':' &&
      candidate[candidateLength - 2] == '8' &&
      candidate[candidateLength - 1] == '0') {
    candidateLength -= 3;
  }
  if (expectedLength >= 3 && expected[expectedLength - 3] == ':' &&
      expected[expectedLength - 2] == '8' &&
      expected[expectedLength - 1] == '0') {
    expectedLength -= 3;
  }
  if (candidateLength == 0 || candidateLength != expectedLength) return false;
  for (size_t i = 0; i < candidateLength; ++i) {
    const uint8_t candidateByte = static_cast<uint8_t>(candidate[i]);
    const uint8_t expectedByte = static_cast<uint8_t>(expected[i]);
    const bool candidateAllowed =
      (candidateByte >= 'A' && candidateByte <= 'Z') ||
      (candidateByte >= 'a' && candidateByte <= 'z') ||
      (candidateByte >= '0' && candidateByte <= '9') ||
      candidateByte == '.' || candidateByte == '-' || candidateByte == '_';
    if (!candidateAllowed ||
        gateAsciiLower(candidateByte) != gateAsciiLower(expectedByte)) {
      return false;
    }
  }
  return true;
}

inline bool hostAllowedForInterface(const char* host,
                                    RequestInterface requestInterface,
                                    const InterfaceSnapshot& network) {
  if (requestInterface == RequestInterface::PROVISIONING_AP ||
      requestInterface == RequestInterface::RECOVERY_AP) {
    return normalizedHostEquals(host, network.apHost);
  }
  if (requestInterface == RequestInterface::STA) {
    return normalizedHostEquals(host, network.staHost) ||
           normalizedHostEquals(host, network.mdnsHost);
  }
  return false;
}

inline bool urlMatchesRequestHost(const char* url, const char* requestHost,
                                  bool allowPath) {
  constexpr size_t kUrlLimit = 256;
  const size_t urlLength = boundedCStringLength(url, kUrlLimit);
  if (urlLength > kUrlLimit || urlLength < 8 || requestHost == nullptr) {
    return false;
  }
  static constexpr char kScheme[] = "http://";
  for (size_t i = 0; i < sizeof(kScheme) - 1; ++i) {
    if (gateAsciiLower(static_cast<uint8_t>(url[i])) !=
        static_cast<uint8_t>(kScheme[i])) {
      return false;
    }
  }
  const size_t authorityStart = sizeof(kScheme) - 1;
  size_t authorityEnd = authorityStart;
  while (authorityEnd < urlLength && url[authorityEnd] != '/' &&
         url[authorityEnd] != '?' && url[authorityEnd] != '#') {
    ++authorityEnd;
  }
  const size_t authorityLength = authorityEnd - authorityStart;
  if (authorityLength == 0 || authorityLength > 512) return false;
  if (!allowPath && authorityEnd != urlLength) return false;
  if (allowPath && authorityEnd != urlLength && url[authorityEnd] != '/') {
    return false;
  }
  char authority[513] = {};
  for (size_t i = 0; i < authorityLength; ++i) {
    authority[i] = url[authorityStart + i];
  }
  authority[authorityLength] = '\0';
  return normalizedHostEquals(authority, requestHost);
}

inline bool csrfAllowedForMutation(const bp_http::RequestView& request) {
  if (request.origin[0] != '\0') {
    return urlMatchesRequestHost(request.origin, request.host, false);
  }
  if (request.referer[0] != '\0') {
    return urlMatchesRequestHost(request.referer, request.host, true);
  }
  return true;
}

struct SecurityGateSnapshot {
  DeviceSecurityAvailability availability =
    DeviceSecurityAvailability::LOCKED;
  DeviceClaimState claimState = DeviceClaimState::UNCLAIMED;
  BasicCredentials credentials{};
};

enum class GateReason : uint8_t {
  ALLOWED,
  UNKNOWN_ROUTE,
  WRONG_METHOD,
  BAD_INTERFACE,
  BAD_HOST,
  SECURITY_UNAVAILABLE,
  STATE_OR_INTERFACE,
  RATE_LIMITED,
  AUTH_REQUIRED,
  ROLE_FORBIDDEN,
  CSRF_REJECTED,
  INVALID_CREDENTIAL_STATE,
  REQUEST_NOT_COMPLETE,
};

struct GateResult {
  bool allowed = false;
  int status = 500;
  GateReason reason = GateReason::SECURITY_UNAVAILABLE;
  const RoutePolicy* route = nullptr;
  AccessRole role = AccessRole::NONE;
  RequestInterface requestInterface = RequestInterface::UNKNOWN;
  bp_http::BodyMode bodyMode = bp_http::BodyMode::NONE;
  uint32_t bodyCap = 0;
};

inline HttpMethod gateMethod(bp_http::RequestMethod method) {
  return method == bp_http::RequestMethod::GET
    ? HttpMethod::GET : HttpMethod::POST;
}

inline bool registeredPathExists(const char* path) {
  if (path == nullptr) return false;
  for (size_t i = 0; i < kRoutePolicyCount; ++i) {
    if (cStringEquals(kRoutePolicies[i].path, path)) return true;
  }
  return false;
}

class WebRequestGate {
public:
  explicit WebRequestGate(AuthFailureLimiter* limiter) : _limiter(limiter) {}

  bool recordClaimResult(uint32_t remoteAddress, bool tokenAccepted,
                         uint32_t nowMs) {
    if (_limiter == nullptr) return false;
    if (tokenAccepted) {
      _limiter->recordSuccess(remoteAddress);
    } else {
      _limiter->recordFailure(remoteAddress, nowMs);
    }
    return true;
  }

  GateResult evaluate(const bp_http::BoundedHttpRequest& parsedRequest,
                      const SecurityGateSnapshot& security,
                      const InterfaceSnapshot& network,
                      uint32_t remoteAddress, uint32_t nowMs) {
    if (parsedRequest.state() != bp_http::RequestState::WAIT_POLICY) {
      GateResult result{};
      result.status = 400;
      result.reason = GateReason::REQUEST_NOT_COMPLETE;
      return result;
    }
    return evaluateCompleted(parsedRequest.view(), security, network,
                             remoteAddress, nowMs);
  }

private:
  GateResult evaluateCompleted(const bp_http::RequestView& request,
                               const SecurityGateSnapshot& security,
                               const InterfaceSnapshot& network,
                               uint32_t remoteAddress, uint32_t nowMs) {
    GateResult result{};
    result.requestInterface = classifyRequestInterface(network);
    if (result.requestInterface == RequestInterface::UNKNOWN) {
      result.status = 404;
      result.reason = GateReason::BAD_INTERFACE;
      return result;
    }
    if (!hostAllowedForInterface(request.host, result.requestInterface,
                                 network)) {
      result.status = 403;
      result.reason = GateReason::BAD_HOST;
      return result;
    }

    if (request.method != bp_http::RequestMethod::GET &&
        request.method != bp_http::RequestMethod::POST) {
      result.status = 400;
      result.reason = GateReason::WRONG_METHOD;
      return result;
    }

    const HttpMethod method = gateMethod(request.method);
    result.route = findRoutePolicy(method, request.path);
    if (result.route == nullptr) {
      result.status = registeredPathExists(request.path) ? 405 : 404;
      result.reason = registeredPathExists(request.path)
        ? GateReason::WRONG_METHOD : GateReason::UNKNOWN_ROUTE;
      return result;
    }
    result.bodyCap = result.route->bodyCap;
    if (!routeBodyPolicyIsValid(*result.route)) {
      result.status = 500;
      result.reason = GateReason::UNKNOWN_ROUTE;
      return result;
    }
    switch (result.route->bodyKind) {
      case RouteBodyKind::NONE:
        result.bodyMode = bp_http::BodyMode::NONE;
        break;
      case RouteBodyKind::FORM:
        result.bodyMode = bp_http::BodyMode::SMALL_FORM;
        break;
      case RouteBodyKind::STREAM:
        result.bodyMode = bp_http::BodyMode::STREAM;
        break;
      default:
        result.status = 500;
        result.reason = GateReason::UNKNOWN_ROUTE;
        return result;
    }

    if (security.availability != DeviceSecurityAvailability::READY) {
      result.status = 503;
      result.reason = GateReason::SECURITY_UNAVAILABLE;
      return result;
    }
    if (isClaimRoute(*result.route)) {
      const AccessDecision access = authorizeRoute(
        method, request.path, security.claimState, result.requestInterface,
        AccessRole::NONE);
      if (access != AccessDecision::ALLOW) {
        result.status = 404;
        result.reason = GateReason::STATE_OR_INTERFACE;
        return result;
      }
      if (result.route->mutation) {
        if (_limiter == nullptr) {
          result.status = 503;
          result.reason = GateReason::INVALID_CREDENTIAL_STATE;
          return result;
        }
        if (!_limiter->allowAttempt(remoteAddress, nowMs)) {
          result.status = 429;
          result.reason = GateReason::RATE_LIMITED;
          return result;
        }
      }
      if (result.route->mutation && !csrfAllowedForMutation(request)) {
        result.status = 403;
        result.reason = GateReason::CSRF_REJECTED;
        return result;
      }
      result.allowed = true;
      result.status = 0;
      result.reason = GateReason::ALLOWED;
      return result;
    }

    const AccessDecision contextAccess = authorizeRoute(
      method, request.path, security.claimState, result.requestInterface,
      AccessRole::ADMIN);
    if (contextAccess != AccessDecision::ALLOW) {
      result.status = 404;
      result.reason = GateReason::STATE_OR_INTERFACE;
      return result;
    }
    const bool staffCanonical = isCanonicalCredential(
      security.credentials.staffSecret,
      security.credentials.staffSecretLength);
    const bool adminCanonical = isCanonicalCredential(
      security.credentials.adminSecret,
      security.credentials.adminSecretLength);
    const bool credentialsAliased = constantTimeCredentialEquals(
      reinterpret_cast<const uint8_t*>(security.credentials.staffSecret),
      security.credentials.staffSecretLength,
      security.credentials.adminSecret,
      security.credentials.adminSecretLength);
    if (!staffCanonical || !adminCanonical || credentialsAliased ||
        _limiter == nullptr) {
      result.status = 503;
      result.reason = GateReason::INVALID_CREDENTIAL_STATE;
      return result;
    }
    if (!_limiter->allowAttempt(remoteAddress, nowMs)) {
      result.status = 429;
      result.reason = GateReason::RATE_LIMITED;
      return result;
    }
    const size_t authorizationLength = boundedCStringLength(
      request.authorization, bp_web::kMaxAuthorizationHeaderBytes);
    result.role = authorizationLength <= kMaxAuthorizationHeaderBytes
      ? authenticateBasic(request.authorization, authorizationLength,
                          security.credentials)
      : AccessRole::NONE;
    if (result.role == AccessRole::NONE) {
      _limiter->recordFailure(remoteAddress, nowMs);
      result.status = 401;
      result.reason = GateReason::AUTH_REQUIRED;
      return result;
    }
    _limiter->recordSuccess(remoteAddress);

    const AccessDecision roleAccess = authorizeRoute(
      method, request.path, security.claimState, result.requestInterface,
      result.role);
    if (roleAccess != AccessDecision::ALLOW) {
      result.status = 403;
      result.reason = GateReason::ROLE_FORBIDDEN;
      return result;
    }
    if (result.route->mutation && !csrfAllowedForMutation(request)) {
      result.status = 403;
      result.reason = GateReason::CSRF_REJECTED;
      return result;
    }
    result.allowed = true;
    result.status = 0;
    result.reason = GateReason::ALLOWED;
    return result;
  }

  AuthFailureLimiter* _limiter;
};

}  // namespace bp_web

#endif
