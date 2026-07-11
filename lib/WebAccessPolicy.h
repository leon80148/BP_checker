#ifndef WEB_ACCESS_POLICY_H
#define WEB_ACCESS_POLICY_H

#include "DeviceSecurity.h"

#include <cstddef>
#include <cstdint>

namespace bp_web {

enum class HttpMethod : uint8_t {
  GET,
  POST,
  OTHER,
};

enum class AccessRole : uint8_t {
  NONE = 0,
  STAFF = 1,
  ADMIN = 2,
};

enum class WebSurface : uint8_t {
  MONITOR_NAV = 0,
  HISTORY_NAV,
  ADMIN_WIFI_NAV,
  ADMIN_MODEL_NAV,
  ADMIN_SECURITY_NAV,
  ADMIN_POLICY_NAV,
  RESET_CONTROL,
  CLEAR_HISTORY_CONTROL,
  POLICY_UPDATE_CONTROL,
};

inline constexpr bool surfaceVisible(AccessRole role, WebSurface surface) {
  if (role != AccessRole::STAFF && role != AccessRole::ADMIN) return false;
  switch (surface) {
    case WebSurface::MONITOR_NAV:
    case WebSurface::HISTORY_NAV:
      return true;
    case WebSurface::ADMIN_WIFI_NAV:
    case WebSurface::ADMIN_MODEL_NAV:
    case WebSurface::ADMIN_SECURITY_NAV:
    case WebSurface::ADMIN_POLICY_NAV:
    case WebSurface::RESET_CONTROL:
    case WebSurface::CLEAR_HISTORY_CONTROL:
    case WebSurface::POLICY_UPDATE_CONTROL:
      return role == AccessRole::ADMIN;
    default:
      return false;
  }
}

enum class RequestInterface : uint8_t {
  PROVISIONING_AP,
  RECOVERY_AP,
  STA,
  UNKNOWN,
};

enum class AccessDecision : uint8_t {
  ALLOW,
  DENY_UNKNOWN_ROUTE,
  DENY_INVALID_CONTEXT,
  DENY_STATE,
  DENY_INTERFACE,
  DENY_ROLE,
};

struct RoutePolicy {
  HttpMethod method;
  const char* path;
  AccessRole requiredRole;
  uint16_t bodyCap;
  bool mutation;
  bool noStore;
};

// This is the complete product route registry. There is deliberately no
// wildcard entry: a new handler remains unreachable until it is classified
// here. All responses can contain credentials, configuration, measurements,
// or operational state, so every route requests Cache-Control: no-store.
inline constexpr RoutePolicy kRoutePolicies[] = {
  {HttpMethod::GET,  "/claim",              AccessRole::NONE,  0,   false, true},
  {HttpMethod::POST, "/claim",              AccessRole::NONE,  96,  true,  true},
  {HttpMethod::GET,  "/",                   AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/data",               AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/history",            AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/export.csv",         AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/api/history",        AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/api/latest",         AccessRole::STAFF, 0,   false, true},
  {HttpMethod::GET,  "/config",             AccessRole::ADMIN, 0,   false, true},
  {HttpMethod::POST, "/configure",           AccessRole::ADMIN, 512, true,  true},
  {HttpMethod::POST, "/clear_history",       AccessRole::ADMIN, 0,   true,  true},
  {HttpMethod::GET,  "/bp_model",           AccessRole::ADMIN, 0,   false, true},
  {HttpMethod::POST, "/set_bp_model",       AccessRole::ADMIN, 64,  true,  true},
  {HttpMethod::GET,  "/security",           AccessRole::ADMIN, 0,   false, true},
  {HttpMethod::POST, "/rotate_credentials", AccessRole::ADMIN, 64,  true,  true},
  {HttpMethod::GET,  "/measurement_policy", AccessRole::ADMIN, 0,   false, true},
  {HttpMethod::POST, "/set_measurement_policy", AccessRole::ADMIN, 512, true, true},
  {HttpMethod::POST, "/reset",               AccessRole::ADMIN, 0,   true,  true},
};

inline constexpr size_t kRoutePolicyCount =
  sizeof(kRoutePolicies) / sizeof(kRoutePolicies[0]);

constexpr bool cStringEquals(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) return left == right;
  size_t index = 0;
  while (left[index] != '\0' && right[index] != '\0') {
    if (left[index] != right[index]) return false;
    ++index;
  }
  return left[index] == right[index];
}

constexpr bool routeTableIsValid() {
  if (kRoutePolicyCount != 18) return false;
  for (size_t i = 0; i < kRoutePolicyCount; ++i) {
    const RoutePolicy& route = kRoutePolicies[i];
    if (route.path == nullptr || route.path[0] != '/' || !route.noStore) {
      return false;
    }
    if (route.method == HttpMethod::OTHER) return false;
    if (route.method == HttpMethod::GET &&
        (route.bodyCap != 0 || route.mutation)) {
      return false;
    }
    if (route.method == HttpMethod::POST && !route.mutation) return false;
    for (size_t j = i + 1; j < kRoutePolicyCount; ++j) {
      if (route.method == kRoutePolicies[j].method &&
          cStringEquals(route.path, kRoutePolicies[j].path)) {
        return false;
      }
    }
  }
  return true;
}

static_assert(routeTableIsValid(),
              "Web route registry must be complete, unique, and default-deny");

constexpr const RoutePolicy* findRoutePolicy(HttpMethod method,
                                              const char* path) {
  if (method == HttpMethod::OTHER || path == nullptr) return nullptr;
  for (size_t i = 0; i < kRoutePolicyCount; ++i) {
    if (kRoutePolicies[i].method == method &&
        cStringEquals(kRoutePolicies[i].path, path)) {
      return &kRoutePolicies[i];
    }
  }
  return nullptr;
}

constexpr bool isClaimRoute(const RoutePolicy& route) {
  return cStringEquals(route.path, "/claim");
}

constexpr bool isKnownHttpMethod(HttpMethod method) {
  return method == HttpMethod::GET || method == HttpMethod::POST;
}

constexpr bool isKnownClaimState(::DeviceClaimState claimState) {
  return claimState == ::DeviceClaimState::UNCLAIMED ||
         claimState == ::DeviceClaimState::CLAIMED;
}

constexpr bool isKnownRequestInterface(RequestInterface requestInterface) {
  return requestInterface == RequestInterface::PROVISIONING_AP ||
         requestInterface == RequestInterface::RECOVERY_AP ||
         requestInterface == RequestInterface::STA;
}

constexpr bool isKnownAccessRole(AccessRole role) {
  return role == AccessRole::NONE || role == AccessRole::STAFF ||
         role == AccessRole::ADMIN;
}

constexpr AccessDecision authorizeRoute(HttpMethod method, const char* path,
                                        ::DeviceClaimState claimState,
                                        RequestInterface requestInterface,
                                        AccessRole role) {
  const RoutePolicy* route = findRoutePolicy(method, path);
  // Validate the entire request context before any claim-specific/public
  // branch. Corrupt enum values must never gain access through numeric role
  // ordering or a special-case route.
  if (!isKnownHttpMethod(method) || !isKnownClaimState(claimState) ||
      !isKnownRequestInterface(requestInterface) ||
      !isKnownAccessRole(role)) {
    return AccessDecision::DENY_INVALID_CONTEXT;
  }
  if (route == nullptr) return AccessDecision::DENY_UNKNOWN_ROUTE;

  if (isClaimRoute(*route)) {
    if (claimState == ::DeviceClaimState::UNCLAIMED) {
      return requestInterface == RequestInterface::PROVISIONING_AP
        ? AccessDecision::ALLOW : AccessDecision::DENY_INTERFACE;
    }
    return requestInterface == RequestInterface::RECOVERY_AP
      ? AccessDecision::ALLOW : AccessDecision::DENY_STATE;
  }

  if (claimState != ::DeviceClaimState::CLAIMED) {
    return AccessDecision::DENY_STATE;
  }
  if (static_cast<uint8_t>(role) <
      static_cast<uint8_t>(route->requiredRole)) {
    return AccessDecision::DENY_ROLE;
  }
  return AccessDecision::ALLOW;
}

// A 16-byte secret encodes to exactly 22 Base64URL characters without '='.
inline constexpr size_t kCanonicalCredentialChars = 22;
inline constexpr size_t kMaxAuthorizationHeaderBytes = 96;
inline constexpr size_t kMaxDecodedCredentialBytes = 72;

struct BasicCredentials {
  const char* staffSecret;
  size_t staffSecretLength;
  const char* adminSecret;
  size_t adminSecretLength;
};

inline uint8_t asciiLower(uint8_t value) {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<uint8_t>(value + ('a' - 'A'));
  }
  return value;
}

