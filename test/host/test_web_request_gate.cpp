#include <Arduino.h>

#include "lib/WebRequestGate.h"
#include "test_support.h"

#include <cstring>
#include <cstdlib>
#include <new>
#include <string>
#include <type_traits>

using namespace bp_web;

static bool gDenyAllocations = false;
static size_t gDeniedAllocationCalls = 0;

void* operator new(std::size_t size) {
  if (gDenyAllocations) {
    ++gDeniedAllocationCalls;
    throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

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
    const uint32_t word = (a << 16U) | (b << 8U) | c;
    output.push_back(alphabet[(word >> 18U) & 0x3fU]);
    output.push_back(alphabet[(word >> 12U) & 0x3fU]);
    output.push_back(remaining > 1 ? alphabet[(word >> 6U) & 0x3fU] : '=');
    output.push_back(remaining > 2 ? alphabet[word & 0x3fU] : '=');
  }
  return output;
}

static std::string basicHeader(const char* user, const char* secret) {
  return "Basic " + base64Encode(std::string(user) + ":" + secret);
}

class CompleteRequest {
public:
  CompleteRequest(bp_http::RequestMethod method, const char* path,
                  const char* host, const char* authorization,
                  const char* origin) {
    const char* methodText = method == bp_http::RequestMethod::GET
      ? "GET" : method == bp_http::RequestMethod::POST ? "POST" : "PATCH";
    std::string wire = std::string(methodText) + " " + path +
      " HTTP/1.1\r\nHost: " + host + "\r\n";
    if (authorization != nullptr) {
      wire += "Authorization: ";
      wire += authorization;
      wire += "\r\n";
    }
    if (origin != nullptr) {
      wire += "Origin: ";
      wire += origin;
      wire += "\r\n";
    }
    wire += "\r\n";
    _request.reset(1);
    size_t offset = 0;
    while (offset < wire.size() &&
           _request.state() != bp_http::RequestState::WAIT_POLICY &&
           _request.state() != bp_http::RequestState::REJECT) {
      const bp_http::ConsumeResult part = _request.consume(
        reinterpret_cast<const uint8_t*>(wire.data() + offset),
        wire.size() - offset, 1);
      offset += part.consumed;
    }
  }

  CompleteRequest(const CompleteRequest&) = delete;
  CompleteRequest& operator=(const CompleteRequest&) = delete;

  operator const bp_http::BoundedHttpRequest&() const { return _request; }
  const bp_http::RequestView& view() const { return _request.view(); }

private:
  bp_http::BoundedHttpRequest _request;
};

static CompleteRequest makeRequest(
    bp_http::RequestMethod method, const char* path, const char* host,
    const char* authorization = nullptr, const char* origin = nullptr) {
  return CompleteRequest(method, path, host, authorization, origin);
}

static InterfaceSnapshot staNetwork() {
  InterfaceSnapshot network{};
  network.acceptedLocalAddress = 0x0a000005U;
  network.staAddress = network.acceptedLocalAddress;
  network.staActive = true;
  network.staHost = "10.0.0.5";
  network.mdnsHost = "bp_checker.local";
  network.apHost = "192.168.4.1";
  return network;
}

static InterfaceSnapshot provisioningNetwork() {
  InterfaceSnapshot network{};
  network.acceptedLocalAddress = 0xc0a80401U;
  network.apAddress = network.acceptedLocalAddress;
  network.apActive = true;
  network.apPurpose = ApPurpose::PROVISIONING;
  network.apHost = "192.168.4.1";
  network.staHost = "10.0.0.5";
  network.mdnsHost = "bp_checker.local";
  return network;
}

static SecurityGateSnapshot claimedSecurity() {
  static const char staff[] = "-_8AAQIDBAUGBwgJCgsMDQ";
  static const char admin[] = "-_4gISIjJCUmJygpKissLQ";
  SecurityGateSnapshot security{};
  security.availability = DeviceSecurityAvailability::READY;
  security.claimState = DeviceClaimState::CLAIMED;
  security.credentials = {staff, sizeof(staff) - 1,
                          admin, sizeof(admin) - 1};
  return security;
}

