#include <Arduino.h>

#include "lib/BoundedHttpRequest.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

static void feedAll(BoundedHttpRequest& request, const std::string& input,
                    uint32_t now = 100) {
  size_t offset = 0;
  while (offset < input.size() &&
         request.state() != RequestState::WAIT_POLICY &&
         request.state() != RequestState::REJECT) {
    const ConsumeResult result = request.consume(
      reinterpret_cast<const uint8_t*>(input.data() + offset),
      input.size() - offset, now);
    CHECK_TRUE(result.consumed > 0, "active parser always makes progress");
    if (result.consumed == 0) break;
    offset += result.consumed;
  }
}

static RequestState parseState(const std::string& input) {
  BoundedHttpRequest request;
  request.reset(100);
  feedAll(request, input);
  return request.state();
}

static RequestError parseError(const std::string& input) {
  BoundedHttpRequest request;
  request.reset(100);
  feedAll(request, input);
  return request.error();
}

static void testValidGetStopsBeforeBody() {
  BoundedHttpRequest request;
  request.reset(100);

  static const char input[] =
    "GET / HTTP/1.1\r\n"
    "Host: bp.local\r\n"
    "\r\n"
    "BODY";
  const size_t headerBytes = sizeof(input) - 1 - 4;
  const ConsumeResult result = request.consume(
    reinterpret_cast<const uint8_t*>(input), sizeof(input) - 1, 100);

  CHECK_EQ(static_cast<int>(result.state),
           static_cast<int>(RequestState::WAIT_POLICY),
           "complete valid request waits for policy");
  CHECK_EQ(result.consumed, headerBytes,
           "parser stops before same-packet body bytes");
  CHECK_EQ(static_cast<int>(request.view().method),
           static_cast<int>(RequestMethod::GET), "method captured");
  CHECK_STR(request.view().path, "/", "path captured");
  CHECK_STR(request.view().host, "bp.local", "fresh Host captured");
}

static void testPostAndQueryAreSeparated() {
  BoundedHttpRequest request;
  request.reset(200);
  static const char input[] =
    "POST /claim?mode=physical HTTP/1.1\r\n"
    "Host: 192.168.4.1\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
  const ConsumeResult result = request.consume(
    reinterpret_cast<const uint8_t*>(input), sizeof(input) - 1, 200);

  CHECK_EQ(static_cast<int>(result.state),
           static_cast<int>(RequestState::WAIT_POLICY),
           "valid POST waits for policy");
  CHECK_EQ(static_cast<int>(request.view().method),
           static_cast<int>(RequestMethod::POST), "POST method captured");
  CHECK_STR(request.view().path, "/claim", "query removed from path");
  CHECK_STR(request.view().query, "mode=physical", "raw query captured");
  CHECK_TRUE(request.view().hasContentLength,
             "Content-Length presence captured");
  CHECK_EQ(request.view().contentLength, 0U,
           "zero Content-Length is exact");
}