inline bool basicSchemeMatches(const char* header, size_t length) {
  static constexpr char kBasic[] = "basic";
  if (header == nullptr || length < 6 || header[5] != ' ') return false;
  for (size_t i = 0; i < 5; ++i) {
    if (asciiLower(static_cast<uint8_t>(header[i])) !=
        static_cast<uint8_t>(kBasic[i])) {
      return false;
    }
  }
  return true;
}

inline int8_t base64Value(uint8_t value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

inline int8_t base64UrlValue(uint8_t value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '-') return 62;
  if (value == '_') return 63;
  return -1;
}

inline bool isCanonicalCredential(const uint8_t* credential,
                                  size_t credentialLength) {
  if (credential == nullptr ||
      credentialLength != kCanonicalCredentialChars) {
    return false;
  }
  uint8_t invalid = 0;
  for (size_t i = 0; i < kCanonicalCredentialChars; ++i) {
    const int8_t value = base64UrlValue(credential[i]);
    invalid |= value < 0 ? 1u : 0u;
    if (i + 1 == kCanonicalCredentialChars && value >= 0) {
      // 128 bits leave four unused bits in the final six-bit character.
      invalid |= (value & 0x0f) != 0 ? 1u : 0u;
    }
  }
  return invalid == 0;
}

inline bool isCanonicalCredential(const char* credential,
                                  size_t credentialLength) {
  return isCanonicalCredential(
    reinterpret_cast<const uint8_t*>(credential), credentialLength);
}