static void testInterfaceClassificationFailsClosed() {
  InterfaceSnapshot network{};
  network.acceptedLocalAddress = 0x0a000001U;
  network.apAddress = 0x0a000001U;
  network.apActive = true;
  network.apPurpose = ApPurpose::PROVISIONING;
  CHECK_EQ(static_cast<int>(classifyRequestInterface(network)),
           static_cast<int>(RequestInterface::PROVISIONING_AP),
           "active provisioning AP classified from accepted local endpoint");

  network.apPurpose = ApPurpose::RECOVERY;
  CHECK_EQ(static_cast<int>(classifyRequestInterface(network)),
           static_cast<int>(RequestInterface::RECOVERY_AP),
           "active recovery AP classified from accepted local endpoint");

  network.apActive = false;
  network.staActive = true;
  network.staAddress = network.acceptedLocalAddress;
  CHECK_EQ(static_cast<int>(classifyRequestInterface(network)),
           static_cast<int>(RequestInterface::STA),
           "active STA classified from accepted local endpoint");

  network.apActive = true;
  network.apAddress = network.acceptedLocalAddress;
  CHECK_EQ(static_cast<int>(classifyRequestInterface(network)),
           static_cast<int>(RequestInterface::UNKNOWN),
           "ambiguous AP and STA endpoint fails closed");
}

static void testHostMatchesOnlyAcceptedInterfaceIdentity() {
  InterfaceSnapshot network{};
  network.apHost = "192.168.4.1";
  network.staHost = "10.0.0.5";
  network.mdnsHost = "bp_checker.local";

  CHECK_TRUE(hostAllowedForInterface(
               "192.168.4.1", RequestInterface::PROVISIONING_AP, network),
             "provisioning AP accepts its own IP Host");
  CHECK_TRUE(hostAllowedForInterface(
               "192.168.4.1:80", RequestInterface::RECOVERY_AP, network),
             "recovery AP accepts explicit default port");
  CHECK_TRUE(!hostAllowedForInterface(
               "10.0.0.5", RequestInterface::PROVISIONING_AP, network),
             "AP ingress rejects STA Host identity");

  CHECK_TRUE(hostAllowedForInterface(
               "10.0.0.5", RequestInterface::STA, network),
             "STA accepts its own IP Host");
  CHECK_TRUE(hostAllowedForInterface(
               "BP_CHECKER.Local:80", RequestInterface::STA, network),
             "STA accepts case-insensitive mDNS Host and default port");
  CHECK_TRUE(!hostAllowedForInterface(
               "bp_checker.local:81", RequestInterface::STA, network),
             "non-listening port rejected");
  CHECK_TRUE(!hostAllowedForInterface(
               "attacker.local", RequestInterface::STA, network),
             "foreign DNS-rebinding Host rejected");
  CHECK_TRUE(!hostAllowedForInterface(
               "", RequestInterface::STA, network),
             "empty Host rejected");
  CHECK_TRUE(!hostAllowedForInterface(
               nullptr, RequestInterface::STA, network),
             "null Host rejected");
  CHECK_TRUE(!hostAllowedForInterface(
               "10.0.0.5,attacker", RequestInterface::STA, network),
             "comma-joined Host rejected");
  CHECK_TRUE(!hostAllowedForInterface(
               "10.0.0.5", RequestInterface::UNKNOWN, network),
             "unknown ingress interface rejects every Host");
}

