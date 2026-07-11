#include <Arduino.h>

#include "lib/BoundedHttpResponse.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

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

static bool objectContains(const void* object, size_t objectSize,
                           const char* needle) {
  const auto* bytes = static_cast<const uint8_t*>(object);
  const size_t needleLength = std::strlen(needle);
  if (needleLength == 0 || needleLength > objectSize) return false;
  for (size_t offset = 0; offset + needleLength <= objectSize; ++offset) {
    if (std::memcmp(bytes + offset, needle, needleLength) == 0) return true;
  }
  return false;
}

static std::string drainResponse(BoundedHttpResponse& response,
                                 size_t budget = 257) {
  std::string wire;
  while (response.state() == ResponseState::SENDING) {
    const ResponseChunk chunk = response.nextChunk(budget);
    if (chunk.length == 0) break;
    wire.append(reinterpret_cast<const char*>(chunk.data), chunk.length);
    if (!response.acknowledge(chunk.length)) break;
  }
  return wire;
}

static void testTransactionalResponseSupportsPartialSends() {
  BoundedHttpResponse response;
  response.begin();
  static const char header[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\n";
  CHECK_EQ(response.append(reinterpret_cast<const uint8_t*>(header),
                           sizeof(header) - 1),
           sizeof(header) - 1, "header copied into owned transaction");
  CHECK_EQ(response.append(reinterpret_cast<const uint8_t*>("hello"), 5),
           5UL, "body copied into owned transaction");
  CHECK_TRUE(response.finalize(100), "complete transaction begins sending");
  CHECK_EQ(static_cast<int>(response.state()),
           static_cast<int>(ResponseState::SENDING),
           "finalized response enters sending state");

  std::string wire;
  while (response.state() == ResponseState::SENDING) {
    const ResponseChunk chunk = response.nextChunk(7);
    CHECK_TRUE(chunk.length > 0 && chunk.length <= 7,
               "next response chunk honors caller budget");
    wire.append(reinterpret_cast<const char*>(chunk.data), chunk.length);
    CHECK_TRUE(response.acknowledge(chunk.length),
               "partial socket progress acknowledged exactly");
  }
  CHECK_EQ(static_cast<int>(response.state()),
           static_cast<int>(ResponseState::COMPLETE),
           "all acknowledged bytes complete transaction");
  CHECK_STR(wire.c_str(),
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello",
            "partial sends preserve response bytes without duplication");
}

static void testOverflowAtomicallyBecomesFixed503() {
  BoundedHttpResponse response;
  response.begin();
  static const char partial[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\npatient-secret";
  CHECK_EQ(response.append(reinterpret_cast<const uint8_t*>(partial),
                           sizeof(partial) - 1),
           sizeof(partial) - 1, "partial oversized response initially fits");
  std::vector<uint8_t> tooMuch(BoundedHttpResponse::kCapacity, 'x');
  CHECK_EQ(response.append(tooMuch.data(), tooMuch.size()), 0UL,
           "append that exceeds fixed capacity reports failure");
  CHECK_TRUE(response.overflowed(), "overflow is sticky for transaction");
  CHECK_EQ(response.append(reinterpret_cast<const uint8_t*>("ignored"), 7),
           0UL, "overflowed transaction accepts no later fragments");
  CHECK_TRUE(response.finalize(200),
             "overflowed transaction still finalizes safe fallback");
  const std::string wire = drainResponse(response);
  CHECK_TRUE(wire.find("HTTP/1.1 503 Service Unavailable\r\n") == 0,
             "overflow emits a complete 503 response");
  CHECK_TRUE(wire.find("Content-Length: 19\r\n") != std::string::npos,
             "fallback has exact body length");
  CHECK_TRUE(wire.find("Cache-Control: no-store, max-age=0\r\n") !=
               std::string::npos,
             "fallback is never cacheable");
  CHECK_TRUE(wire.find("Connection: close\r\n") != std::string::npos,
             "fallback always closes connection");
  CHECK_TRUE(wire.rfind("response_too_large\n") == wire.size() - 19,
             "fallback body is complete");
  CHECK_TRUE(wire.find("patient-secret") == std::string::npos,
             "partial sensitive response is never sent after overflow");
}

static void testSlowReaderDeadlineAndFailureCleanup() {
  BoundedHttpResponse slow;
  slow.begin();
  static const char sensitive[] = "HTTP/1.1 200 OK\r\n\r\nclinic-secret";
  (void)slow.append(reinterpret_cast<const uint8_t*>(sensitive),
                    sizeof(sensitive) - 1);
  CHECK_TRUE(slow.finalize(100), "slow-reader fixture finalizes");
  CHECK_TRUE(slow.enforceDeadline(1599),
             "response remains sendable before absolute deadline");
  const ResponseChunk first = slow.nextChunk(1);
  CHECK_EQ(first.length, 1UL, "fixture exposes one progress byte");
  CHECK_TRUE(slow.acknowledge(1), "progress before deadline acknowledged");
  CHECK_TRUE(!slow.enforceDeadline(1600),
             "progress does not reset absolute send deadline");
  CHECK_EQ(static_cast<int>(slow.state()),
           static_cast<int>(ResponseState::ABORTED),
           "slow reader aborts exactly at deadline");
  CHECK_EQ(slow.responseLength(), 0UL,
           "deadline abort clears queued response length");
  CHECK_EQ(slow.nextChunk().length, 0UL,
           "aborted response exposes no sensitive bytes");

  const uint32_t wrappedStart = UINT32_MAX - 749U;
  BoundedHttpResponse wrapped;
  wrapped.begin();
  (void)wrapped.append(reinterpret_cast<const uint8_t*>("abc"), 3);
  CHECK_TRUE(wrapped.finalize(wrappedStart), "wrapped fixture finalizes");
  CHECK_TRUE(wrapped.enforceDeadline(wrappedStart + 1499U),
             "wrapped clock remains active before deadline");
  CHECK_TRUE(!wrapped.enforceDeadline(wrappedStart + 1500U),
             "wrapped clock aborts exactly at deadline");
  CHECK_EQ(static_cast<int>(wrapped.state()),
           static_cast<int>(ResponseState::ABORTED),
           "wrapped timeout reaches aborted state");

  BoundedHttpResponse invalidAck;
  invalidAck.begin();
  (void)invalidAck.append(reinterpret_cast<const uint8_t*>("abcdef"), 6);
  CHECK_TRUE(invalidAck.finalize(1), "invalid-ack fixture finalizes");
  CHECK_EQ(invalidAck.nextChunk(1).length, 1UL,
           "invalid-ack fixture offers one byte");
  CHECK_TRUE(!invalidAck.acknowledge(2),
             "socket progress beyond offered bytes is rejected");
  CHECK_EQ(static_cast<int>(invalidAck.state()),
           static_cast<int>(ResponseState::ABORTED),
           "invalid socket progress fails closed");
  CHECK_EQ(invalidAck.responseLength(), 0UL,
           "invalid socket progress clears transaction");

  BoundedHttpResponse missingOffer;
  missingOffer.begin();
  (void)missingOffer.append(reinterpret_cast<const uint8_t*>("abc"), 3);
  CHECK_TRUE(missingOffer.finalize(1), "missing-offer fixture finalizes");
  CHECK_TRUE(!missingOffer.acknowledge(1),
             "socket progress without an outstanding offer is rejected");
  CHECK_EQ(static_cast<int>(missingOffer.state()),
           static_cast<int>(ResponseState::ABORTED),
           "acknowledgement without offer fails closed");

  BoundedHttpResponse disconnected;
  disconnected.begin();
  (void)disconnected.append(reinterpret_cast<const uint8_t*>(sensitive),
                            sizeof(sensitive) - 1);
  CHECK_TRUE(disconnected.finalize(1), "disconnect fixture finalizes");
  disconnected.abort();
  CHECK_EQ(static_cast<int>(disconnected.state()),
           static_cast<int>(ResponseState::ABORTED),
           "peer disconnect aborts transaction");
  CHECK_EQ(disconnected.responseLength(), 0UL,
           "peer disconnect clears sensitive response");
}

static void testCapacityWipeAndAllocationContract() {
  static_assert(!std::is_copy_constructible<BoundedHttpResponse>::value,
                "response transaction cannot copy sensitive bytes");
  static_assert(!std::is_copy_assignable<BoundedHttpResponse>::value,
                "response transaction cannot copy-assign sensitive bytes");
  static_assert(!std::is_trivially_destructible<BoundedHttpResponse>::value,
                "response destructor must wipe fixed storage");
  static_assert(sizeof(BoundedHttpResponse) <=
                  BoundedHttpResponse::kCapacity + 128,
                "response metadata has a small fixed overhead");

  std::vector<uint8_t> exact(BoundedHttpResponse::kCapacity, 'e');
  BoundedHttpResponse capacity;
  capacity.begin();
  CHECK_EQ(capacity.append(exact.data(), exact.size()), exact.size(),
           "exact response capacity accepted");
  CHECK_TRUE(capacity.finalize(1), "exact-capacity response finalizes");
  size_t acknowledged = 0;
  while (capacity.state() == ResponseState::SENDING) {
    const ResponseChunk chunk = capacity.nextChunk(5000);
    CHECK_TRUE(chunk.length <= BoundedHttpResponse::kSendBudget,
               "hard send budget caps oversized caller request");
    acknowledged += chunk.length;
    CHECK_TRUE(capacity.acknowledge(chunk.length),
               "exact-capacity response acknowledges every chunk");
  }
  CHECK_EQ(acknowledged, exact.size(),
           "exact-capacity response sends every byte once");

  BoundedHttpResponse progressiveWipe;
  progressiveWipe.begin();
  static const char progressive[] = "sensitive-prefix|remaining";
  (void)progressiveWipe.append(
    reinterpret_cast<const uint8_t*>(progressive), sizeof(progressive) - 1);
  CHECK_TRUE(progressiveWipe.finalize(1),
             "progressive wipe fixture finalizes");
  CHECK_EQ(progressiveWipe.nextChunk(16).length, 16UL,
           "progressive wipe offers exact sensitive prefix");
  CHECK_TRUE(progressiveWipe.acknowledge(16),
             "sent sensitive prefix acknowledged");
  CHECK_TRUE(!objectContains(&progressiveWipe, sizeof(progressiveWipe),
                             "sensitive-prefix"),
             "acknowledged prefix is wiped immediately");

  BoundedHttpResponse reset;
  reset.begin();
  static const char secret[] = "response-secret-42";
  (void)reset.append(reinterpret_cast<const uint8_t*>(secret),
                     sizeof(secret) - 1);
  CHECK_TRUE(objectContains(&reset, sizeof(reset), secret),
             "secret exists before reset");
  reset.begin();
  CHECK_TRUE(!objectContains(&reset, sizeof(reset), secret),
             "begin securely clears prior response");

  alignas(BoundedHttpResponse) uint8_t storage[sizeof(BoundedHttpResponse)];
  std::memset(storage, 0xa5, sizeof(storage));
  auto* placed = ::new (static_cast<void*>(storage)) BoundedHttpResponse();
  (void)placed->append(reinterpret_cast<const uint8_t*>(secret),
                       sizeof(secret) - 1);
  CHECK_TRUE(objectContains(storage, sizeof(storage), secret),
             "secret exists before response destruction");
  placed->~BoundedHttpResponse();
  CHECK_TRUE(!objectContains(storage, sizeof(storage), secret),
             "destructor clears response backing storage");

  BoundedHttpResponse noAlloc;
  noAlloc.begin();
  static const uint8_t fixed[] = "HTTP/1.1 204 No Content\r\n\r\n";
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    (void)noAlloc.append(fixed, sizeof(fixed) - 1);
    (void)noAlloc.finalize(10);
    const ResponseChunk chunk = noAlloc.nextChunk();
    (void)noAlloc.acknowledge(chunk.length);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "response lifecycle succeeds with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "response lifecycle performs no allocation attempt");
  CHECK_EQ(static_cast<int>(noAlloc.state()),
           static_cast<int>(ResponseState::COMPLETE),
           "allocation-free response reaches complete");
}

static void testOutstandingOfferIsIdempotentUntilAck() {
  BoundedHttpResponse response;
  response.begin();
  (void)response.append(reinterpret_cast<const uint8_t*>("abcdef"), 6);
  CHECK_TRUE(response.finalize(1), "idempotent-offer fixture finalizes");
  const ResponseChunk first = response.nextChunk(6);
  CHECK_EQ(first.length, 6UL, "first poll offers six bytes");
  const ResponseChunk smallerPoll = response.nextChunk(2);
  CHECK_TRUE(smallerPoll.data == first.data,
             "re-poll preserves outstanding pointer");
  CHECK_EQ(smallerPoll.length, first.length,
           "smaller re-poll cannot rewrite outstanding offer");
  const ResponseChunk zeroPoll = response.nextChunk(0);
  CHECK_TRUE(zeroPoll.data == first.data,
             "zero-budget re-poll preserves outstanding pointer");
  CHECK_EQ(zeroPoll.length, first.length,
           "zero-budget re-poll cannot cancel outstanding offer");
  CHECK_TRUE(response.acknowledge(6),
             "original transport completion remains valid after re-polls");
  CHECK_EQ(static_cast<int>(response.state()),
           static_cast<int>(ResponseState::COMPLETE),
           "idempotent offer completes without abort");
}

static bool validateEnvelope(const std::string& wire) {
  BoundedHttpResponse response;
  response.begin();
  (void)response.append(
    reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
  return response.validHttp1Envelope();
}

static void testHttpEnvelopeValidationFailsClosed() {
  const std::string valid =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=UTF-8\r\n"
    "Content-Length: 2\r\n"
    "Cache-Control: no-store, max-age=0\r\n"
    "Pragma: no-cache\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";
  CHECK_TRUE(validateEnvelope(valid),
             "complete bounded HTTP/1.1 response envelope is accepted");

  std::string missingNoStore = valid;
  const std::string cacheLine =
    "Cache-Control: no-store, max-age=0\r\n";
  missingNoStore.erase(missingNoStore.find(cacheLine), cacheLine.size());
  CHECK_TRUE(!validateEnvelope(missingNoStore),
             "missing mandatory no-store header is rejected");

  std::string duplicateLength = valid;
  duplicateLength.insert(duplicateLength.find("Content-Length:"),
                         "Content-Length: 2\r\n");
  CHECK_TRUE(!validateEnvelope(duplicateLength),
             "duplicate Content-Length is rejected");

  std::string wrongLength = valid;
  wrongLength.replace(wrongLength.find("Content-Length: 2"),
                      std::strlen("Content-Length: 2"),
                      "Content-Length: 3");
  CHECK_TRUE(!validateEnvelope(wrongLength),
             "body length mismatch is rejected");

  std::string chunked = valid;
  chunked.insert(chunked.find("Connection:"),
                 "Transfer-Encoding: chunked\r\n");
  CHECK_TRUE(!validateEnvelope(chunked),
             "capture-bypassing transfer encoding is rejected");

  std::string missingConnection = valid;
  const std::string connectionLine = "Connection: close\r\n";
  missingConnection.erase(missingConnection.find(connectionLine),
                          connectionLine.size());
  CHECK_TRUE(!validateEnvelope(missingConnection),
             "missing close contract is rejected");

  std::string truncated = valid;
  truncated.erase(truncated.find("\r\n\r\n") + 2);
  CHECK_TRUE(!validateEnvelope(truncated),
             "truncated header terminator is rejected");

  std::string malformedStatus = valid;
  malformedStatus.replace(9, 3, "2O0");
  CHECK_TRUE(!validateEnvelope(malformedStatus),
             "malformed status code is rejected");

  BoundedHttpResponse noAlloc;
  noAlloc.begin();
  (void)noAlloc.append(reinterpret_cast<const uint8_t*>(valid.data()),
                       valid.size());
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    CHECK_TRUE(noAlloc.validHttp1Envelope(),
               "envelope validation succeeds with allocation denied");
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "envelope validation never allocates");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "envelope validation makes no allocation attempt");
}

int main() {
  testTransactionalResponseSupportsPartialSends();
  testOverflowAtomicallyBecomesFixed503();
  testSlowReaderDeadlineAndFailureCleanup();
  testCapacityWipeAndAllocationContract();
  testOutstandingOfferIsIdempotentUntilAck();
  testHttpEnvelopeValidationFailsClosed();
  return testReport();
}
