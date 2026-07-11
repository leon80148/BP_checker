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

static bool objectContainsBytes(const void* object, size_t objectSize,
                                const uint8_t* needle,
                                size_t needleLength) {
  if (object == nullptr || needle == nullptr || needleLength == 0 ||
      needleLength > objectSize) {
    return false;
  }
  const auto* bytes = static_cast<const uint8_t*>(object);
  for (size_t offset = 0; offset + needleLength <= objectSize; ++offset) {
    if (std::memcmp(bytes + offset, needle, needleLength) == 0) return true;
  }
  return false;
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
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Cache-Control: no-store, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n\r\nOK";
  CHECK_EQ(transaction.capture(
             reinterpret_cast<const uint8_t*>(response),
             sizeof(response) - 1),
           sizeof(response) - 1,
           "handler response is captured before socket output");
  CHECK_TRUE(transaction.capturedResponseIsValidHttp1(),
             "adapter can validate the captured response before publish");
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

static void testHandoffDeadlinesAndMethodMetadata() {
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

  BoundedHttpTransaction invalidForm;
  invalidForm.begin(1100);
  (void)feedUntilBoundary(invalidForm, authenticatedGet, 1100);
  CHECK_TRUE(invalidForm.acceptPolicy(BodyMode::NONE, 0, 1101),
             "pre-dispatch rejection fixture accepts policy");
  CHECK_TRUE(invalidForm.rejectDispatch(400, 1102),
             "validated-input failure queues bounded rejection");
  CHECK_EQ(invalidForm.queuedStatus(), 400,
           "pre-dispatch rejection preserves selected status");
  CHECK_STR(invalidForm.request().view().authorization, "",
            "pre-dispatch rejection wipes authorization");

  BoundedHttpTransaction lateInputValidation;
  lateInputValidation.begin(1200);
  (void)feedUntilBoundary(lateInputValidation, get, 1200);
  CHECK_TRUE(lateInputValidation.acceptPolicy(BodyMode::NONE, 0, 1201),
             "late validation fixture accepts policy");
  CHECK_TRUE(lateInputValidation.rejectDispatch(
               400, 1201 + BoundedHttpTransaction::kHandoffDeadlineMs),
             "late input validation still queues bounded response");
  CHECK_EQ(lateInputValidation.queuedStatus(), 503,
           "late pre-dispatch rejection cannot bypass handoff deadline");

  BoundedHttpTransaction allocationFailure;
  allocationFailure.begin(1300);
  (void)feedUntilBoundary(allocationFailure, authenticatedGet, 1300);
  CHECK_TRUE(allocationFailure.acceptPolicy(BodyMode::NONE, 0, 1301),
             "allocation-failure fixture accepts policy");
  CHECK_TRUE(allocationFailure.beginDispatch(1302),
             "allocation-failure fixture begins capture");
  (void)allocationFailure.capture(
    reinterpret_cast<const uint8_t*>(partialSecret),
    sizeof(partialSecret) - 1);
  CHECK_TRUE(allocationFailure.rejectCapture(503, 1303),
             "handler allocation failure substitutes bounded response");
  CHECK_EQ(allocationFailure.queuedStatus(), 503,
           "capture rejection records service-unavailable outcome");
  const std::string allocationResponse = drain(allocationFailure);
  CHECK_TRUE(allocationResponse.find(
               "HTTP/1.1 503 Service Unavailable\r\n") == 0,
             "handler allocation failure emits deterministic status");
  CHECK_TRUE(allocationResponse.find(partialSecret) == std::string::npos,
             "handler allocation failure never leaks partial output");
}

static void testStreamFragmentationBudgetBackpressureAndPipeline() {
  BoundedHttpTransaction stream;
  stream.begin(400);
  static const char streamHeaders[] =
    "POST /stream HTTP/1.1\r\n"
    "Host: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 600\r\n\r\n";
  uint8_t wire[604] = {};
  for (size_t i = 0; i < 600; ++i) {
    wire[i] = static_cast<uint8_t>((i * 37U) & 0xffU);
  }
  std::memcpy(wire + 600, "PIPE", 4);

  CHECK_EQ(feedUntilBoundary(stream, streamHeaders, 400),
           sizeof(streamHeaders) - 1,
           "stream headers stop at the central policy boundary");
  CHECK_TRUE(stream.acceptPolicy(BodyMode::STREAM, 600, 401),
             "authorized binary stream enters bounded body reading");
  CHECK_EQ(static_cast<int>(stream.state()),
           static_cast<int>(TransactionState::READING_BODY),
           "accepted stream waits for a consumer-drained chunk");

  size_t offset = 0;
  uint8_t reconstructed[600] = {};
  size_t reconstructedLength = 0;
  const size_t budgets[] = {1, 17, 255, 999};
  size_t budgetIndex = 0;
  while (offset < 600) {
    const size_t budget = budgets[budgetIndex++ % 4];
    const TransactionConsume part = stream.consume(
      wire + offset, sizeof(wire) - offset, 402, budget);
    CHECK_TRUE(part.consumed > 0 &&
                 part.consumed <= BoundedHttpRequest::kStreamChunkLimit,
               "each stream read obeys caller and 256-byte hard budgets");
    CHECK_TRUE(part.consumed <= budget,
               "stream read never exceeds a smaller caller budget");
    offset += part.consumed;
    CHECK_TRUE(offset <= 600,
               "declared length prevents consuming pipelined bytes");
    CHECK_EQ(static_cast<int>(stream.state()),
             static_cast<int>(TransactionState::READING_BODY),
             "even the final received chunk is not dispatch-ready before drain");

    const RequestBodyChunk pending = stream.pendingStreamChunk();
    CHECK_TRUE(pending.data != nullptr,
               "transaction exposes a read-only pending stream view");
    CHECK_EQ(pending.length, part.consumed,
             "pending view matches socket consumption exactly");
    static_assert(std::is_same<decltype(pending.data), const uint8_t*>::value,
                  "pending stream bytes are read-only");
    std::memcpy(reconstructed + reconstructedLength,
                pending.data, pending.length);
    reconstructedLength += pending.length;

    const TransactionConsume blocked = stream.consume(
      wire + offset, sizeof(wire) - offset, 403, 999);
    CHECK_EQ(blocked.consumed, 0UL,
             "undrained chunk applies backpressure to socket input");
    CHECK_TRUE(stream.drainStreamChunk(404),
               "consumer explicitly drains one pending chunk");
    CHECK_EQ(stream.pendingStreamChunk().length, 0UL,
             "drain removes the pending chunk view");
    for (size_t i = 0; i < pending.length; ++i) {
      CHECK_EQ(pending.data[i], static_cast<uint8_t>(0),
               "drain securely zeros bytes behind the former read-only view");
    }
  }

  CHECK_EQ(offset, 600UL, "stream consumes exactly Content-Length bytes");
  CHECK_EQ(reconstructedLength, 600UL,
           "fragmented stream reconstructs the complete body");
  CHECK_TRUE(std::memcmp(reconstructed, wire, 600) == 0,
             "stream preserves arbitrary binary byte order");
  CHECK_EQ(static_cast<int>(stream.state()),
           static_cast<int>(TransactionState::DISPATCH_READY),
           "only explicit final-chunk drain makes stream dispatch-ready");
  CHECK_TRUE(!stream.drainStreamChunk(405),
             "drain is rejected when no stream chunk is pending");
  CHECK_EQ(sizeof(wire) - offset, 4UL,
           "four pipelined bytes remain for the closed connection");
}

static void testStreamPolicyMediaTypeAndHardCap() {
  static_assert(BoundedHttpRequest::kStreamBodyLimit == 1310720UL,
                "stream hard cap matches the maximum update image envelope");

  auto prepare = [](BoundedHttpTransaction& transaction,
                    const char* contentLength,
                    const char* contentType) {
    transaction.begin(500);
    std::string headers =
      "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n";
    if (contentType != nullptr) {
      headers += "Content-Type: ";
      headers += contentType;
      headers += "\r\n";
    }
    if (contentLength != nullptr) {
      headers += "Content-Length: ";
      headers += contentLength;
      headers += "\r\n";
    }
    headers += "\r\n";
    (void)feedUntilBoundary(transaction, headers, 500);
  };

  BoundedHttpTransaction missingLength;
  prepare(missingLength, nullptr, "application/octet-stream");
  CHECK_TRUE(!missingLength.acceptPolicy(BodyMode::STREAM, 600, 501),
             "stream requires explicit Content-Length");
  CHECK_EQ(missingLength.queuedStatus(), 411,
           "missing stream length becomes Length Required");

  BoundedHttpTransaction missingType;
  prepare(missingType, "1", nullptr);
  CHECK_TRUE(!missingType.acceptPolicy(BodyMode::STREAM, 600, 501),
             "stream requires an explicit binary media type");
  CHECK_EQ(missingType.queuedStatus(), 415,
           "missing stream type becomes Unsupported Media Type");

  BoundedHttpTransaction wrongType;
  prepare(wrongType, "1", "application/x-www-form-urlencoded");
  CHECK_TRUE(!wrongType.acceptPolicy(BodyMode::STREAM, 600, 501),
             "stream rejects non-binary media type");
  CHECK_EQ(wrongType.queuedStatus(), 415,
           "wrong stream type becomes Unsupported Media Type");

  BoundedHttpTransaction parameterizedType;
  prepare(parameterizedType, "1", "application/octet-stream; charset=utf-8");
  CHECK_TRUE(!parameterizedType.acceptPolicy(
               BodyMode::STREAM, 600, 501),
             "stream media type is exact and parameter-free");
  CHECK_EQ(parameterizedType.queuedStatus(), 415,
           "parameterized binary type fails closed");

  BoundedHttpTransaction caseInsensitiveType;
  prepare(caseInsensitiveType, "1", "Application/Octet-Stream");
  CHECK_TRUE(caseInsensitiveType.acceptPolicy(
               BodyMode::STREAM, 600, 501),
             "HTTP media type token comparison is case-insensitive");

  BoundedHttpTransaction routeOversize;
  prepare(routeOversize, "601", "application/octet-stream");
  CHECK_TRUE(!routeOversize.acceptPolicy(BodyMode::STREAM, 600, 501),
             "route-specific stream cap rejects oversized body");
  CHECK_EQ(routeOversize.queuedStatus(), 413,
           "route cap rejection becomes Payload Too Large");

  BoundedHttpTransaction emptyStream;
  prepare(emptyStream, "0", "application/octet-stream");
  CHECK_TRUE(!emptyStream.acceptPolicy(BodyMode::STREAM, 600, 501),
             "empty binary stream cannot bypass final-chunk drain");
  CHECK_EQ(emptyStream.queuedStatus(), 400,
           "zero-length stream is an invalid binary request");

  BoundedHttpTransaction invalidGlobalCap;
  prepare(invalidGlobalCap, "1", "application/octet-stream");
  CHECK_TRUE(!invalidGlobalCap.acceptPolicy(
               BodyMode::STREAM,
               BoundedHttpRequest::kStreamBodyLimit + 1UL, 501),
             "caller cannot install a route cap above the global hard limit");
  CHECK_EQ(invalidGlobalCap.queuedStatus(), 500,
           "invalid oversized route policy fails closed");

  BoundedHttpTransaction exactGlobalCap;
  prepare(exactGlobalCap, "1310720", "application/octet-stream");
  CHECK_TRUE(exactGlobalCap.acceptPolicy(
               BodyMode::STREAM, BoundedHttpRequest::kStreamBodyLimit, 501),
             "maximum supported image length is accepted exactly");

  BoundedHttpTransaction transferEncoding;
  transferEncoding.begin(500);
  static const uint8_t chunked[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n";
  (void)transferEncoding.consume(chunked, sizeof(chunked) - 1, 500);
  CHECK_EQ(transferEncoding.queuedStatus(), 501,
           "Transfer-Encoding is parser-denied before stream policy");

  BoundedHttpTransaction expectation;
  expectation.begin(500);
  static const uint8_t expect[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 1\r\nExpect: 100-continue\r\n\r\n";
  (void)expectation.consume(expect, sizeof(expect) - 1, 500);
  CHECK_EQ(expectation.queuedStatus(), 417,
           "Expect is parser-denied before stream policy");
}

static void testStreamAbsoluteDeadlineWrapAndSmallFormDeadline() {
  static_assert(BoundedHttpRequest::kBodyDeadlineMs == 1500U,
                "small form body deadline remains short");
  static_assert(BoundedHttpRequest::kStreamBodyDeadlineMs == 120000U,
                "stream gets one finite two-minute absolute deadline");

  const uint32_t start = UINT32_MAX - 59999U;
  BoundedHttpTransaction stream;
  stream.begin(start - 1U);
  static const char headers[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 2\r\n\r\n";
  (void)feedUntilBoundary(stream, headers, start - 1U);
  CHECK_TRUE(stream.acceptPolicy(BodyMode::STREAM, 2, start),
             "wrapped deadline fixture accepts stream");
  static const uint8_t first[] = {0x91};
  CHECK_EQ(stream.consume(first, sizeof(first), start + 119999U).consumed,
           1UL, "late stream progress is accepted before absolute deadline");
  CHECK_TRUE(stream.pendingStreamChunk().length == 1,
             "late progress produces one pending chunk");
  CHECK_TRUE(stream.poll(start + 120000U),
             "deadline timeout is converted to a bounded response");
  CHECK_EQ(stream.queuedStatus(), 408,
           "stream trickle does not extend the absolute deadline");
  CHECK_EQ(static_cast<int>(stream.state()),
           static_cast<int>(TransactionState::SENDING_RESPONSE),
           "wrapped stream timeout fails through response pump");
  CHECK_EQ(stream.pendingStreamChunk().length, 0UL,
           "timeout wipes pending binary bytes");

  BoundedHttpTransaction lateDrain;
  lateDrain.begin(6000);
  static const char oneByteHeaders[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 1\r\n\r\n";
  (void)feedUntilBoundary(lateDrain, oneByteHeaders, 6000);
  CHECK_TRUE(lateDrain.acceptPolicy(BodyMode::STREAM, 1, 6001),
             "late-drain fixture accepts stream");
  CHECK_EQ(lateDrain.consume(first, sizeof(first), 6002).consumed, 1UL,
           "final chunk arrives within the absolute window");
  CHECK_TRUE(!lateDrain.drainStreamChunk(
               6001 + BoundedHttpRequest::kStreamBodyDeadlineMs),
             "direct final drain cannot bypass the absolute deadline");
  CHECK_EQ(lateDrain.queuedStatus(), 408,
           "late direct drain queues Request Timeout");
  CHECK_EQ(lateDrain.pendingStreamChunk().length, 0UL,
           "late direct drain wipes the expired chunk");

  BoundedHttpTransaction smallForm;
  smallForm.begin(1000);
  static const char formHeaders[] =
    "POST /form HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 2\r\n\r\n";
  (void)feedUntilBoundary(smallForm, formHeaders, 1000);
  CHECK_TRUE(smallForm.acceptPolicy(BodyMode::SMALL_FORM, 2, 1001),
             "small-form deadline fixture accepts policy");
  CHECK_TRUE(smallForm.poll(2500),
             "small form remains active one millisecond before deadline");
  CHECK_TRUE(smallForm.poll(2501),
             "small form times out at the unchanged 1500ms boundary");
  CHECK_EQ(smallForm.queuedStatus(), 408,
           "small form retains its short request timeout");
}

static void testStreamInvalidDrainWipeDestructorAndNoAllocation() {
  static const uint8_t secretChunk[] = {
    0xde, 0xad, 0xfa, 0xce, 0x13, 0x57, 0x9b, 0xdf,
  };
  static const char headers[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 8\r\n\r\n";

  BoundedHttpTransaction invalid;
  CHECK_TRUE(!invalid.drainStreamChunk(1),
             "drain is invalid before a transaction begins");
  invalid.begin(1);
  (void)feedUntilBoundary(invalid, headers, 1);
  CHECK_TRUE(!invalid.drainStreamChunk(2),
             "drain is invalid while waiting for route policy");
  CHECK_TRUE(invalid.acceptPolicy(BodyMode::STREAM, 8, 2),
             "invalid-drain fixture accepts policy");
  CHECK_TRUE(!invalid.drainStreamChunk(3),
             "drain is invalid before any chunk is pending");

  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    (void)invalid.consume(secretChunk, sizeof(secretChunk), 3, 999);
    (void)invalid.pendingStreamChunk();
    (void)invalid.drainStreamChunk(4);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "stream consume/view/drain work with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "stream transaction makes no dynamic allocation attempt");

  BoundedHttpTransaction aborted;
  aborted.begin(10);
  (void)feedUntilBoundary(aborted, headers, 10);
  CHECK_TRUE(aborted.acceptPolicy(BodyMode::STREAM, 8, 11),
             "abort wipe fixture accepts policy");
  (void)aborted.consume(secretChunk, sizeof(secretChunk), 12);
  CHECK_TRUE(objectContainsBytes(&aborted, sizeof(aborted),
                                 secretChunk, sizeof(secretChunk)),
             "pending binary canary exists before abort");
  aborted.abort();
  CHECK_EQ(static_cast<int>(aborted.state()),
           static_cast<int>(TransactionState::ABORTED),
           "abort terminates stream transaction");
  CHECK_EQ(aborted.pendingStreamChunk().length, 0UL,
           "abort removes pending stream view");
  CHECK_TRUE(!objectContainsBytes(&aborted, sizeof(aborted),
                                  secretChunk, sizeof(secretChunk)),
             "abort securely clears pending binary bytes");
  CHECK_TRUE(!aborted.drainStreamChunk(13),
             "drain is rejected after abort");

  BoundedHttpTransaction reset;
  reset.begin(20);
  (void)feedUntilBoundary(reset, headers, 20);
  CHECK_TRUE(reset.acceptPolicy(BodyMode::STREAM, 8, 21),
             "reset wipe fixture accepts policy");
  (void)reset.consume(secretChunk, sizeof(secretChunk), 22);
  reset.begin(23);
  CHECK_TRUE(!objectContainsBytes(&reset, sizeof(reset),
                                  secretChunk, sizeof(secretChunk)),
             "begin/reset securely clears pending binary bytes");

  alignas(BoundedHttpTransaction)
    uint8_t storage[sizeof(BoundedHttpTransaction)];
  std::memset(storage, 0xa5, sizeof(storage));
  auto* placed =
    ::new (static_cast<void*>(storage)) BoundedHttpTransaction();
  placed->begin(30);
  (void)feedUntilBoundary(*placed, headers, 30);
  CHECK_TRUE(placed->acceptPolicy(BodyMode::STREAM, 8, 31),
             "destructor wipe fixture accepts policy");
  (void)placed->consume(secretChunk, sizeof(secretChunk), 32);
  CHECK_TRUE(objectContainsBytes(storage, sizeof(storage),
                                 secretChunk, sizeof(secretChunk)),
             "pending binary canary exists before destruction");
  placed->~BoundedHttpTransaction();
  CHECK_TRUE(!objectContainsBytes(storage, sizeof(storage),
                                  secretChunk, sizeof(secretChunk)),
             "transaction destruction securely clears stream bytes");
}

static void testStreamConsumerCanRejectBodyWithBoundedResponse() {
  BoundedHttpTransaction transaction;
  transaction.begin(9000);
  static const char headers[] =
    "POST /stream HTTP/1.1\r\nHost: 10.0.0.5\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 4\r\n\r\n";
  (void)feedUntilBoundary(transaction, headers, 9000);
  CHECK_TRUE(transaction.acceptPolicy(BodyMode::STREAM, 4, 9001),
             "consumer rejection fixture accepts stream policy");
  const uint8_t secret[] = {0xde, 0xad, 0xbe, 0xef};
  (void)transaction.consume(secret, sizeof(secret), 9002);
  CHECK_TRUE(transaction.rejectBody(503, 9003),
             "stream consumer failure queues a bounded error response");
  CHECK_EQ(transaction.queuedStatus(), 503,
           "consumer failure is Service Unavailable");
  CHECK_EQ(transaction.pendingStreamChunk().length, 0UL,
           "consumer rejection wipes the pending stream chunk");
  CHECK_TRUE(!objectContainsBytes(&transaction, sizeof(transaction),
                                  secret, sizeof(secret)),
             "consumer rejection wipes binary canary bytes");
  const std::string response = drain(transaction);
  CHECK_TRUE(response.find("HTTP/1.1 503 Service Unavailable\r\n") == 0,
             "consumer rejection uses the bounded HTTP envelope");
  CHECK_TRUE(!transaction.rejectBody(503, 9004),
             "body rejection is invalid after response begins");
}

int main() {
  testDeniedPolicyNeverConsumesBody();
  testAllowedBodyAndCapturedResponseLifecycle();
  testBoundsTimeoutsAndOverflowFailClosed();
  testSendDeadlineInvalidTransitionsAndNoAllocation();
  testHandoffDeadlinesAndMethodMetadata();
  testStreamFragmentationBudgetBackpressureAndPipeline();
  testStreamPolicyMediaTypeAndHardCap();
  testStreamAbsoluteDeadlineWrapAndSmallFormDeadline();
  testStreamInvalidDrainWipeDestructorAndNoAllocation();
  testStreamConsumerCanRejectBodyWithBoundedResponse();
  return testReport();
}