static void testMutationCsrfUsesFreshHost() {
  bp_http::RequestView view{};
  std::strcpy(view.host, "bp.local:80");
  CHECK_TRUE(csrfAllowedForMutation(view),
             "non-browser client may omit Origin and Referer");

  std::strcpy(view.origin, "http://BP.Local");
  CHECK_TRUE(csrfAllowedForMutation(view),
             "same-origin Origin accepts normalized default port");
  std::strcpy(view.origin, "http://attacker.local");
  CHECK_TRUE(!csrfAllowedForMutation(view), "foreign Origin rejected");
  std::strcpy(view.origin, "null");
  CHECK_TRUE(!csrfAllowedForMutation(view), "opaque Origin rejected");
  std::strcpy(view.origin, "https://bp.local");
  CHECK_TRUE(!csrfAllowedForMutation(view), "wrong scheme rejected");
  std::strcpy(view.origin, "http://bp.local/path");
  CHECK_TRUE(!csrfAllowedForMutation(view), "Origin with path rejected");

  view.origin[0] = '\0';
  std::strcpy(view.referer, "http://BP.Local:80/config?x=1");
  CHECK_TRUE(csrfAllowedForMutation(view),
             "same-origin Referer path accepted");
  std::strcpy(view.referer, "http://bp.local.attacker/config");
  CHECK_TRUE(!csrfAllowedForMutation(view),
             "host-prefix Referer attack rejected");
  std::strcpy(view.referer, "http://attacker@bp.local/config");
  CHECK_TRUE(!csrfAllowedForMutation(view),
             "userinfo Referer rejected");

  std::strcpy(view.origin, "http://attacker.local");
  std::strcpy(view.referer, "http://bp.local/config");
  CHECK_TRUE(!csrfAllowedForMutation(view),
             "present Origin takes precedence over Referer");
}

static void testClaimAndRoleGate() {
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  SecurityGateSnapshot unclaimed = claimedSecurity();
  unclaimed.claimState = DeviceClaimState::UNCLAIMED;
  InterfaceSnapshot ap = provisioningNetwork();

  GateResult result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/claim", "192.168.4.1"),
    unclaimed, ap, 0xc0a80402U, 100);
  CHECK_TRUE(result.allowed, "unclaimed provisioning GET claim allowed");
  CHECK_EQ(static_cast<int>(result.bodyMode),
           static_cast<int>(bp_http::BodyMode::NONE),
           "GET claim is bodyless");
  CHECK_TRUE(result.route != nullptr && result.route->noStore,
             "claim response is no-store");

  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/claim", "192.168.4.1",
                nullptr, "http://192.168.4.1"),
    unclaimed, ap, 0xc0a80402U, 101);
  CHECK_TRUE(result.allowed, "same-origin provisioning POST claim allowed");
  CHECK_EQ(static_cast<int>(result.bodyMode),
           static_cast<int>(bp_http::BodyMode::SMALL_FORM),
           "POST claim uses bounded form body");
  CHECK_EQ(result.bodyCap, 96U, "claim route body cap propagated");

  const SecurityGateSnapshot claimed = claimedSecurity();
  const InterfaceSnapshot sta = staNetwork();
  const std::string staff = basicHeader("staff", claimed.credentials.staffSecret);
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "bp_checker.local",
                staff.c_str()),
    claimed, sta, 0x0a000006U, 200);
  CHECK_TRUE(result.allowed, "staff credential may read dashboard");
  CHECK_EQ(static_cast<int>(result.role), static_cast<int>(AccessRole::STAFF),
           "staff role returned to dispatcher");

  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/config", "10.0.0.5",
                staff.c_str()),
    claimed, sta, 0x0a000006U, 201);
  CHECK_TRUE(!result.allowed, "staff cannot access admin configuration");
  CHECK_EQ(result.status, 403, "valid lower role receives forbidden");

  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/config", "10.0.0.5",
                admin.c_str()),
    claimed, sta, 0x0a000006U, 202);
  CHECK_TRUE(result.allowed, "admin credential may access configuration");
  CHECK_EQ(static_cast<int>(result.role), static_cast<int>(AccessRole::ADMIN),
           "admin role returned to dispatcher");
}

