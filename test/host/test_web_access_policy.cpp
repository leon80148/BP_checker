// Host-side contract tests for the default-deny Web access policy.
//
// This suite intentionally exercises only pure policy. Socket parsing and
// WebServer integration are separate slices.

#include <Arduino.h>
#include "lib/WebAccessPolicy.h"
#include "test_support.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

using namespace bp_web;

using CrossModuleAuthorizeSignature = AccessDecision (*)(
  HttpMethod, const char*, ::DeviceClaimState, RequestInterface, AccessRole);
constexpr CrossModuleAuthorizeSignature kCrossModuleAuthorize = &authorizeRoute;

static std::string base64Encode(const std::string& input) {
  static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  for (size_t offset = 0; offset < input.size(); offset += 3) {
    const size_t remaining = input.size() - offset;
    const uint32_t a = static_cast<unsigned char>(input[offset]);
    const uint32_t b = remaining > 1
      ? static_cast<unsigned char>(input[offset + 1]) : 0;
    const uint32_t c = remaining > 2
      ? static_cast<unsigned char>(input[offset + 2]) : 0;
    const uint32_t word = (a << 16) | (b << 8) | c;
    output.push_back(alphabet[(word >> 18) & 0x3f]);
    output.push_back(alphabet[(word >> 12) & 0x3f]);
    output.push_back(remaining > 1 ? alphabet[(word >> 6) & 0x3f] : '=');
    output.push_back(remaining > 2 ? alphabet[word & 0x3f] : '=');
  }
  return output;
}

static std::string basicHeader(const std::string& username,
                               const std::string& password) {
  return std::string("Basic ") + base64Encode(username + ":" + password);
}

static std::string basicHeaderForRaw(const std::string& decoded) {
  return std::string("Basic ") + base64Encode(decoded);
}

static std::string percentEncodeEveryByte(const std::string& input) {
  static const char hex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(input.size() * 3);
  for (unsigned char value : input) {
    output.push_back('%');
    output.push_back(hex[value >> 4]);
    output.push_back(hex[value & 0x0f]);
  }
  return output;
}

static AccessRole authenticate(const std::string& header,
                               const BasicCredentials& credentials) {
  return authenticateBasic(header.data(), header.size(), credentials);
}

