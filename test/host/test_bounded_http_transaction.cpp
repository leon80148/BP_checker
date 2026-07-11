#include <Arduino.h>

#include "lib/BoundedHttpTransaction.h"
#include "test_support.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>

using namespace bp_http;

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

static size_t feedUntilBoundary(BoundedHttpTransaction& transaction,
                                const std::string& wire, uint32_t nowMs) {
  size_t offset = 0;
  while (offset < wire.size() &&
         (transaction.state() == TransactionState::READING_HEADERS ||
          transaction.state() == TransactionState::READING_BODY)) {
    const TransactionConsume part = transaction.consume(
      reinterpret_cast<const uint8_t*>(wire.data() + offset),
      wire.size() - offset, nowMs);
    if (part.consumed == 0) break;
    offset += part.consumed;
  }
  return offset;
}

static std::string drain(BoundedHttpTransaction& transaction,
                         size_t budget = 113) {
  std::string wire;
  while (transaction.state() == TransactionState::SENDING_RESPONSE) {
    const ResponseChunk chunk = transaction.nextOutput(budget);
    if (chunk.length == 0) break;
    wire.append(reinterpret_cast<const char*>(chunk.data), chunk.length);
    if (!transaction.acknowledgeOutput(chunk.length)) break;
  }
  return wire;
}

static void testDeniedPolicyNeverConsumesBody() {
  BoundedHttpTransaction transaction;
  transaction.begin(100);
  const std::string headers =
    "POST /unknown HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 2000\r\n\r\n";
  const std::string wire = headers + std::string(300, 'x');
  const size_t consumed = feedUntilBoundary(transaction, wire, 100);
  CHECK_EQ(consumed, headers.size(),
           "transaction stops before unknown request body");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::WAIT_POLICY),
           "complete headers wait for central policy");
  CHECK_TRUE(transaction.rejectPolicy(404, 101),
             "central denial queues a bounded response");
  CHECK_EQ(transaction.queuedStatus(), 404,
           "denial preserves the selected HTTP status");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "denial enters nonblocking response pump");
  CHECK_STR(transaction.request().view().host, "",
            "denial wipes parsed request metadata before send");

  const std::string response = drain(transaction, 17);
  CHECK_TRUE(response.find("HTTP/1.1 404 Not Found\r\n") == 0,
             "denial emits a complete status line");
  CHECK_TRUE(response.find("Cache-Control: no-store, max-age=0\r\n") !=
               std::string::npos,
             "every denial is non-cacheable");
  CHECK_TRUE(response.find("Connection: close\r\n") != std::string::npos,
             "bounded transaction always closes HTTP connection");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::COMPLETE),
           "acknowledging all denial bytes completes transaction");
  CHECK_TRUE(transaction.terminal(),
             "deferred action becomes eligible only at terminal state");
}

static void testAllowedBodyAndCapturedResponseLifecycle() {
  BoundedHttpTransaction transaction;
  transaction.begin(200);
  static const char headers[] =
    "POST /configure HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Authorization: Basic YWRtaW46c2VjcmV0\r\n"
    "Origin: http://10.0.0.5\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 11\r\n\r\n";
  CHECK_EQ(feedUntilBoundary(transaction, headers, 200),
           sizeof(headers) - 1,
           "authorized fixture consumes only complete headers");
  CHECK_TRUE(transaction.acceptPolicy(BodyMode::SMALL_FORM, 512, 201),
             "authorized route installs its body policy");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::READING_BODY),
           "nonempty accepted form enters bounded body state");

  static const char body[] = "ssid=clinic";
  CHECK_EQ(feedUntilBoundary(transaction, body, 202), sizeof(body) - 1,
           "accepted route consumes exact declared form body");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::DISPATCH_READY),
           "complete authorized body becomes dispatchable");
  CHECK_STR(transaction.request().body(), body,
            "handler sees the bounded form before dispatch");
  CHECK_TRUE(transaction.beginDispatch(202),
             "dispatch begins one transactional response");
  static const char response[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
  CHECK_EQ(transaction.capture(
             reinterpret_cast<const uint8_t*>(response),
             sizeof(response) - 1),
           sizeof(response) - 1,
           "handler response is captured before socket output");
  CHECK_TRUE(transaction.finishDispatch(203),
             "complete captured response enters send phase");
  CHECK_STR(transaction.request().view().authorization, "",
            "authorization is wiped immediately after handler dispatch");
  CHECK_STR(transaction.request().body(), "",
            "form secrets are wiped immediately after handler dispatch");

  const ResponseChunk offered = transaction.nextOutput(5000);
  CHECK_TRUE(offered.length > 0 &&
               offered.length <= BoundedHttpResponse::kSendBudget,
             "socket offer enforces one-kibibyte loop budget");
  const ResponseChunk repeated = transaction.nextOutput(1);
  CHECK_TRUE(repeated.data == offered.data &&
               repeated.length == offered.length,
             "socket re-poll cannot rewrite an outstanding offer");
  CHECK_TRUE(transaction.acknowledgeOutput(offered.length),
             "offered socket progress is acknowledged exactly");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::COMPLETE),
           "short captured response completes after one acknowledgement");
}