static void testDefaultDenyStateAndHostOrdering() {
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  const SecurityGateSnapshot claimed = claimedSecurity();
  SecurityGateSnapshot unclaimed = claimed;
  unclaimed.claimState = DeviceClaimState::UNCLAIMED;
  const InterfaceSnapshot sta = staNetwork();
  const InterfaceSnapshot ap = provisioningNetwork();
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);

  GateResult result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/missing", "10.0.0.5",
                admin.c_str()),
    claimed, sta, 0x0a000006U, 300);
  CHECK_TRUE(!result.allowed && result.status == 404,
             "unknown route defaults to 404 deny");
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/", "10.0.0.5",
                admin.c_str()),
    claimed, sta, 0x0a000006U, 301);
  CHECK_TRUE(!result.allowed && result.status == 405,
             "known path with wrong method defaults to 405 deny");
  result = gate.evaluate(
    makeRequest(static_cast<bp_http::RequestMethod>(0xff), "/claim",
                "192.168.4.1"),
    unclaimed, ap, 0xc0a80402U, 302);
  CHECK_TRUE(!result.allowed,
             "corrupt parser method cannot enter public claim route");

  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/claim", "192.168.4.1"),
    claimed, ap, 0xc0a80402U, 303);
  CHECK_TRUE(!result.allowed && result.status == 404,
             "claimed device cannot reuse claim route");
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/config", "192.168.4.1",
                admin.c_str()),
    claimed, ap, 0xc0a80402U, 303);
  CHECK_TRUE(result.allowed && result.role == AccessRole::ADMIN,
             "freshly claimed owner can finish WiFi onboarding on current AP");
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "192.168.4.1"),
    unclaimed, ap, 0xc0a80402U, 304);
  CHECK_TRUE(!result.allowed && result.status == 404,
             "unclaimed device exposes no measurement route");

  InterfaceSnapshot recovery = ap;
  recovery.apPurpose = ApPurpose::RECOVERY;
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/claim", "192.168.4.1"),
    unclaimed, recovery, 0xc0a80402U, 305);
  CHECK_TRUE(!result.allowed && result.status == 404,
             "recovery AP cannot perform first claim");
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/claim", "192.168.4.1",
                nullptr, "http://192.168.4.1"),
    claimed, recovery, 0xc0a80402U, 305);
  CHECK_TRUE(result.allowed && result.role == AccessRole::NONE,
             "physically opened recovery AP can verify armed token");
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/claim", "10.0.0.5"),
    unclaimed, sta, 0x0a000006U, 306);
  CHECK_TRUE(!result.allowed && result.status == 404,
             "STA cannot perform first claim");

  SecurityGateSnapshot unavailable = claimed;
  unavailable.availability = DeviceSecurityAvailability::REBOOT_REQUIRED;
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                admin.c_str()),
    unavailable, sta, 0x0a000006U, 307);
  CHECK_TRUE(!result.allowed && result.status == 503,
             "unavailable security state dispatches no handler");

  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "attacker.local",
                "Basic wrong"),
    claimed, sta, 0x0a000006U, 308);
  CHECK_TRUE(!result.allowed && result.status == 403,
             "foreign Host rejected before authentication");
  CHECK_EQ(limiter.globalFailureCount(), 0,
           "foreign Host does not consume authentication limiter");

  SecurityGateSnapshot badCredentials = claimed;
  badCredentials.credentials.adminSecretLength = 0;
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                admin.c_str()),
    badCredentials, sta, 0x0a000006U, 309);
  CHECK_TRUE(!result.allowed && result.status == 503,
             "invalid committed credential snapshot fails closed");
  CHECK_EQ(limiter.globalFailureCount(), 0,
           "invalid credential state does not charge client limiter");
}