static void testCompileTimeRouteRegistry() {
  static_assert(kRoutePolicyCount == 18,
                "every supported GET/POST route must be classified");
  static_assert(routeTableIsValid(),
                "route registry must be unique and fail closed");

  struct ExpectedRoute {
    HttpMethod method;
    const char* path;
    AccessRole role;
    uint16_t bodyCap;
    bool mutation;
  };

  static const ExpectedRoute expected[] = {
    {HttpMethod::GET,  "/claim",              AccessRole::NONE,  0,   false},
    {HttpMethod::POST, "/claim",              AccessRole::NONE,  96,  true},
    {HttpMethod::GET,  "/",                   AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/data",               AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/history",            AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/export.csv",         AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/api/history",        AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/api/latest",         AccessRole::STAFF, 0,   false},
    {HttpMethod::GET,  "/config",             AccessRole::ADMIN, 0,   false},
    {HttpMethod::POST, "/configure",           AccessRole::ADMIN, 512, true},
    {HttpMethod::POST, "/clear_history",       AccessRole::ADMIN, 0,   true},
    {HttpMethod::GET,  "/bp_model",           AccessRole::ADMIN, 0,   false},
    {HttpMethod::POST, "/set_bp_model",       AccessRole::ADMIN, 64,  true},
    {HttpMethod::GET,  "/security",           AccessRole::ADMIN, 0,   false},
    {HttpMethod::POST, "/rotate_credentials", AccessRole::ADMIN, 64,  true},
    {HttpMethod::GET,  "/measurement_policy", AccessRole::ADMIN, 0,   false},
    {HttpMethod::POST, "/set_measurement_policy", AccessRole::ADMIN, 512, true},
    {HttpMethod::POST, "/reset",               AccessRole::ADMIN, 0,   true},
  };

  CHECK_EQ(sizeof(expected) / sizeof(expected[0]), kRoutePolicyCount,
           "expected registry size");
  for (const ExpectedRoute& item : expected) {
    const RoutePolicy* policy = findRoutePolicy(item.method, item.path);
    CHECK_TRUE(policy != nullptr, "declared route is registered");
    if (!policy) continue;
    CHECK_EQ(static_cast<int>(policy->requiredRole),
             static_cast<int>(item.role), "route role metadata");
    CHECK_EQ(policy->bodyCap, item.bodyCap, "route body cap metadata");
    CHECK_EQ(policy->mutation, item.mutation, "route mutation metadata");
    CHECK_TRUE(policy->noStore, "every product response is no-store");
  }

  CHECK_TRUE(findRoutePolicy(HttpMethod::GET, "/missing") == nullptr,
             "unknown GET defaults to deny");
  CHECK_TRUE(findRoutePolicy(HttpMethod::POST, "/missing") == nullptr,
             "unknown POST defaults to deny");
  CHECK_TRUE(findRoutePolicy(HttpMethod::OTHER, "/") == nullptr,
             "unknown method defaults to deny");
  CHECK_TRUE(findRoutePolicy(HttpMethod::POST, "/") == nullptr,
             "wrong method on known GET defaults to deny");
  CHECK_TRUE(findRoutePolicy(HttpMethod::GET, "/configure") == nullptr,
             "wrong method on known POST defaults to deny");
  CHECK_TRUE(findRoutePolicy(HttpMethod::GET, nullptr) == nullptr,
             "null path defaults to deny");

  // Browser form for a manual 32-byte SSID and 63-character WPA passphrase.
  // Percent-encoding every input byte is legal x-www-form-urlencoded and is
  // the maximum expansion the bounded HTTP layer must accept.
  const std::string worstCaseConfigureBody =
    "ssid=manual&manual_ssid=" +
    percentEncodeEveryByte(std::string(32, 'S')) +
    "&password=" + percentEncodeEveryByte(std::string(63, 'P'));
  CHECK_EQ(worstCaseConfigureBody.size(), 319UL,
           "maximum legal configure form has deterministic encoded length");
  CHECK_TRUE(worstCaseConfigureBody.size() > 256,
             "former 256-byte cap rejects a legal maximum form");
  const RoutePolicy* configure =
    findRoutePolicy(HttpMethod::POST, "/configure");
  CHECK_TRUE(configure != nullptr && configure->bodyCap >= 512,
             "configure route reserves at least 512 bytes");
  CHECK_TRUE(configure != nullptr &&
               worstCaseConfigureBody.size() <= configure->bodyCap,
             "maximum legal configure form fits route cap");
}

static void testRoleToSurfacePolicy() {
  static const WebSurface staffSurfaces[] = {
    WebSurface::MONITOR_NAV,
    WebSurface::HISTORY_NAV,
  };
  static const WebSurface adminSurfaces[] = {
    WebSurface::ADMIN_WIFI_NAV,
    WebSurface::ADMIN_MODEL_NAV,
    WebSurface::ADMIN_SECURITY_NAV,
    WebSurface::ADMIN_POLICY_NAV,
    WebSurface::RESET_CONTROL,
    WebSurface::CLEAR_HISTORY_CONTROL,
    WebSurface::POLICY_UPDATE_CONTROL,
  };
  for (WebSurface surface : staffSurfaces) {
    CHECK_TRUE(!surfaceVisible(AccessRole::NONE, surface),
               "anonymous sees no authenticated surface");
    CHECK_TRUE(surfaceVisible(AccessRole::STAFF, surface),
               "staff sees each staff navigation surface");
    CHECK_TRUE(surfaceVisible(AccessRole::ADMIN, surface),
               "admin inherits each staff navigation surface");
  }
  for (WebSurface surface : adminSurfaces) {
    CHECK_TRUE(!surfaceVisible(AccessRole::NONE, surface),
               "anonymous cannot discover admin surface");
    CHECK_TRUE(!surfaceVisible(AccessRole::STAFF, surface),
               "staff cannot discover any admin navigation or control");
    CHECK_TRUE(surfaceVisible(AccessRole::ADMIN, surface),
               "admin sees each admin navigation or control");
  }
  CHECK_TRUE(!surfaceVisible(static_cast<AccessRole>(0xff),
                             WebSurface::ADMIN_POLICY_NAV),
             "corrupt role fails closed for surface discovery");
  CHECK_TRUE(!surfaceVisible(AccessRole::ADMIN,
                             static_cast<WebSurface>(0xff)),
             "corrupt surface fails closed");
}

static void testClaimBoundary() {
  for (HttpMethod method : {HttpMethod::GET, HttpMethod::POST}) {
    CHECK_EQ(static_cast<int>(authorizeRoute(
               method, "/claim", ::DeviceClaimState::UNCLAIMED,
               RequestInterface::PROVISIONING_AP, AccessRole::NONE)),
             static_cast<int>(AccessDecision::ALLOW),
             "unclaimed claim is public only on provisioning AP");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               method, "/claim", ::DeviceClaimState::UNCLAIMED,
               RequestInterface::STA, AccessRole::ADMIN)),
             static_cast<int>(AccessDecision::DENY_INTERFACE),
             "claim denied on STA even with admin role");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               method, "/claim", ::DeviceClaimState::UNCLAIMED,
               RequestInterface::RECOVERY_AP, AccessRole::ADMIN)),
             static_cast<int>(AccessDecision::DENY_INTERFACE),
             "claim denied on recovery AP");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               method, "/claim", ::DeviceClaimState::CLAIMED,
               RequestInterface::PROVISIONING_AP, AccessRole::ADMIN)),
             static_cast<int>(AccessDecision::DENY_STATE),
             "claim is one-time after device is claimed");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               method, "/claim", ::DeviceClaimState::CLAIMED,
               RequestInterface::RECOVERY_AP, AccessRole::NONE)),
             static_cast<int>(AccessDecision::ALLOW),
             "claimed recovery route requires a physically opened AP");
  }

  CHECK_EQ(static_cast<int>(authorizeRoute(
             HttpMethod::GET, "/", ::DeviceClaimState::UNCLAIMED,
             RequestInterface::PROVISIONING_AP, AccessRole::ADMIN)),
           static_cast<int>(AccessDecision::DENY_STATE),
           "unclaimed device exposes no measurement route");
}