static void testBoundsTimeoutsAndOverflowFailClosed() {
  BoundedHttpTransaction oversized;
  oversized.begin(300);
  const std::string headers =
    "POST /configure HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 513\r\n\r\n";
  (void)feedUntilBoundary(oversized, headers, 300);
  CHECK_TRUE(!oversized.acceptPolicy(BodyMode::SMALL_FORM, 512, 301),
             "route body cap rejects oversized declared body");
  CHECK_EQ(oversized.queuedStatus(), 413,
           "oversized route body queues payload-too-large response");
  CHECK_EQ(static_cast<int>(oversized.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "parser rejection is converted into bounded response");

  BoundedHttpTransaction slowHeader;
  slowHeader.begin(1000);
  static const char partial[] = "GET / HTTP/1.1\r\nHost: ";
  (void)slowHeader.consume(reinterpret_cast<const uint8_t*>(partial),
                           sizeof(partial) - 1, 1000);
  CHECK_TRUE(slowHeader.poll(2499),
             "partial header remains active before absolute deadline");
  CHECK_TRUE(slowHeader.poll(2500),
             "header timeout is converted to a sendable error");
  CHECK_EQ(slowHeader.queuedStatus(), 408,
           "header timeout queues request-timeout status");
  CHECK_EQ(static_cast<int>(slowHeader.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "timed-out request still uses bounded response pump");

  BoundedHttpTransaction overflow;
  overflow.begin(400);
  static const char get[] = "GET / HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n";
  (void)feedUntilBoundary(overflow, get, 400);
  CHECK_TRUE(overflow.acceptPolicy(BodyMode::NONE, 0, 401),
             "bodyless request accepts zero cap");
  CHECK_TRUE(overflow.beginDispatch(401),
             "overflow fixture begins dispatch");
  uint8_t full[BoundedHttpResponse::kCapacity] = {};
  CHECK_EQ(overflow.capture(full, sizeof(full)), sizeof(full),
           "exact response capacity is initially captured");
  CHECK_EQ(overflow.capture(reinterpret_cast<const uint8_t*>("x"), 1), 0UL,
           "one byte beyond response capacity trips overflow");
  CHECK_TRUE(overflow.finishDispatch(402),
             "overflow finalizes a safe atomic fallback");
  CHECK_EQ(overflow.queuedStatus(), 503,
           "overflow records service-unavailable outcome");
  const std::string fallback = drain(overflow);
  CHECK_TRUE(fallback.find("HTTP/1.1 503 Service Unavailable\r\n") == 0,
             "overflow sends no partial handler response");
}

static void testSendDeadlineInvalidTransitionsAndNoAllocation() {
  BoundedHttpTransaction transaction;
  CHECK_TRUE(!transaction.acceptPolicy(BodyMode::NONE, 0, 1),
             "policy cannot be accepted before transaction begin");
  CHECK_TRUE(!transaction.rejectPolicy(404, 1),
             "policy cannot be rejected before complete headers");
  CHECK_TRUE(!transaction.beginDispatch(1),
             "dispatch cannot begin from idle state");
  CHECK_TRUE(!transaction.finishDispatch(1),
             "response cannot finalize before dispatch");
  CHECK_TRUE(!transaction.terminal(), "idle is not a completed request");

  transaction.begin(UINT32_MAX - 10U);
  static const char get[] = "GET / HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n";
  (void)feedUntilBoundary(transaction, get, UINT32_MAX - 10U);
  CHECK_TRUE(transaction.rejectPolicy(403, UINT32_MAX - 5U),
             "wrapped-clock denial begins send phase");
  CHECK_TRUE(transaction.poll(UINT32_MAX - 5U + 1499U),
             "response remains active before wrapped deadline");
  CHECK_TRUE(!transaction.poll(UINT32_MAX - 5U + 1500U),
             "response aborts exactly at wrapped deadline");
  CHECK_EQ(static_cast<int>(transaction.state()),
           static_cast<int>(TransactionState::ABORTED),
           "slow reader leaves no queued response bytes");
  CHECK_TRUE(transaction.terminal(),
             "deferred restart may proceed after send timeout abort");

  static_assert(!std::is_copy_constructible<BoundedHttpTransaction>::value,
                "transaction cannot copy credential-bearing request state");
  static_assert(!std::is_copy_assignable<BoundedHttpTransaction>::value,
                "transaction cannot copy-assign sensitive state");
  BoundedHttpTransaction noAlloc;
  static const uint8_t noAllocHeaders[] =
    "GET /missing HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n";
  noAlloc.begin(10);
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    (void)noAlloc.consume(noAllocHeaders, sizeof(noAllocHeaders) - 1, 10);
    (void)noAlloc.rejectPolicy(404, 10);
    while (noAlloc.state() == TransactionState::SENDING_RESPONSE) {
      const ResponseChunk chunk = noAlloc.nextOutput();
      if (chunk.length == 0) break;
      (void)noAlloc.acknowledgeOutput(chunk.length);
    }
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "full denied transaction works with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "bounded transaction performs no allocation attempt");
  CHECK_EQ(static_cast<int>(noAlloc.state()),
           static_cast<int>(TransactionState::COMPLETE),
           "allocation-free denial drains successfully");
}

static void testHandoffDeadlinesStreamAndMethodMetadata() {
  static const char get[] = "GET / HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n";
  static const char authenticatedGet[] =
    "GET / HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Authorization: Basic YWRtaW46c2VjcmV0\r\n\r\n";

  BoundedHttpTransaction policyWait;
  policyWait.begin(100);
  (void)feedUntilBoundary(policyWait, get, 100);
  CHECK_TRUE(policyWait.poll(1599),
             "policy handoff remains active before absolute deadline");
  CHECK_TRUE(policyWait.poll(1600),
             "policy handoff timeout queues a response");
  CHECK_EQ(policyWait.queuedStatus(), 408,
           "stalled policy handoff becomes request timeout");
  CHECK_EQ(static_cast<int>(policyWait.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "stalled policy handoff cannot pin the client slot");

  BoundedHttpTransaction dispatchWait;
  dispatchWait.begin(200);
  (void)feedUntilBoundary(dispatchWait, authenticatedGet, 200);
  CHECK_TRUE(dispatchWait.acceptPolicy(BodyMode::NONE, 0, 201),
             "dispatch deadline fixture accepts bodyless request");
  CHECK_TRUE(dispatchWait.poll(1700),
             "dispatch-ready state remains active before deadline");
  CHECK_TRUE(dispatchWait.poll(1701),
             "dispatch-ready timeout queues a response");
  CHECK_EQ(dispatchWait.queuedStatus(), 503,
           "stalled dispatch becomes service unavailable");
  CHECK_STR(dispatchWait.request().view().host, "",
            "dispatch timeout wipes request metadata");
  CHECK_STR(dispatchWait.request().view().authorization, "",
            "dispatch timeout wipes authorization");

  BoundedHttpTransaction captureWait;
  captureWait.begin(300);
  (void)feedUntilBoundary(captureWait, authenticatedGet, 300);
  CHECK_TRUE(captureWait.acceptPolicy(BodyMode::NONE, 0, 301),
             "capture deadline fixture accepts request");
  CHECK_TRUE(captureWait.beginDispatch(301),
             "capture deadline fixture begins response");
  static const char partialSecret[] = "partial-clinic-secret";
  (void)captureWait.capture(
    reinterpret_cast<const uint8_t*>(partialSecret),
    sizeof(partialSecret) - 1);
  CHECK_TRUE(captureWait.poll(1800),
             "capture state remains active before deadline");
  CHECK_TRUE(captureWait.poll(1801),
             "capture timeout substitutes a bounded response");
  CHECK_EQ(captureWait.queuedStatus(), 503,
           "stalled capture becomes service unavailable");
  const std::string captureFailure = drain(captureWait);
  CHECK_TRUE(captureFailure.find(partialSecret) == std::string::npos,
             "capture timeout never sends partial sensitive output");

  BoundedHttpTransaction stream;
  stream.begin(400);
  static const char streamHeaders[] =
    "POST /stream HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Content-Length: 600\r\n\r\n";
  const std::string streamWire =
    std::string(streamHeaders) + std::string(300, 's');
  CHECK_EQ(feedUntilBoundary(stream, streamWire, 400),
           sizeof(streamHeaders) - 1,
           "unsupported stream body remains unread before policy");
  CHECK_TRUE(!stream.acceptPolicy(BodyMode::STREAM, 600, 401),
             "transaction fails closed until stream draining is implemented");
  CHECK_EQ(stream.queuedStatus(), 501,
           "unsupported streaming returns not implemented");
  CHECK_EQ(static_cast<int>(stream.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "unsupported stream cannot enter a stuck body state");

  BoundedHttpTransaction wrongMethod;
  wrongMethod.begin(500);
  (void)feedUntilBoundary(wrongMethod, get, 500);
  CHECK_TRUE(wrongMethod.rejectPolicy(
               405, 501, AllowedMethods::GET),
             "method denial queues bounded response");
  const std::string methodResponse = drain(wrongMethod);
  CHECK_TRUE(methodResponse.find("Allow: GET\r\n") !=
               std::string::npos,
             "405 response declares target-supported method");
  CHECK_TRUE(methodResponse.find("Allow: GET, POST\r\n") ==
               std::string::npos,
             "405 response does not overstate target methods");

  BoundedHttpTransaction missingAllowMetadata;
  missingAllowMetadata.begin(510);
  (void)feedUntilBoundary(missingAllowMetadata, get, 510);
  CHECK_TRUE(missingAllowMetadata.rejectPolicy(405, 511),
             "missing Allow metadata still queues fail-closed response");
  CHECK_EQ(missingAllowMetadata.queuedStatus(), 500,
           "405 cannot be emitted without explicit target method metadata");

  const uint32_t wrappedStart = UINT32_MAX - 749U;
  BoundedHttpTransaction wrappedPolicy;
  wrappedPolicy.begin(wrappedStart);
  (void)feedUntilBoundary(wrappedPolicy, get, wrappedStart);
  CHECK_TRUE(wrappedPolicy.poll(wrappedStart + 1499U),
             "wrapped policy deadline remains active before boundary");
  CHECK_TRUE(wrappedPolicy.poll(wrappedStart + 1500U),
             "wrapped policy deadline queues timeout at boundary");
  CHECK_EQ(wrappedPolicy.queuedStatus(), 408,
           "wrapped handoff deadline remains fail closed");

  BoundedHttpTransaction wrappedDirectPolicy;
  wrappedDirectPolicy.begin(wrappedStart);
  (void)feedUntilBoundary(wrappedDirectPolicy, get, wrappedStart);
  CHECK_TRUE(!wrappedDirectPolicy.acceptPolicy(
               BodyMode::NONE, 0, wrappedStart +
                 BoundedHttpTransaction::kHandoffDeadlineMs),
             "direct late policy API is wrap-safe at exact boundary");
  CHECK_EQ(wrappedDirectPolicy.queuedStatus(), 408,
           "direct wrapped transition queues timeout");

  BoundedHttpTransaction latePolicy;
  latePolicy.begin(600);
  (void)feedUntilBoundary(latePolicy, get, 600);
  CHECK_TRUE(!latePolicy.acceptPolicy(
               BodyMode::NONE, 0, 600 +
                 BoundedHttpTransaction::kHandoffDeadlineMs),
             "direct late policy acceptance fails at exact deadline");
  CHECK_EQ(latePolicy.queuedStatus(), 408,
           "late policy API queues request timeout without prior poll");

  BoundedHttpTransaction lateRejection;
  lateRejection.begin(700);
  (void)feedUntilBoundary(lateRejection, get, 700);
  CHECK_TRUE(lateRejection.rejectPolicy(
               404, 700 + BoundedHttpTransaction::kHandoffDeadlineMs),
             "direct late policy rejection still queues a response");
  CHECK_EQ(lateRejection.queuedStatus(), 408,
           "late policy rejection cannot bypass handoff deadline");

  BoundedHttpTransaction lateDispatch;
  lateDispatch.begin(800);
  (void)feedUntilBoundary(lateDispatch, get, 800);
  CHECK_TRUE(lateDispatch.acceptPolicy(BodyMode::NONE, 0, 801),
             "late-dispatch fixture accepts request on time");
  CHECK_TRUE(!lateDispatch.beginDispatch(
               801 + BoundedHttpTransaction::kHandoffDeadlineMs),
             "direct late dispatch fails at exact deadline");
  CHECK_EQ(lateDispatch.queuedStatus(), 503,
           "late dispatch API queues service unavailable");

  BoundedHttpTransaction lateFinish;
  lateFinish.begin(900);
  (void)feedUntilBoundary(lateFinish, authenticatedGet, 900);
  CHECK_TRUE(lateFinish.acceptPolicy(BodyMode::NONE, 0, 901),
             "late-finish fixture accepts request");
  CHECK_TRUE(lateFinish.beginDispatch(902),
             "late-finish fixture begins capture");
  (void)lateFinish.capture(
    reinterpret_cast<const uint8_t*>(partialSecret),
    sizeof(partialSecret) - 1);
  CHECK_TRUE(!lateFinish.finishDispatch(
               902 + BoundedHttpTransaction::kHandoffDeadlineMs),
             "direct late finish cannot publish captured success");
  CHECK_EQ(lateFinish.queuedStatus(), 503,
           "late finish substitutes service unavailable");
  const std::string lateFailure = drain(lateFinish);
  CHECK_TRUE(lateFailure.find(partialSecret) == std::string::npos,
             "late finish wipes partial sensitive response");

  BoundedHttpTransaction unsupportedMethod;
  unsupportedMethod.begin(1000);
  static const uint8_t deleteReset[] =
    "DELETE /reset HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n";
  (void)unsupportedMethod.consume(deleteReset, sizeof(deleteReset) - 1,
                                  1000);
  CHECK_EQ(unsupportedMethod.queuedStatus(), 501,
           "parser-unsupported method returns not implemented");
  const std::string unsupportedResponse = drain(unsupportedMethod);
  CHECK_TRUE(unsupportedResponse.find("Allow:") == std::string::npos,
             "unsupported method never fabricates target Allow metadata");
}

int main() {
  testDeniedPolicyNeverConsumesBody();
  testAllowedBodyAndCapturedResponseLifecycle();
  testBoundsTimeoutsAndOverflowFailClosed();
  testSendDeadlineInvalidTransitionsAndNoAllocation();
  testHandoffDeadlinesStreamAndMethodMetadata();
  return testReport();
}