static void testAuthenticationRateAndCsrfComposition() {
  const SecurityGateSnapshot claimed = claimedSecurity();
  const InterfaceSnapshot sta = staNetwork();
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);
  const std::string staff = basicHeader("staff", claimed.credentials.staffSecret);

  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  __delayCallCount() = 0;
  for (uint16_t i = 0; i < kPerSourceFailureLimit; ++i) {
    const GateResult failure = gate.evaluate(
      makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                  "Basic invalid"),
      claimed, sta, 0x0a000010U, 400);
    CHECK_TRUE(!failure.allowed && failure.status == 401,
               "wrong Basic credential receives immediate 401");
  }
  GateResult result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                admin.c_str()),
    claimed, sta, 0x0a000010U, 400);
  CHECK_TRUE(!result.allowed && result.status == 429,
             "per-source failure threshold rate-limits before auth");
  CHECK_EQ(__delayCallCount(), 0UL,
           "auth failure and rate limiting never block loop");

  AuthFailureLimiter resetLimiter;
  WebRequestGate resetGate(&resetLimiter);
  for (int i = 0; i < 4; ++i) {
    (void)resetGate.evaluate(
      makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                  "Basic invalid"),
      claimed, sta, 0x0a000011U, 401);
  }
  result = resetGate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                staff.c_str()),
    claimed, sta, 0x0a000011U, 401);
  CHECK_TRUE(result.allowed, "successful auth resets per-source failures");
  result = resetGate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/", "10.0.0.5",
                "Basic invalid"),
    claimed, sta, 0x0a000011U, 401);
  CHECK_TRUE(!result.allowed && result.status == 401,
             "one failure after success is not still rate-limited");

  AuthFailureLimiter roleLimiter;
  WebRequestGate roleGate(&roleLimiter);
  for (int i = 0; i < 8; ++i) {
    result = roleGate.evaluate(
      makeRequest(bp_http::RequestMethod::GET, "/config", "10.0.0.5",
                  staff.c_str()),
      claimed, sta, 0x0a000012U, 402);
    CHECK_TRUE(!result.allowed && result.status == 403,
               "valid staff on admin route remains role denial");
  }
  CHECK_EQ(roleLimiter.globalFailureCount(), 0,
           "valid lower role is not counted as auth failure");

  result = roleGate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/clear_history", "10.0.0.5",
                admin.c_str(), "http://10.0.0.5"),
    claimed, sta, 0x0a000012U, 403);
  CHECK_TRUE(result.allowed, "admin same-origin mutation allowed");
  CHECK_EQ(static_cast<int>(result.bodyMode),
           static_cast<int>(bp_http::BodyMode::NONE),
           "clear-history mutation declares no body");

  result = roleGate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/configure", "10.0.0.5",
                admin.c_str(), "http://attacker.local"),
    claimed, sta, 0x0a000012U, 404);
  CHECK_TRUE(!result.allowed && result.status == 403,
             "authenticated foreign-origin mutation denied");
  result = roleGate.evaluate(
    makeRequest(bp_http::RequestMethod::POST, "/configure", "10.0.0.5",
                admin.c_str()),
    claimed, sta, 0x0a000012U, 405);
  CHECK_TRUE(result.allowed,
             "authenticated CLI mutation may omit browser CSRF headers");
  CHECK_EQ(static_cast<int>(result.bodyMode),
           static_cast<int>(bp_http::BodyMode::SMALL_FORM),
           "configure uses bounded small-form mode");
  CHECK_EQ(result.bodyCap, 512U, "configure body cap propagated exactly");
}

static void testEveryRegistryRoutePassesOneCentralGate() {
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  const SecurityGateSnapshot claimed = claimedSecurity();
  SecurityGateSnapshot unclaimed = claimed;
  unclaimed.claimState = DeviceClaimState::UNCLAIMED;
  const InterfaceSnapshot sta = staNetwork();
  const InterfaceSnapshot ap = provisioningNetwork();
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);

  for (size_t i = 0; i < kRoutePolicyCount; ++i) {
    const RoutePolicy& route = kRoutePolicies[i];
    const bool claim = isClaimRoute(route);
    const bp_http::RequestMethod method = route.method == HttpMethod::GET
      ? bp_http::RequestMethod::GET : bp_http::RequestMethod::POST;
    const char* origin = route.mutation
      ? (claim ? "http://192.168.4.1" : "http://10.0.0.5") : nullptr;
    const CompleteRequest request = makeRequest(
      method, route.path, claim ? "192.168.4.1" : "10.0.0.5",
      claim ? nullptr : admin.c_str(), origin);
    const GateResult result = gate.evaluate(
      request, claim ? unclaimed : claimed, claim ? ap : sta,
      claim ? 0xc0a80402U : 0x0a000020U,
      static_cast<uint32_t>(500 + i));
    CHECK_TRUE(result.allowed, "every classified product route can pass gate");
    CHECK_TRUE(result.route == &route,
               "gate returns exact constexpr route metadata");
    CHECK_TRUE(result.route != nullptr && result.route->noStore,
               "every allowed route remains no-store");
    CHECK_EQ(result.bodyCap, route.bodyCap,
             "gate propagates exact route body cap");
    CHECK_EQ(static_cast<int>(result.bodyMode),
             static_cast<int>(route.bodyCap == 0
               ? bp_http::BodyMode::NONE : bp_http::BodyMode::SMALL_FORM),
             "gate derives bounded body mode from route metadata");
  }
}