static void testDeviceSecurityClaimStateIsThePolicyState() {
  static_assert(static_cast<uint8_t>(::DeviceClaimState::UNCLAIMED) == 1,
                "device-security persisted unclaimed value is explicit");
  static_assert(static_cast<uint8_t>(::DeviceClaimState::CLAIMED) == 2,
                "device-security persisted claimed value is explicit");

  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/claim", ::DeviceClaimState::UNCLAIMED,
             RequestInterface::PROVISIONING_AP, AccessRole::NONE)),
           static_cast<int>(AccessDecision::ALLOW),
           "DeviceSecurity UNCLAIMED maps directly to unclaimed policy");
  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/", ::DeviceClaimState::CLAIMED,
             RequestInterface::STA, AccessRole::STAFF)),
           static_cast<int>(AccessDecision::ALLOW),
           "DeviceSecurity CLAIMED maps directly to claimed policy");

  const auto legacyPolicyZero = static_cast<::DeviceClaimState>(0);
  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/claim", legacyPolicyZero,
             RequestInterface::PROVISIONING_AP, AccessRole::NONE)),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "old policy raw zero cannot masquerade as device unclaimed");
  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/", legacyPolicyZero,
             RequestInterface::STA, AccessRole::STAFF)),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "old policy raw zero cannot access claimed routes");

  const auto corruptState = static_cast<::DeviceClaimState>(0xff);
  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/claim", corruptState,
             RequestInterface::PROVISIONING_AP, AccessRole::NONE)),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "corrupt device state fails closed on claim route");
  CHECK_EQ(static_cast<int>(kCrossModuleAuthorize(
             HttpMethod::GET, "/", corruptState,
             RequestInterface::STA, AccessRole::STAFF)),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "corrupt device state fails closed on claimed route");
}