inline void secureClearBytes(uint8_t* buffer, size_t length) {
  if (buffer == nullptr) return;
  volatile uint8_t* clear = buffer;
  for (size_t i = 0; i < length; ++i) clear[i] = 0;
}

inline bool failDecodedCredential(uint8_t* decoded, size_t decodedCapacity,
                                  size_t& decodedLength) {
  secureClearBytes(decoded, decodedCapacity);
  decodedLength = 0;
  return false;
}

inline bool decodeBase64Strict(const char* encoded, size_t encodedLength,
                               uint8_t* decoded, size_t decodedCapacity,
                               size_t& decodedLength) {
  decodedLength = 0;
  if (encoded == nullptr || decoded == nullptr || encodedLength == 0 ||
      encodedLength % 4 != 0) {
    return failDecodedCredential(decoded, decodedCapacity, decodedLength);
  }

  for (size_t offset = 0; offset < encodedLength; offset += 4) {
    const bool finalQuartet = offset + 4 == encodedLength;
    const uint8_t c0 = static_cast<uint8_t>(encoded[offset]);
    const uint8_t c1 = static_cast<uint8_t>(encoded[offset + 1]);
    const uint8_t c2 = static_cast<uint8_t>(encoded[offset + 2]);
    const uint8_t c3 = static_cast<uint8_t>(encoded[offset + 3]);
    const int8_t v0 = base64Value(c0);
    const int8_t v1 = base64Value(c1);
    if (v0 < 0 || v1 < 0) {
      return failDecodedCredential(decoded, decodedCapacity, decodedLength);
    }

    if (c2 == '=') {
      if (!finalQuartet || c3 != '=' || (v1 & 0x0f) != 0 ||
          decodedLength + 1 > decodedCapacity) {
        return failDecodedCredential(decoded, decodedCapacity, decodedLength);
      }
      decoded[decodedLength++] = static_cast<uint8_t>(
        (static_cast<uint16_t>(v0) << 2) |
        (static_cast<uint16_t>(v1) >> 4));
      continue;
    }

    const int8_t v2 = base64Value(c2);
    if (v2 < 0) {
      return failDecodedCredential(decoded, decodedCapacity, decodedLength);
    }
    if (c3 == '=') {
      if (!finalQuartet || (v2 & 0x03) != 0 ||
          decodedLength + 2 > decodedCapacity) {
        return failDecodedCredential(decoded, decodedCapacity, decodedLength);
      }
      decoded[decodedLength++] = static_cast<uint8_t>(
        (static_cast<uint16_t>(v0) << 2) |
        (static_cast<uint16_t>(v1) >> 4));
      decoded[decodedLength++] = static_cast<uint8_t>(
        (static_cast<uint16_t>(v1) << 4) |
        (static_cast<uint16_t>(v2) >> 2));
      continue;
    }

    const int8_t v3 = base64Value(c3);
    if (v3 < 0 || decodedLength + 3 > decodedCapacity) {
      return failDecodedCredential(decoded, decodedCapacity, decodedLength);
    }
    decoded[decodedLength++] = static_cast<uint8_t>(
      (static_cast<uint16_t>(v0) << 2) |
      (static_cast<uint16_t>(v1) >> 4));
    decoded[decodedLength++] = static_cast<uint8_t>(
      (static_cast<uint16_t>(v1) << 4) |
      (static_cast<uint16_t>(v2) >> 2));
    decoded[decodedLength++] = static_cast<uint8_t>(
      (static_cast<uint16_t>(v2) << 6) | static_cast<uint16_t>(v3));
  }
  return decodedLength > 0;
}