static void testGateIsAllocationFreeAndFuzzFailsClosed() {
  static_assert(sizeof(WebRequestGate) <= 32,
                "central gate owns only bounded policy references");
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  const SecurityGateSnapshot claimed = claimedSecurity();
  const InterfaceSnapshot sta = staNetwork();
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);
  const CompleteRequest valid = makeRequest(
    bp_http::RequestMethod::GET, "/", "10.0.0.5", admin.c_str());
  bool threw = false;
  GateResult allocationResult{};
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    allocationResult = gate.evaluate(valid, claimed, sta, 0x0a000030U, 600);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "central gate succeeds with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "central gate performs no allocation attempt");
  CHECK_TRUE(allocationResult.allowed,
             "allocation-free valid request remains allowed");

  uint32_t random = 0x61c88647U;
  const char* paths[] = {"/", "/claim", "/configure", "/missing"};
  const char* hosts[] = {"10.0.0.5", "bp_checker.local", "attacker.local", ""};
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    auto nextRandom = [&random]() {
      random ^= random << 13U;
      random ^= random >> 17U;
      random ^= random << 5U;
      return random;
    };
    CompleteRequest request = makeRequest(
      static_cast<bp_http::RequestMethod>(nextRandom() & 0xffU),
      paths[nextRandom() % 4], hosts[nextRandom() % 4],
      (nextRandom() & 1U) != 0 ? admin.c_str() : "Basic invalid",
      (nextRandom() & 1U) != 0 ? "http://10.0.0.5" : nullptr);
    SecurityGateSnapshot security = claimed;
    security.availability = static_cast<DeviceSecurityAvailability>(
      nextRandom() % 6U);
    security.claimState = static_cast<DeviceClaimState>(nextRandom() & 0xffU);
    InterfaceSnapshot network = sta;
    network.acceptedLocalAddress = nextRandom();
    network.apPurpose = static_cast<ApPurpose>(nextRandom() & 0xffU);
    const GateResult result = gate.evaluate(
      request, security, network, nextRandom(), nextRandom());
    CHECK_TRUE(result.status == 0 || result.status == 400 ||
                 result.status == 401 || result.status == 403 ||
                 result.status == 404 || result.status == 405 ||
                 result.status == 429 || result.status == 503,
               "fuzzed gate returns a declared HTTP outcome");
    CHECK_TRUE(!result.allowed ||
                 (result.status == 0 && result.route != nullptr &&
                  result.route->noStore),
               "fuzzed allow always has classified no-store route");
  }
}

static void testClaimTokenOutcomesShareBoundedLimiter() {
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  SecurityGateSnapshot unclaimed = claimedSecurity();
  unclaimed.claimState = DeviceClaimState::UNCLAIMED;
  const InterfaceSnapshot ap = provisioningNetwork();
  const CompleteRequest post = makeRequest(
    bp_http::RequestMethod::POST, "/claim", "192.168.4.1", nullptr,
    "http://192.168.4.1");
  const CompleteRequest get = makeRequest(
    bp_http::RequestMethod::GET, "/claim", "192.168.4.1");
  const uint32_t source = 0xc0a80444U;
  __delayCallCount() = 0;

  for (uint16_t i = 0; i < kPerSourceFailureLimit; ++i) {
    const GateResult beforeBody = gate.evaluate(
      post, unclaimed, ap, source, 700);
    CHECK_TRUE(beforeBody.allowed,
               "claim POST allowed before token failure threshold");
    CHECK_TRUE(gate.recordClaimResult(source, false, 700),
               "wrong bootstrap token records bounded failure");
  }
  GateResult result = gate.evaluate(post, unclaimed, ap, source, 700);
  CHECK_TRUE(!result.allowed && result.status == 429,
             "claim POST rate-limited before reading another token body");
  result = gate.evaluate(get, unclaimed, ap, source, 700);
  CHECK_TRUE(result.allowed, "claim GET form remains readable while POST blocked");
  result = gate.evaluate(
    post, unclaimed, ap, source, 700 + kAuthFailureCooldownMs);
  CHECK_TRUE(result.allowed, "claim POST limiter resets after cooldown");
  CHECK_EQ(__delayCallCount(), 0UL, "claim token failures never delay loop");

  for (int i = 0; i < 4; ++i) {
    gate.recordClaimResult(source, false,
                           701 + kAuthFailureCooldownMs);
  }
  CHECK_TRUE(gate.recordClaimResult(source, true,
                                    701 + kAuthFailureCooldownMs),
             "successful claim clears source failures");
  result = gate.evaluate(post, unclaimed, ap, source,
                         701 + kAuthFailureCooldownMs);
  CHECK_TRUE(result.allowed,
             "claim success reset prevents stale source lockout");
}