static void testClaimedRoleMatrix() {
  static const char* staffReads[] = {
    "/", "/data", "/history", "/export.csv", "/api/history", "/api/latest"
  };
  for (const char* path : staffReads) {
    CHECK_EQ(static_cast<int>(authorizeRoute(
               HttpMethod::GET, path, ::DeviceClaimState::CLAIMED,
               RequestInterface::STA, AccessRole::NONE)),
             static_cast<int>(AccessDecision::DENY_ROLE),
             "anonymous claimed read denied");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               HttpMethod::GET, path, ::DeviceClaimState::CLAIMED,
               RequestInterface::STA, AccessRole::STAFF)),
             static_cast<int>(AccessDecision::ALLOW),
             "staff claimed read allowed");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               HttpMethod::GET, path, ::DeviceClaimState::CLAIMED,
               RequestInterface::STA, AccessRole::ADMIN)),
             static_cast<int>(AccessDecision::ALLOW),
             "admin inherits staff reads");
  }

  struct AdminRoute {
    HttpMethod method;
    const char* path;
  };
  static const AdminRoute adminRoutes[] = {
    {HttpMethod::GET, "/config"},
    {HttpMethod::POST, "/configure"},
    {HttpMethod::POST, "/clear_history"},
    {HttpMethod::GET, "/bp_model"},
    {HttpMethod::POST, "/set_bp_model"},
    {HttpMethod::GET, "/security"},
    {HttpMethod::POST, "/rotate_credentials"},
    {HttpMethod::GET, "/measurement_policy"},
    {HttpMethod::POST, "/set_measurement_policy"},
    {HttpMethod::POST, "/reset"},
  };
  for (const AdminRoute& route : adminRoutes) {
    CHECK_EQ(static_cast<int>(authorizeRoute(
               route.method, route.path, ::DeviceClaimState::CLAIMED,
               RequestInterface::STA, AccessRole::STAFF)),
             static_cast<int>(AccessDecision::DENY_ROLE),
             "staff cannot access admin route");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               route.method, route.path, ::DeviceClaimState::CLAIMED,
               RequestInterface::STA, AccessRole::ADMIN)),
             static_cast<int>(AccessDecision::ALLOW),
             "admin can access admin route");
  }

  CHECK_EQ(static_cast<int>(authorizeRoute(
             HttpMethod::GET, "/404", ::DeviceClaimState::CLAIMED,
             RequestInterface::STA, AccessRole::ADMIN)),
           static_cast<int>(AccessDecision::DENY_UNKNOWN_ROUTE),
           "404 path remains default deny");
  CHECK_EQ(static_cast<int>(authorizeRoute(
             HttpMethod::POST, "/", ::DeviceClaimState::CLAIMED,
             RequestInterface::STA, AccessRole::ADMIN)),
           static_cast<int>(AccessDecision::DENY_UNKNOWN_ROUTE),
           "wrong method remains default deny");
  CHECK_EQ(static_cast<int>(authorizeRoute(
             HttpMethod::GET, "/", ::DeviceClaimState::CLAIMED,
             static_cast<RequestInterface>(0xff), AccessRole::STAFF)),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "corrupt interface enum fails closed");
  CHECK_EQ(static_cast<int>(authorizeRoute(
             HttpMethod::GET, "/", ::DeviceClaimState::CLAIMED,
             RequestInterface::STA, static_cast<AccessRole>(0xff))),
           static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
           "corrupt role enum fails closed");
}

static void testInvalidEnumsFailClosedBeforeClaimSpecialCase() {
  struct Context {
    const char* label;
    HttpMethod method;
    const char* path;
    ::DeviceClaimState state;
    RequestInterface requestInterface;
    AccessRole role;
  };
  const Context validClaim = {
    "claim", HttpMethod::GET, "/claim", ::DeviceClaimState::UNCLAIMED,
    RequestInterface::PROVISIONING_AP, AccessRole::NONE,
  };
  const Context validRead = {
    "read", HttpMethod::GET, "/", ::DeviceClaimState::CLAIMED,
    RequestInterface::STA, AccessRole::STAFF,
  };

  for (const Context& context : {validClaim, validRead}) {
    (void)context.label;
    CHECK_EQ(static_cast<int>(authorizeRoute(
               HttpMethod::OTHER, context.path, context.state,
               context.requestInterface, context.role)),
             static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
             "OTHER method fails closed for claim and nonclaim");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               static_cast<HttpMethod>(0xff), context.path, context.state,
               context.requestInterface, context.role)),
             static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
             "corrupt method enum fails closed for claim and nonclaim");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               context.method, context.path,
               static_cast<::DeviceClaimState>(0xff),
               context.requestInterface, context.role)),
             static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
             "corrupt claim-state enum fails closed for both route classes");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               context.method, context.path, context.state,
               static_cast<RequestInterface>(0xff), context.role)),
             static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
             "corrupt interface enum fails closed for both route classes");
    CHECK_EQ(static_cast<int>(authorizeRoute(
               context.method, context.path, context.state,
               context.requestInterface, static_cast<AccessRole>(0xff))),
             static_cast<int>(AccessDecision::DENY_INVALID_CONTEXT),
             "corrupt role enum fails closed for both route classes");
  }
}