inline bool publicNameEquals(const uint8_t* actual, size_t actualLength,
                             const char* expected, size_t expectedLength) {
  if (actual == nullptr || expected == nullptr ||
      actualLength != expectedLength) {
    return false;
  }
  for (size_t i = 0; i < expectedLength; ++i) {
    if (actual[i] != static_cast<uint8_t>(expected[i])) return false;
  }
  return true;
}

// Candidate and expected secrets are always compared over the complete
// canonical encoding. Length mismatch is folded into the result rather than
// returning early.
inline bool constantTimeCredentialEquals(const uint8_t* candidate,
                                         size_t candidateLength,
                                         const char* expected,
                                         size_t expectedLength) {
  uint8_t candidateFixed[kCanonicalCredentialChars] = {};
  if (candidate != nullptr) {
    const size_t copyLength = candidateLength < kCanonicalCredentialChars
      ? candidateLength : kCanonicalCredentialChars;
    for (size_t i = 0; i < copyLength; ++i) candidateFixed[i] = candidate[i];
  }

  volatile uint32_t difference = static_cast<uint32_t>(
    candidateLength ^ expectedLength);
  difference |= static_cast<uint32_t>(
    expectedLength ^ kCanonicalCredentialChars);
  difference |= expected == nullptr ? 1u : 0u;
  for (size_t i = 0; i < kCanonicalCredentialChars; ++i) {
    const uint8_t expectedByte =
      (expected != nullptr && i < expectedLength)
        ? static_cast<uint8_t>(expected[i]) : 0;
    difference |= static_cast<uint32_t>(candidateFixed[i] ^ expectedByte);
  }

  secureClearBytes(candidateFixed, sizeof(candidateFixed));
  return difference == 0;
}

inline AccessRole authenticateBasic(const char* header, size_t headerLength,
                                    const BasicCredentials& credentials) {
  if (header == nullptr || headerLength > kMaxAuthorizationHeaderBytes ||
      !basicSchemeMatches(header, headerLength)) {
    return AccessRole::NONE;
  }

  uint8_t decoded[kMaxDecodedCredentialBytes] = {};
  size_t decodedLength = 0;
  if (!decodeBase64Strict(header + 6, headerLength - 6, decoded,
                          sizeof(decoded), decodedLength)) {
    return AccessRole::NONE;
  }

  size_t colon = decodedLength;
  bool malformed = false;
  for (size_t i = 0; i < decodedLength; ++i) {
    if (decoded[i] < 33 || decoded[i] > 126) malformed = true;
    if (decoded[i] == ':') {
      if (colon != decodedLength) malformed = true;
      colon = i;
    }
  }
  if (colon == 0 || colon >= decodedLength - 1) malformed = true;

  AccessRole role = AccessRole::NONE;
  if (!malformed) {
    const uint8_t* password = decoded + colon + 1;
    const size_t passwordLength = decodedLength - colon - 1;
    if (publicNameEquals(decoded, colon, "staff", 5) &&
        isCanonicalCredential(password, passwordLength) &&
        isCanonicalCredential(credentials.staffSecret,
                              credentials.staffSecretLength) &&
        constantTimeCredentialEquals(
          password, passwordLength, credentials.staffSecret,
          credentials.staffSecretLength)) {
      role = AccessRole::STAFF;
    } else if (publicNameEquals(decoded, colon, "admin", 5) &&
               isCanonicalCredential(password, passwordLength) &&
               isCanonicalCredential(credentials.adminSecret,
                                     credentials.adminSecretLength) &&
               constantTimeCredentialEquals(
                 password, passwordLength, credentials.adminSecret,
                 credentials.adminSecretLength)) {
      role = AccessRole::ADMIN;
    }
  }

  secureClearBytes(decoded, sizeof(decoded));
  return role;
}