static void testParserGateAndRouteCapComposeBeforeBody() {
  const SecurityGateSnapshot claimed = claimedSecurity();
  const InterfaceSnapshot sta = staNetwork();
  const std::string admin = basicHeader("admin", claimed.credentials.adminSecret);
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);

  const std::string unknownHeaders =
    "POST /unknown HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 9999\r\n"
    "\r\n";
  const std::string unknownWire = unknownHeaders + std::string(20, 'x');
  bp_http::BoundedHttpRequest unknown;
  unknown.reset(800);
  const bp_http::ConsumeResult unknownParse = unknown.consume(
    reinterpret_cast<const uint8_t*>(unknownWire.data()),
    unknownWire.size(), 800);
  CHECK_EQ(static_cast<int>(unknownParse.state),
           static_cast<int>(bp_http::RequestState::WAIT_POLICY),
           "unknown POST stops at policy before body");
  CHECK_EQ(unknownParse.consumed, unknownHeaders.size(),
           "unknown POST body remains unread");
  const GateResult unknownGate = gate.evaluate(
    unknown, claimed, sta, 0x0a000040U, 800);
  CHECK_TRUE(!unknownGate.allowed && unknownGate.status == 404,
             "unknown POST denied without accepting body");
  CHECK_EQ(static_cast<int>(unknown.state()),
           static_cast<int>(bp_http::RequestState::WAIT_POLICY),
           "denied request never transitions into body state");

  auto parseConfigure = [&admin](bp_http::BoundedHttpRequest& request,
                                 uint32_t contentLength) {
    request.reset(810);
    const std::string headers =
      "POST /configure HTTP/1.1\r\n"
      "Host: 10.0.0.5\r\n"
      "Authorization: " + admin + "\r\n"
      "Origin: http://10.0.0.5\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: " + std::to_string(contentLength) + "\r\n\r\n";
    size_t offset = 0;
    while (offset < headers.size() &&
           request.state() != bp_http::RequestState::WAIT_POLICY &&
           request.state() != bp_http::RequestState::REJECT) {
      const bp_http::ConsumeResult part = request.consume(
        reinterpret_cast<const uint8_t*>(headers.data() + offset),
        headers.size() - offset, 810);
      offset += part.consumed;
    }
  };

  bp_http::BoundedHttpRequest oversized;
  parseConfigure(oversized, 513);
  const GateResult oversizedGate = gate.evaluate(
    oversized, claimed, sta, 0x0a000041U, 810);
  CHECK_TRUE(oversizedGate.allowed, "authorized route reaches body-cap gate");
  CHECK_TRUE(!oversized.acceptPolicy(
               oversizedGate.bodyMode, oversizedGate.bodyCap, 811),
             "route cap rejects 513-byte configure body before read");
  CHECK_EQ(static_cast<int>(oversized.error()),
           static_cast<int>(bp_http::RequestError::PAYLOAD_TOO_LARGE),
           "route-cap rejection has 413-class parser reason");

  bp_http::BoundedHttpRequest legal;
  parseConfigure(legal, 319);
  const GateResult legalGate = gate.evaluate(
    legal, claimed, sta, 0x0a000042U, 812);
  CHECK_TRUE(legalGate.allowed, "legal configure request passes central gate");
  CHECK_TRUE(legal.acceptPolicy(legalGate.bodyMode, legalGate.bodyCap, 812),
             "legal configure body cap accepted");
  const std::string body(319, 'a');
  size_t offset = 0;
  while (legal.state() == bp_http::RequestState::BODY) {
    const bp_http::ConsumeResult part = legal.consume(
      reinterpret_cast<const uint8_t*>(body.data() + offset),
      body.size() - offset, 812);
    offset += part.consumed;
  }
  CHECK_EQ(offset, body.size(), "legal body consumes exact declared bytes");
  CHECK_EQ(static_cast<int>(legal.state()),
           static_cast<int>(bp_http::RequestState::READY),
           "legal route reaches ready only after bounded body");
}