static void testBasicAuthentication() {
  // Canonical Base64URL (no padding) encodings of independent 16-byte
  // fixtures. Both URL-safe alphabet characters are intentional regression
  // probes: '+'/'/' are not valid credential characters here.
  static const char kStaffSecret[] = "-_8AAQIDBAUGBwgJCgsMDQ";
  static const char kAdminSecret[] = "-_4gISIjJCUmJygpKissLQ";
  static_assert(sizeof(kStaffSecret) - 1 == 22,
                "16-byte Base64URL credentials encode to 22 characters");
  static_assert(sizeof(kAdminSecret) - 1 == 22,
                "16-byte Base64URL credentials encode to 22 characters");
  static_assert(kCanonicalCredentialChars == 22,
                "access policy must use canonical 128-bit credentials");
  const BasicCredentials credentials = {
    kStaffSecret, sizeof(kStaffSecret) - 1,
    kAdminSecret, sizeof(kAdminSecret) - 1,
  };

  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("staff", kStaffSecret), credentials)),
           static_cast<int>(AccessRole::STAFF), "valid staff Basic credential");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", kAdminSecret), credentials)),
           static_cast<int>(AccessRole::ADMIN), "valid admin Basic credential");

  std::string mixedCase = basicHeader("admin", kAdminSecret);
  mixedCase.replace(0, 5, "bAsIc");
  CHECK_EQ(static_cast<int>(authenticate(mixedCase, credentials)),
           static_cast<int>(AccessRole::ADMIN),
           "Basic auth scheme is case-insensitive");

  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", "-_4gISIjJCUmJygpKissMQ"),
             credentials)), static_cast<int>(AccessRole::NONE),
           "wrong same-length admin secret denied");
  const std::string adminPrefix(kAdminSecret, 21);
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", adminPrefix),
             credentials)), static_cast<int>(AccessRole::NONE),
           "21-character admin secret denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", std::string(kAdminSecret) + "A"),
             credentials)), static_cast<int>(AccessRole::NONE),
           "23-character admin secret denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("operator", kAdminSecret), credentials)),
           static_cast<int>(AccessRole::NONE), "unknown Basic username denied");

  CHECK_EQ(static_cast<int>(authenticate("", credentials)),
           static_cast<int>(AccessRole::NONE), "empty Authorization denied");
  CHECK_EQ(static_cast<int>(authenticate("Digest abc", credentials)),
           static_cast<int>(AccessRole::NONE), "Digest is not accepted");
  CHECK_EQ(static_cast<int>(authenticate("Basic", credentials)),
           static_cast<int>(AccessRole::NONE), "Basic without payload denied");
  CHECK_EQ(static_cast<int>(authenticate(" Basic YQ==", credentials)),
           static_cast<int>(AccessRole::NONE), "leading whitespace denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic\tYQ==", credentials)),
           static_cast<int>(AccessRole::NONE), "tab separator denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic YQ== ", credentials)),
           static_cast<int>(AccessRole::NONE), "trailing whitespace denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic A", credentials)),
           static_cast<int>(AccessRole::NONE), "non-quad base64 denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic !!!!", credentials)),
           static_cast<int>(AccessRole::NONE), "invalid base64 alphabet denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic Y=Q=", credentials)),
           static_cast<int>(AccessRole::NONE), "interior base64 padding denied");
  CHECK_EQ(static_cast<int>(authenticate("Basic YR==", credentials)),
           static_cast<int>(AccessRole::NONE), "non-canonical base64 bits denied");

  CHECK_EQ(static_cast<int>(authenticate(
             basicHeaderForRaw("admin-no-colon"), credentials)),
           static_cast<int>(AccessRole::NONE), "missing colon denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeaderForRaw(":" + std::string(kAdminSecret)), credentials)),
           static_cast<int>(AccessRole::NONE), "empty username denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeaderForRaw("admin:"), credentials)),
           static_cast<int>(AccessRole::NONE), "empty password denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeaderForRaw("admin:" + std::string(kAdminSecret) + ":x"),
             credentials)), static_cast<int>(AccessRole::NONE),
           "second colon denied");
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeaderForRaw("admin:" + std::string(kAdminSecret) + "\n"),
             credentials)), static_cast<int>(AccessRole::NONE),
           "decoded control character denied");

  std::string withNul = "admin:" + std::string(kAdminSecret);
  withNul.push_back('\0');
  withNul.push_back('x');
  CHECK_EQ(static_cast<int>(authenticate(basicHeaderForRaw(withNul), credentials)),
           static_cast<int>(AccessRole::NONE), "decoded NUL denied");

  std::string overlong(kMaxAuthorizationHeaderBytes + 1, 'A');
  CHECK_EQ(static_cast<int>(authenticate(overlong, credentials)),
           static_cast<int>(AccessRole::NONE), "overlong Authorization denied");

  const BasicCredentials shortConfiguredSecret = {
    adminPrefix.c_str(), adminPrefix.size(),
    kAdminSecret, sizeof(kAdminSecret) - 1,
  };
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("staff", adminPrefix), shortConfiguredSecret)),
           static_cast<int>(AccessRole::NONE),
           "21-character configured secret fails closed");
  const std::string adminSuffix = std::string(kAdminSecret) + "A";
  const BasicCredentials longConfiguredSecret = {
    kStaffSecret, sizeof(kStaffSecret) - 1,
    adminSuffix.c_str(), adminSuffix.size(),
  };
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", adminSuffix), longConfiguredSecret)),
           static_cast<int>(AccessRole::NONE),
           "23-character configured secret fails closed");

  std::string nonCanonical = kAdminSecret;
  nonCanonical.back() = 'R';  // low four unused bits must be zero
  const BasicCredentials nonCanonicalConfigured = {
    kStaffSecret, sizeof(kStaffSecret) - 1,
    nonCanonical.c_str(), nonCanonical.size(),
  };
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", nonCanonical), nonCanonicalConfigured)),
           static_cast<int>(AccessRole::NONE),
           "non-canonical 22-character Base64URL secret fails closed");

  std::string wrongAlphabet = kAdminSecret;
  wrongAlphabet[0] = '+';
  const BasicCredentials wrongAlphabetConfigured = {
    kStaffSecret, sizeof(kStaffSecret) - 1,
    wrongAlphabet.c_str(), wrongAlphabet.size(),
  };
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", wrongAlphabet), wrongAlphabetConfigured)),
           static_cast<int>(AccessRole::NONE),
           "non-URL-safe credential alphabet fails closed");
  const BasicCredentials emptyConfiguredSecrets = {nullptr, 0, nullptr, 0};
  CHECK_EQ(static_cast<int>(authenticate(
             basicHeader("admin", kAdminSecret), emptyConfiguredSecrets)),
           static_cast<int>(AccessRole::NONE),
           "uninitialized configured credentials fail closed");
}