inline bool isProductionModelAllowed(const char* model) {
  return cStringEquals(model, "OMRON-HBP9030");
}

constexpr bool credentialRotationRequiresRestart(
    DeviceSecretKind kind, RequestInterface requestInterface) {
  return kind == DeviceSecretKind::AP &&
         (requestInterface == RequestInterface::PROVISIONING_AP ||
          requestInterface == RequestInterface::RECOVERY_AP);
}

inline constexpr size_t kAuthFailureSourceSlots = 16;
inline constexpr uint16_t kPerSourceFailureLimit = 5;
inline constexpr uint16_t kGlobalFailureLimit = 32;
inline constexpr uint32_t kAuthFailureCooldownMs = 60000;

class AuthFailureLimiter {
public:
  AuthFailureLimiter()
    : slots_{}, globalWindowStart_(0), globalFailures_(0),
      globalActive_(false), nextEviction_(0) {}

  bool allowAttempt(uint32_t source, uint32_t nowMs) {
    expireGlobal(nowMs);
    if (globalActive_ && globalFailures_ >= kGlobalFailureLimit) return false;

    Slot* slot = findSlot(source);
    if (slot == nullptr) return true;
    expireSlot(*slot, nowMs);
    return !slot->occupied || slot->failures < kPerSourceFailureLimit;
  }

  void recordFailure(uint32_t source, uint32_t nowMs) {
    expireGlobal(nowMs);
    if (!globalActive_) {
      globalActive_ = true;
      globalWindowStart_ = nowMs;
    }
    if (globalFailures_ < kGlobalFailureLimit) ++globalFailures_;

    Slot* slot = findSlot(source);
    if (slot != nullptr) expireSlot(*slot, nowMs);
    if (slot == nullptr || !slot->occupied) slot = allocateSlot(source, nowMs);
    if (slot->failures < kPerSourceFailureLimit) ++slot->failures;
    slot->lastSeen = nowMs;
  }

  void recordSuccess(uint32_t source) {
    Slot* slot = findSlot(source);
    if (slot != nullptr) *slot = Slot{};
  }

  size_t trackedSourceCount() const {
    size_t count = 0;
    for (const Slot& slot : slots_) {
      if (slot.occupied) ++count;
    }
    return count;
  }

  uint16_t globalFailureCount() const { return globalFailures_; }

private:
  struct Slot {
    uint32_t source = 0;
    uint32_t windowStart = 0;
    uint32_t lastSeen = 0;
    uint16_t failures = 0;
    bool occupied = false;
  };

  static bool cooldownElapsed(uint32_t nowMs, uint32_t startMs) {
    return static_cast<uint32_t>(nowMs - startMs) >=
           kAuthFailureCooldownMs;
  }

  void expireGlobal(uint32_t nowMs) {
    if (globalActive_ && cooldownElapsed(nowMs, globalWindowStart_)) {
      globalWindowStart_ = nowMs;
      globalFailures_ = 0;
      globalActive_ = false;
    }
  }

  void expireSlot(Slot& slot, uint32_t nowMs) {
    if (slot.occupied && cooldownElapsed(nowMs, slot.windowStart)) {
      slot.failures = 0;
      slot.windowStart = nowMs;
      slot.lastSeen = nowMs;
    }
  }

  Slot* findSlot(uint32_t source) {
    for (Slot& slot : slots_) {
      if (slot.occupied && slot.source == source) return &slot;
    }
    return nullptr;
  }

  Slot* allocateSlot(uint32_t source, uint32_t nowMs) {
    for (Slot& slot : slots_) {
      if (!slot.occupied) {
        slot = Slot{source, nowMs, nowMs, 0, true};
        return &slot;
      }
    }
    Slot& slot = slots_[nextEviction_];
    nextEviction_ = (nextEviction_ + 1) % kAuthFailureSourceSlots;
    slot = Slot{source, nowMs, nowMs, 0, true};
    return &slot;
  }

  Slot slots_[kAuthFailureSourceSlots];
  uint32_t globalWindowStart_;
  uint16_t globalFailures_;
  bool globalActive_;
  size_t nextEviction_;
};

}  // namespace bp_web

#endif