static void testGateRejectsIncompleteParserAndAliasedCredentials() {
  AuthFailureLimiter limiter;
  WebRequestGate gate(&limiter);
  SecurityGateSnapshot unclaimed = claimedSecurity();
  unclaimed.claimState = DeviceClaimState::UNCLAIMED;
  const InterfaceSnapshot ap = provisioningNetwork();

  bp_http::BoundedHttpRequest partial;
  partial.reset(900);
  static constexpr char kPartialHeaders[] =
    "POST /claim HTTP/1.1\r\n"
    "Host: 192.168.4.1\r\n"
    "Origin: http://attacker.local";
  const bp_http::ConsumeResult parsed = partial.consume(
    reinterpret_cast<const uint8_t*>(kPartialHeaders),
    sizeof(kPartialHeaders) - 1, 900);
  CHECK_EQ(static_cast<int>(parsed.state),
           static_cast<int>(bp_http::RequestState::HEADERS),
           "adversarial request remains incomplete before final CRLF");
  GateResult result = gate.evaluate(
    partial, unclaimed, ap, 0xc0a80455U, 900);
  CHECK_TRUE(!result.allowed && result.status == 400,
             "gate cannot authorize a partial parser snapshot");

  SecurityGateSnapshot aliased = claimedSecurity();
  aliased.credentials.adminSecret = aliased.credentials.staffSecret;
  aliased.credentials.adminSecretLength =
    aliased.credentials.staffSecretLength;
  const std::string forgedAdmin = basicHeader(
    "admin", aliased.credentials.staffSecret);
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/config", "10.0.0.5",
                forgedAdmin.c_str()),
    aliased, staNetwork(), 0x0a000055U, 901);
  CHECK_TRUE(!result.allowed && result.status == 503,
             "aliased staff/admin credentials fail closed before auth");
  CHECK_EQ(static_cast<int>(result.reason),
           static_cast<int>(GateReason::INVALID_CREDENTIAL_STATE),
           "duplicate role secrets report invalid committed state");

  char duplicateAdmin[kCanonicalCredentialChars + 1] = {};
  std::memcpy(duplicateAdmin, aliased.credentials.staffSecret,
              kCanonicalCredentialChars);
  aliased.credentials.adminSecret = duplicateAdmin;
  result = gate.evaluate(
    makeRequest(bp_http::RequestMethod::GET, "/config", "10.0.0.5",
                forgedAdmin.c_str()),
    aliased, staNetwork(), 0x0a000056U, 902);
  CHECK_TRUE(!result.allowed && result.status == 503,
             "equal role secrets in distinct buffers also fail closed");
  CHECK_EQ(limiter.globalFailureCount(), 0,
           "invalid credential snapshots never consume auth limiter state");
}

int main() {
  testInterfaceClassificationFailsClosed();
  testHostMatchesOnlyAcceptedInterfaceIdentity();
  testMutationCsrfUsesFreshHost();
  testClaimAndRoleGate();
  testDefaultDenyStateAndHostOrdering();
  testAuthenticationRateAndCsrfComposition();
  testEveryRegistryRoutePassesOneCentralGate();
  testGateIsAllocationFreeAndFuzzFailsClosed();
  testClaimTokenOutcomesShareBoundedLimiter();
  testParserGateAndRouteCapComposeBeforeBody();
  testGateRejectsIncompleteParserAndAliasedCredentials();
  return testReport();
}