static void testStrictDecoderClearsPartialCredentialOnFailure() {
  uint8_t decoded[16];
  for (uint8_t& value : decoded) value = 0xa5;
  size_t decodedLength = 99;

  // The first quartet decodes to "adm"; the second is malformed. A failure
  // must not leave that credential prefix in caller-visible memory.
  CHECK_TRUE(!decodeBase64Strict("YWRt!!!!", 8, decoded, sizeof(decoded),
                                 decodedLength),
             "malformed later quartet is rejected");
  CHECK_EQ(decodedLength, 0UL, "failed decode reports no output bytes");
  for (uint8_t value : decoded) {
    CHECK_EQ(value, static_cast<uint8_t>(0),
             "failed decode clears complete output capacity");
  }
}

static void testProductionModelAllowlist() {
  CHECK_TRUE(isProductionModelAllowed("OMRON-HBP9030"),
             "HBP-9030 is the sole production model");
  CHECK_TRUE(!isProductionModelAllowed(nullptr), "null model denied");
  CHECK_TRUE(!isProductionModelAllowed(""), "empty model denied");
  CHECK_TRUE(!isProductionModelAllowed("CUSTOM"), "CUSTOM denied in production");
  CHECK_TRUE(!isProductionModelAllowed("omron-hbp9030"),
             "model allowlist is exact");
  CHECK_TRUE(!isProductionModelAllowed("OMRON-HBP9030 "),
             "model suffix denied");
}