static void testStrictRequestLineAndLimits() {
  const std::string headers = "\r\nHost: bp.local\r\n\r\n";
  CHECK_EQ(static_cast<int>(parseState("PUT / HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "unsupported method rejected");
  CHECK_EQ(static_cast<int>(parseState("get / HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "method is case-sensitive");
  CHECK_EQ(static_cast<int>(parseState("GET / HTTP/1.0" + headers)),
           static_cast<int>(RequestState::REJECT),
           "HTTP/1.0 rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET http://bp.local/ HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "absolute-form target rejected");
  CHECK_EQ(static_cast<int>(parseState("GET * HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "asterisk-form target rejected");
  CHECK_EQ(static_cast<int>(parseState("GET /bad#fragment HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "URI fragment rejected");
  CHECK_EQ(static_cast<int>(parseState(
             std::string("GET /bad") + static_cast<char>(0x1f) +
             "path HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "request-line control byte rejected");
  CHECK_EQ(static_cast<int>(parseState(
             std::string("GET /bad") + static_cast<char>(0x7f) +
             "path HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "request-line DEL rejected");

  const std::string maxPath = "/" + std::string(242, 'a');
  const std::string maxLine = "GET " + maxPath + " HTTP/1.1";
  CHECK_EQ(maxLine.size(), BoundedHttpRequest::kRequestLineLimit,
           "request-line exact fixture");
  CHECK_EQ(static_cast<int>(parseState(maxLine + headers)),
           static_cast<int>(RequestState::WAIT_POLICY),
           "exact request-line limit accepted");
  CHECK_EQ(static_cast<int>(parseState(
             "GET /" + std::string(243, 'a') + " HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "request-line limit plus one rejected");

  const std::string maxQuery(241, 'q');
  CHECK_EQ(static_cast<int>(parseState(
             "GET /?" + maxQuery + " HTTP/1.1" + headers)),
           static_cast<int>(RequestState::WAIT_POLICY),
           "maximum query reachable under line cap accepted");
  CHECK_EQ(static_cast<int>(parseState(
             "GET /?" + std::string(242, 'q') + " HTTP/1.1" + headers)),
           static_cast<int>(RequestState::REJECT),
           "reachable query plus one rejected by line cap");
}

static void testSensitiveHeadersAndMalformedLines() {
  BoundedHttpRequest request;
  request.reset(300);
  const std::string valid =
    "POST /configure HTTP/1.1\r\n"
    "hOsT:  bp.local:80  \r\n"
    "aUtHoRiZaTiOn: Basic YWRtaW46eA==\r\n"
    "Origin: http://bp.local:80\r\n"
    "Referer: http://bp.local/config\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "content-length: 123\r\n"
    "X-Ignored: bounded\r\n"
    "\r\n";
  feedAll(request, valid, 300);
  CHECK_EQ(static_cast<int>(request.state()),
           static_cast<int>(RequestState::WAIT_POLICY),
           "case-insensitive valid headers accepted");
  CHECK_STR(request.view().host, "bp.local:80", "Host OWS trimmed");
  CHECK_STR(request.view().authorization, "Basic YWRtaW46eA==",
            "Authorization captured");
  CHECK_STR(request.view().origin, "http://bp.local:80",
            "Origin captured");
  CHECK_STR(request.view().referer, "http://bp.local/config",
            "Referer captured");
  CHECK_STR(request.view().contentType,
            "application/x-www-form-urlencoded",
            "Content-Type captured");
  CHECK_EQ(request.view().contentLength, 123U,
           "strict Content-Length captured");

  const char* duplicateNames[] = {
    "Host", "Content-Length", "Authorization", "Origin", "Referer",
    "Content-Type",
  };
  for (const char* name : duplicateNames) {
    std::string firstValue = "one";
    std::string secondValue = "two";
    if (std::strcmp(name, "Host") == 0) {
      firstValue = "bp.local";
      secondValue = "bp.local";
    } else if (std::strcmp(name, "Content-Length") == 0) {
      firstValue = "0";
      secondValue = "0";
    }
    const std::string input =
      std::string("POST /claim HTTP/1.1\r\nHost: bp.local\r\n") +
      name + ": " + firstValue + "\r\n" +
      name + ": " + secondValue + "\r\n\r\n";
    CHECK_EQ(static_cast<int>(parseState(input)),
             static_cast<int>(RequestState::REJECT),
             "duplicate sensitive header rejected");
  }

  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nNoColon\r\nHost: bp.local\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "header without colon rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\n Host: bp.local\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "obs-fold or leading whitespace rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost: bp.local\r\nTransfer-Encoding: chunked\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "Transfer-Encoding rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "POST /claim HTTP/1.1\r\nHost: bp.local\r\nExpect: 100-continue\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "Expect rejected");
  CHECK_EQ(static_cast<int>(parseState(
             std::string("GET / HTTP/1.1\r\nHost: bp.local\r\nX-Test: ok") +
             static_cast<char>(0x01) + "bad\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "header value control byte rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost:\tbp.local\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "leading HTAB in header value rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost: bp\t.local\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "embedded HTAB in header value rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost: bp.local\t\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "trailing HTAB in header value rejected");
}

static void testContentLengthAndFreshHost() {
  const char* invalidLengths[] = {
    "", "-1", "+1", "1x", "1,2", "1 2", "4294967296",
  };
  for (const char* value : invalidLengths) {
    const std::string input =
      std::string("POST /claim HTTP/1.1\r\nHost: bp.local\r\n") +
      "Content-Length: " + value + "\r\n\r\n";
    CHECK_EQ(static_cast<int>(parseState(input)),
             static_cast<int>(RequestState::REJECT),
             "malformed Content-Length rejected");
  }
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost: bp.local\r\nContent-Length: 1\r\n\r\nX")),
           static_cast<int>(RequestState::REJECT),
           "GET request body rejected before body read");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nUser-Agent: test\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "missing Host rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\r\nHost:  \t\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "empty Host rejected");

  BoundedHttpRequest reused;
  reused.reset(1);
  feedAll(reused, "GET / HTTP/1.1\r\nHost: first.local\r\n\r\n", 1);
  CHECK_EQ(static_cast<int>(reused.state()),
           static_cast<int>(RequestState::WAIT_POLICY),
           "first request has valid Host");
  CHECK_STR(reused.view().host, "first.local", "first Host visible");
  reused.reset(2);
  CHECK_STR(reused.view().host, "", "reset clears prior Host immediately");
  feedAll(reused, "GET /next HTTP/1.1\r\nUser-Agent: test\r\n\r\n", 2);
  CHECK_EQ(static_cast<int>(reused.state()),
           static_cast<int>(RequestState::REJECT),
           "Host-less second request cannot inherit prior Host");
  CHECK_STR(reused.view().host, "", "rejected second request has empty Host");
}

static void testHeaderLineTotalAndCountLimits() {
  const std::string exactLine =
    "GET / HTTP/1.1\r\nHost: bp.local\r\nX:" +
    std::string(510, 'a') + "\r\n\r\n";
  CHECK_EQ(static_cast<int>(parseState(exactLine)),
           static_cast<int>(RequestState::WAIT_POLICY),
           "exact single-header line limit accepted");
  const std::string overLine =
    "GET / HTTP/1.1\r\nHost: bp.local\r\nX:" +
    std::string(511, 'a') + "\r\n\r\n";
  CHECK_EQ(static_cast<int>(parseState(overLine)),
           static_cast<int>(RequestState::REJECT),
           "single-header line limit plus one rejected");

  std::string exactTotal = "GET / HTTP/1.1\r\nHost: bp.local\r\n";
  for (int i = 0; i < 3; ++i) {
    exactTotal += "X:" + std::string(510, static_cast<char>('a' + i)) +
                  "\r\n";
  }
  exactTotal += "Y:" + std::string(484, 'z') + "\r\n\r\n";
  const size_t requestLineBytes = std::strlen("GET / HTTP/1.1\r\n");
  CHECK_EQ(exactTotal.size() - requestLineBytes, 2048UL,
           "exact total-header fixture includes CRLF bytes");
  CHECK_EQ(static_cast<int>(parseState(exactTotal)),
           static_cast<int>(RequestState::WAIT_POLICY),
           "exact total-header byte limit accepted");
  std::string overTotal = exactTotal;
  const size_t finalHeader = overTotal.rfind("Y:");
  overTotal.insert(finalHeader + 2, 1, 'x');
  CHECK_EQ(overTotal.size() - requestLineBytes, 2049UL,
           "over-total fixture is one byte larger");
  CHECK_EQ(static_cast<int>(parseState(overTotal)),
           static_cast<int>(RequestState::REJECT),
           "total-header byte limit plus one rejected");

  std::string exactCount = "GET / HTTP/1.1\r\nHost: bp.local\r\n";
  for (int i = 0; i < 23; ++i) exactCount += "X: y\r\n";
  exactCount += "\r\n";
  CHECK_EQ(static_cast<int>(parseState(exactCount)),
           static_cast<int>(RequestState::WAIT_POLICY),
           "exact header-count limit accepted");
  std::string overCount = "GET / HTTP/1.1\r\nHost: bp.local\r\n";
  for (int i = 0; i < 24; ++i) overCount += "X: y\r\n";
  overCount += "\r\n";
  CHECK_EQ(static_cast<int>(parseState(overCount)),
           static_cast<int>(RequestState::REJECT),
           "header-count limit plus one rejected");
}

static void testFragmentationBudgetAndStrictCrlf() {
  const std::string input =
    "POST /claim?x=1 HTTP/1.1\r\n"
    "Host: bp.local\r\n"
    "Authorization: Basic YQ==\r\n"
    "Content-Length: 0\r\n"
    "\r\nBODY";
  const size_t headerBytes = input.size() - 4;
  for (size_t cut = 0; cut <= input.size(); ++cut) {
    BoundedHttpRequest request;
    request.reset(10);
    size_t totalConsumed = 0;
    if (cut != 0) {
      const ConsumeResult first = request.consume(
        reinterpret_cast<const uint8_t*>(input.data()), cut, 10);
      totalConsumed += first.consumed;
    }
    if (request.state() != RequestState::WAIT_POLICY &&
        request.state() != RequestState::REJECT) {
      const ConsumeResult second = request.consume(
        reinterpret_cast<const uint8_t*>(input.data() + cut),
        input.size() - cut, 10);
      totalConsumed += second.consumed;
    }
    CHECK_EQ(static_cast<int>(request.state()),
             static_cast<int>(RequestState::WAIT_POLICY),
             "every request fragmentation cut succeeds");
    CHECK_EQ(totalConsumed, headerBytes,
             "fragmentation never consumes pipelined body");
  }

  BoundedHttpRequest budgeted;
  budgeted.reset(20);
  const std::string huge(1000, 'A');
  ConsumeResult limited = budgeted.consume(
    reinterpret_cast<const uint8_t*>(huge.data()), huge.size(), 20, 1000);
  CHECK_EQ(limited.consumed, BoundedHttpRequest::kByteBudget,
           "caller cannot exceed hard per-tick byte budget");
  CHECK_EQ(static_cast<int>(limited.state),
           static_cast<int>(RequestState::REQUEST_LINE),
           "exact line cap remains pending until another byte or CRLF");

  BoundedHttpRequest customBudget;
  customBudget.reset(20);
  limited = customBudget.consume(
    reinterpret_cast<const uint8_t*>(huge.data()), huge.size(), 20, 17);
  CHECK_EQ(limited.consumed, 17UL, "smaller caller budget is honored");

  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\nHost: bp.local\n\n")),
           static_cast<int>(RequestState::REJECT), "bare LF rejected");
  CHECK_EQ(static_cast<int>(parseState(
             "GET / HTTP/1.1\rXHost: bp.local\r\n\r\n")),
           static_cast<int>(RequestState::REJECT),
           "CR not followed by LF rejected");
  std::string nulHeader = "GET / HTTP/1.1\r\nHost: bp.local\r\nX: a";
  nulHeader.push_back('\0');
  nulHeader += "b\r\n\r\n";
  CHECK_EQ(static_cast<int>(parseState(nulHeader)),
           static_cast<int>(RequestState::REJECT), "embedded NUL rejected");
}

static void testAbsoluteDeadlineAndClockWrap() {
  BoundedHttpRequest slow;
  slow.reset(1000);
  const uint8_t byte = 'G';
  CHECK_EQ(slow.consume(&byte, 1, 1000).consumed, 1UL,
           "first slow byte accepted");
  CHECK_EQ(slow.consume(&byte, 1, 2499).consumed, 1UL,
           "progress before absolute deadline accepted");
  const ConsumeResult timedOut = slow.consume(&byte, 1, 2500);
  CHECK_EQ(timedOut.consumed, 0UL,
           "deadline rejects before consuming another byte");
  CHECK_EQ(static_cast<int>(timedOut.state),
           static_cast<int>(RequestState::REJECT),
           "slowloris rejected at absolute deadline despite progress");
  CHECK_EQ(static_cast<int>(slow.error()),
           static_cast<int>(RequestError::TIMEOUT),
           "deadline exposes stable timeout reason");

  const uint32_t start = std::numeric_limits<uint32_t>::max() - 749U;
  BoundedHttpRequest wrapped;
  wrapped.reset(start);
  CHECK_EQ(wrapped.consume(&byte, 1, start + 1499U).consumed, 1UL,
           "wrapped clock accepts byte before deadline");
  const ConsumeResult wrappedTimeout =
    wrapped.consume(&byte, 1, start + 1500U);
  CHECK_EQ(static_cast<int>(wrappedTimeout.state),
           static_cast<int>(RequestState::REJECT),
           "wrapped clock rejects exactly at deadline");
  CHECK_EQ(static_cast<int>(wrapped.error()),
           static_cast<int>(RequestError::TIMEOUT),
           "wrapped deadline preserves timeout reason");
}

static void testStableErrorClassification() {
  const std::string headers = "\r\nHost: bp.local\r\n\r\n";
  CHECK_EQ(static_cast<int>(parseError("PUT / HTTP/1.1" + headers)),
           static_cast<int>(RequestError::METHOD_NOT_ALLOWED),
           "unsupported method maps to 405-class error");
  CHECK_EQ(static_cast<int>(parseError("GET / HTTP/1.0" + headers)),
           static_cast<int>(RequestError::VERSION_NOT_SUPPORTED),
           "unsupported HTTP version maps to 505-class error");
  CHECK_EQ(static_cast<int>(parseError(
             "GET /" + std::string(243, 'a') + " HTTP/1.1" + headers)),
           static_cast<int>(RequestError::REQUEST_LINE_TOO_LONG),
           "overlong request line maps to 414-class error");
  CHECK_EQ(static_cast<int>(parseError(
             "GET / HTTP/1.1\r\nHost: bp.local\r\nX:" +
             std::string(511, 'a') + "\r\n\r\n")),
           static_cast<int>(RequestError::HEADER_FIELDS_TOO_LARGE),
           "overlong header maps to 431-class error");
  CHECK_EQ(static_cast<int>(parseError(
             "POST /claim HTTP/1.1\r\nHost: bp.local\r\nContent-Length: -1\r\n\r\n")),
           static_cast<int>(RequestError::INVALID_CONTENT_LENGTH),
           "invalid Content-Length has stable error");
  CHECK_EQ(static_cast<int>(parseError(
             "POST /claim HTTP/1.1\r\nHost: bp.local\r\nTransfer-Encoding: chunked\r\n\r\n")),
           static_cast<int>(RequestError::TRANSFER_ENCODING_NOT_SUPPORTED),
           "Transfer-Encoding maps to 501-class error");
  CHECK_EQ(static_cast<int>(parseError(
             "POST /claim HTTP/1.1\r\nHost: bp.local\r\nExpect: 100-continue\r\n\r\n")),
           static_cast<int>(RequestError::EXPECTATION_FAILED),
           "Expect maps to 417-class error");

  CHECK_EQ(httpStatusForError(RequestError::BAD_REQUEST), 400,
           "bad request status mapping");
  CHECK_EQ(httpStatusForError(RequestError::MISSING_HOST), 400,
           "missing Host status mapping");
  CHECK_EQ(httpStatusForError(RequestError::INVALID_CONTENT_LENGTH), 400,
           "invalid Content-Length status mapping");
  CHECK_EQ(httpStatusForError(RequestError::TIMEOUT), 408,
           "timeout status mapping");
  CHECK_EQ(httpStatusForError(RequestError::METHOD_NOT_ALLOWED), 405,
           "method status mapping");
  CHECK_EQ(httpStatusForError(RequestError::EXPECTATION_FAILED), 417,
           "Expect status mapping");
  CHECK_EQ(httpStatusForError(RequestError::REQUEST_LINE_TOO_LONG), 414,
           "request-line status mapping");
  CHECK_EQ(httpStatusForError(RequestError::HEADER_FIELDS_TOO_LARGE), 431,
           "header-limit status mapping");
  CHECK_EQ(httpStatusForError(RequestError::TRANSFER_ENCODING_NOT_SUPPORTED),
           501, "Transfer-Encoding status mapping");
  CHECK_EQ(httpStatusForError(RequestError::VERSION_NOT_SUPPORTED), 505,
           "version status mapping");
  CHECK_EQ(httpStatusForError(RequestError::NONE), 0,
           "non-error has no HTTP status");
}

static void testNoAllocationAndDeterministicFuzz() {
  static_assert(std::is_trivially_destructible<BoundedHttpRequest>::value,
                "request reducer owns no heap resource");
  static_assert(sizeof(BoundedHttpRequest) <= 4096,
                "request reducer has a bounded stack/static footprint");

  BoundedHttpRequest request;
  static const uint8_t valid[] =
    "GET / HTTP/1.1\r\nHost: bp.local\r\n\r\n";
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    request.reset(10);
    (void)request.consume(valid, sizeof(valid) - 1, 10);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "reset and header parsing succeed with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "header reducer performs no allocation attempt");
  CHECK_EQ(static_cast<int>(request.state()),
           static_cast<int>(RequestState::WAIT_POLICY),
           "allocation-free valid parse reaches policy gate");

  uint32_t random = 0x8f3a21c7U;
  uint8_t bytes[600] = {};
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    auto nextRandom = [&random]() {
      random ^= random << 13U;
      random ^= random >> 17U;
      random ^= random << 5U;
      return random;
    };
    const size_t length = nextRandom() % (sizeof(bytes) + 1);
    for (size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<uint8_t>(nextRandom() & 0xffU);
    }
    const size_t budget = nextRandom() % 400U;
    const uint32_t now = nextRandom();
    request.reset(now);
    const ConsumeResult result = request.consume(
      bytes, length, now, budget);
    size_t expectedMaximum = budget;
    if (expectedMaximum > BoundedHttpRequest::kByteBudget) {
      expectedMaximum = BoundedHttpRequest::kByteBudget;
    }
    if (expectedMaximum > length) expectedMaximum = length;
    CHECK_TRUE(result.consumed <= expectedMaximum,
               "fuzz input never exceeds caller or hard byte budget");
    CHECK_TRUE(result.state == RequestState::REQUEST_LINE ||
                 result.state == RequestState::HEADERS ||
                 result.state == RequestState::WAIT_POLICY ||
                 result.state == RequestState::REJECT,
               "fuzz input leaves reducer in a declared state");
  }
}

int main() {
  testValidGetStopsBeforeBody();
  testPostAndQueryAreSeparated();
  testStrictRequestLineAndLimits();
  testSensitiveHeadersAndMalformedLines();
  testContentLengthAndFreshHost();
  testHeaderLineTotalAndCountLimits();
  testFragmentationBudgetAndStrictCrlf();
  testAbsoluteDeadlineAndClockWrap();
  testStableErrorClassification();
  testNoAllocationAndDeterministicFuzz();
  return testReport();
}