static void testCredentialRotationRuntimeBoundary() {
  CHECK_TRUE(credentialRotationRequiresRestart(
               DeviceSecretKind::AP, RequestInterface::PROVISIONING_AP),
             "AP secret rotation restarts an active provisioning AP");
  CHECK_TRUE(credentialRotationRequiresRestart(
               DeviceSecretKind::AP, RequestInterface::RECOVERY_AP),
             "AP secret rotation restarts an active recovery AP");
  CHECK_TRUE(!credentialRotationRequiresRestart(
               DeviceSecretKind::AP, RequestInterface::STA),
             "AP secret rotation from STA needs no immediate restart");
  CHECK_TRUE(!credentialRotationRequiresRestart(
               DeviceSecretKind::ADMIN, RequestInterface::PROVISIONING_AP),
             "admin rotation does not restart the device");
  CHECK_TRUE(!credentialRotationRequiresRestart(
               DeviceSecretKind::STAFF, RequestInterface::RECOVERY_AP),
             "staff rotation does not restart the device");
  CHECK_TRUE(!credentialRotationRequiresRestart(
               DeviceSecretKind::BOOTSTRAP, RequestInterface::STA),
             "bootstrap rotation does not restart the device");
}

static void testPerSourceFailureLimitAndCooldown() {
  AuthFailureLimiter limiter;
  const uint32_t source = 0x0a000001u;
  const uint32_t start = 1000;
  for (uint16_t i = 0; i < kPerSourceFailureLimit; ++i) {
    CHECK_TRUE(limiter.allowAttempt(source, start),
               "source allowed before failure threshold");
    limiter.recordFailure(source, start);
  }
  CHECK_TRUE(!limiter.allowAttempt(source, start),
             "source blocked at failure threshold");
  CHECK_TRUE(!limiter.allowAttempt(source, start + kAuthFailureCooldownMs - 1),
             "source remains blocked before cooldown");
  CHECK_TRUE(limiter.allowAttempt(source, start + kAuthFailureCooldownMs),
             "source resets exactly at cooldown");

  for (uint16_t i = 0; i < kPerSourceFailureLimit; ++i) {
    limiter.recordFailure(source, start + kAuthFailureCooldownMs + 1);
  }
  CHECK_TRUE(!limiter.allowAttempt(source,
                                   start + kAuthFailureCooldownMs + 1),
             "source blocks again in new window");
  limiter.recordSuccess(source);
  CHECK_TRUE(limiter.allowAttempt(source,
                                  start + kAuthFailureCooldownMs + 1),
             "successful authentication clears source failures");
}

static void testGlobalGuardAndFixedStorage() {
  AuthFailureLimiter limiter;
  __delayCallCount() = 0;
  for (uint32_t i = 0; i < 100; ++i) {
    limiter.recordFailure(0x0a000100u + i, 5000 + i);
  }
  CHECK_TRUE(limiter.trackedSourceCount() <= kAuthFailureSourceSlots,
             "100 sources cannot grow fixed source storage");
  CHECK_TRUE(sizeof(limiter) <= 512,
             "failure limiter has a small fixed memory bound");
  CHECK_EQ(__delayCallCount(), 0UL, "failure limiter never delays");
  CHECK_EQ(limiter.globalFailureCount(), kGlobalFailureLimit,
           "global failure count saturates at its bound");
  CHECK_TRUE(!limiter.allowAttempt(0x7f000001u, 5100),
             "global guard blocks source rotation attack");
  CHECK_TRUE(limiter.allowAttempt(
               0x7f000001u, 5000 + kAuthFailureCooldownMs),
             "global guard resets after cooldown");
}

static void testUnsignedClockWrap() {
  AuthFailureLimiter limiter;
  const uint32_t source = 0xc0a80101u;
  const uint32_t start = std::numeric_limits<uint32_t>::max() -
                         (kAuthFailureCooldownMs / 2);
  for (uint16_t i = 0; i < kPerSourceFailureLimit; ++i) {
    limiter.recordFailure(source, start);
  }
  CHECK_TRUE(!limiter.allowAttempt(
               source, start + kAuthFailureCooldownMs - 1),
             "wrapped clock remains blocked before cooldown");
  CHECK_TRUE(limiter.allowAttempt(source,
                                  start + kAuthFailureCooldownMs),
             "wrapped clock resets at cooldown");
}

int main() {
  testCompileTimeRouteRegistry();
  testRoleToSurfacePolicy();
  testClaimBoundary();
  testDeviceSecurityClaimStateIsThePolicyState();
  testClaimedRoleMatrix();
  testInvalidEnumsFailClosedBeforeClaimSpecialCase();
  testBasicAuthentication();
  testStrictDecoderClearsPartialCredentialOnFailure();
  testProductionModelAllowlist();
  testCredentialRotationRuntimeBoundary();
  testPerSourceFailureLimitAndCooldown();
  testGlobalGuardAndFixedStorage();
  testUnsignedClockWrap();
  return testReport();
}
