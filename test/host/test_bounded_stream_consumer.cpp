#include <cstdlib>
#include <cstring>
#include <new>

#include "../../lib/BoundedStreamConsumer.h"
#include "test_support.h"

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
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static int code(StreamConsumerResult result) {
  return static_cast<int>(result);
}

struct Fixture {
  bool beginOk = true;
  bool writeOk = true;
  bool finishOk = true;
  bool throwBegin = false;
  bool throwWrite = false;
  bool throwFinish = false;
  bool throwAbort = false;
  int begins = 0;
  int writes = 0;
  int finishes = 0;
  int aborts = 0;
  uint32_t expected = 0;
  size_t received = 0;
  uint8_t bytes[512] = {};
};

static bool begin(void* context, uint32_t expected) {
  Fixture* fixture = static_cast<Fixture*>(context);
  fixture->begins++;
  fixture->expected = expected;
  if (fixture->throwBegin) throw 1;
  return fixture->beginOk;
}

static bool write(void* context, const uint8_t* bytes, size_t length) {
  Fixture* fixture = static_cast<Fixture*>(context);
  fixture->writes++;
  if (fixture->throwWrite) throw 2;
  if (!fixture->writeOk || fixture->received + length > sizeof(fixture->bytes)) {
    return false;
  }
  memcpy(fixture->bytes + fixture->received, bytes, length);
  fixture->received += length;
  return true;
}

static bool finish(void* context) {
  Fixture* fixture = static_cast<Fixture*>(context);
  fixture->finishes++;
  if (fixture->throwFinish) throw 3;
  return fixture->finishOk;
}

static void abortStream(void* context) {
  Fixture* fixture = static_cast<Fixture*>(context);
  fixture->aborts++;
  if (fixture->throwAbort) throw 4;
}

static StreamConsumerCallbacks callbacks(Fixture& fixture) {
  return {&fixture, begin, write, finish, abortStream};
}

static void testSuccessfulLifecycleAndReuse() {
  Fixture fixture;
  BoundedStreamConsumer consumer;
  CHECK_EQ(code(consumer.start(4, callbacks(fixture))),
           code(StreamConsumerResult::OK), "valid consumer begins");
  CHECK_TRUE(consumer.active() && fixture.begins == 1 && fixture.expected == 4,
             "begin owns the exact declared length");
  const uint8_t first[] = {1, 2};
  const uint8_t second[] = {3, 4};
  CHECK_EQ(code(consumer.write(first, sizeof(first))),
           code(StreamConsumerResult::OK), "first chunk writes once");
  CHECK_EQ(code(consumer.write(second, sizeof(second))),
           code(StreamConsumerResult::OK), "second chunk writes once");
  CHECK_EQ(consumer.receivedLength(), 4U, "received length is exact");
  CHECK_EQ(code(consumer.finish()), code(StreamConsumerResult::OK),
           "exact stream finishes");
  CHECK_EQ(static_cast<int>(consumer.state()),
           static_cast<int>(StreamConsumerState::COMPLETE),
           "successful stream reaches complete");
  CHECK_TRUE(fixture.finishes == 1 && fixture.aborts == 0,
             "successful stream never aborts");
  CHECK_TRUE(memcmp(fixture.bytes, "\x01\x02\x03\x04", 4) == 0,
             "consumer preserves chunk order");
  consumer.cancel();
  CHECK_EQ(fixture.aborts, 0, "cancel after success remains non-destructive");

  Fixture reused;
  CHECK_EQ(code(consumer.start(1, callbacks(reused))),
           code(StreamConsumerResult::OK), "completed object may serve next request");
  CHECK_EQ(code(consumer.write(first, 1)), code(StreamConsumerResult::OK),
           "reused object writes new stream");
  CHECK_EQ(code(consumer.finish()), code(StreamConsumerResult::OK),
           "reused object completes independently");
}

static void testInvalidConfigurationAndBeginFailure() {
  Fixture fixture;
  BoundedStreamConsumer consumer;
  CHECK_EQ(code(consumer.start(0, callbacks(fixture))),
           code(StreamConsumerResult::INVALID_LENGTH),
           "zero-length stream cannot begin");
  CHECK_EQ(fixture.begins, 0, "invalid length invokes no external callback");
  StreamConsumerCallbacks incomplete{};
  CHECK_EQ(code(consumer.start(1, incomplete)),
           code(StreamConsumerResult::INVALID_CALLBACKS),
           "missing callback set fails closed");

  fixture.beginOk = false;
  CHECK_EQ(code(consumer.start(1, callbacks(fixture))),
           code(StreamConsumerResult::BEGIN_FAILED),
           "sink begin failure propagates");
  CHECK_TRUE(fixture.begins == 1 && fixture.aborts == 1,
             "partially initialized begin aborts exactly once");
  consumer.cancel();
  CHECK_EQ(fixture.aborts, 1, "cancel cannot double-abort failed begin");

  Fixture throwing;
  throwing.throwBegin = true;
  throwing.throwAbort = true;
  CHECK_EQ(code(consumer.start(1, callbacks(throwing))),
           code(StreamConsumerResult::BEGIN_FAILED),
           "begin exception becomes a failure result");
  CHECK_EQ(throwing.aborts, 1,
           "throwing abort still transfers ownership exactly once");
  CHECK_EQ(static_cast<int>(consumer.state()),
           static_cast<int>(StreamConsumerState::FAILED),
           "callback exceptions leave consumer failed closed");
}

static void testWriteBoundsFailuresAndIdempotentAbort() {
  const uint8_t bytes[257] = {};
  for (int scenario = 0; scenario < 5; ++scenario) {
    Fixture fixture;
    BoundedStreamConsumer consumer;
    CHECK_EQ(code(consumer.start(4, callbacks(fixture))),
             code(StreamConsumerResult::OK), "write-failure fixture begins");
    StreamConsumerResult result = StreamConsumerResult::OK;
    if (scenario == 0) result = consumer.write(nullptr, 1);
    if (scenario == 1) result = consumer.write(bytes, 0);
    if (scenario == 2) result = consumer.write(bytes, sizeof(bytes));
    if (scenario == 3) result = consumer.write(bytes, 5);
    if (scenario == 4) {
      fixture.writeOk = false;
      result = consumer.write(bytes, 1);
    }
    CHECK_EQ(code(result), code(scenario == 4
               ? StreamConsumerResult::WRITE_FAILED
               : StreamConsumerResult::INVALID_LENGTH),
             "null empty oversized or failed chunk rejects");
    CHECK_EQ(fixture.aborts, 1, "write failure aborts exactly once");
    consumer.cancel();
    consumer.reset();
    CHECK_EQ(fixture.aborts, 1, "cancel and reset cannot repeat abort");
    CHECK_EQ(code(consumer.write(bytes, 1)),
             code(StreamConsumerResult::INVALID_STATE),
             "writes after failure/reset are rejected");
  }

  Fixture throwing;
  throwing.throwWrite = true;
  BoundedStreamConsumer consumer;
  CHECK_EQ(code(consumer.start(1, callbacks(throwing))),
           code(StreamConsumerResult::OK), "throwing write fixture begins");
  CHECK_EQ(code(consumer.write(bytes, 1)),
           code(StreamConsumerResult::WRITE_FAILED),
           "write exception fails closed");
  CHECK_EQ(throwing.aborts, 1, "write exception aborts exactly once");
}

static void testFinishFailuresActiveRestartAndDestructor() {
  const uint8_t bytes[] = {1, 2};
  Fixture shortFixture;
  BoundedStreamConsumer shortConsumer;
  CHECK_EQ(code(shortConsumer.start(2, callbacks(shortFixture))),
           code(StreamConsumerResult::OK), "short fixture begins");
  CHECK_EQ(code(shortConsumer.write(bytes, 1)), code(StreamConsumerResult::OK),
           "short fixture writes partial body");
  CHECK_EQ(code(shortConsumer.finish()),
           code(StreamConsumerResult::INVALID_LENGTH),
           "underflow cannot finalize");
  CHECK_EQ(shortFixture.aborts, 1, "underflow aborts exactly once");

  Fixture failedFinish;
  failedFinish.finishOk = false;
  BoundedStreamConsumer finishConsumer;
  CHECK_EQ(code(finishConsumer.start(1, callbacks(failedFinish))),
           code(StreamConsumerResult::OK), "finish-failure fixture begins");
  CHECK_EQ(code(finishConsumer.write(bytes, 1)), code(StreamConsumerResult::OK),
           "finish-failure fixture writes exact body");
  CHECK_EQ(code(finishConsumer.finish()),
           code(StreamConsumerResult::FINISH_FAILED),
           "sink finish failure propagates");
  CHECK_EQ(failedFinish.aborts, 1, "finish failure aborts exactly once");

  Fixture active;
  BoundedStreamConsumer activeConsumer;
  CHECK_EQ(code(activeConsumer.start(2, callbacks(active))),
           code(StreamConsumerResult::OK), "active restart fixture begins");
  Fixture replacement;
  CHECK_EQ(code(activeConsumer.start(1, callbacks(replacement))),
           code(StreamConsumerResult::INVALID_STATE),
           "active stream cannot be replaced without cleanup");
  CHECK_TRUE(activeConsumer.active() && replacement.begins == 0,
             "failed replacement preserves original ownership");
  activeConsumer.cancel();
  CHECK_EQ(active.aborts, 1, "explicit cancel aborts active owner once");

  Fixture abandoned;
  {
    BoundedStreamConsumer scoped;
    CHECK_EQ(code(scoped.start(2, callbacks(abandoned))),
             code(StreamConsumerResult::OK), "destructor fixture begins");
    CHECK_EQ(code(scoped.write(bytes, 1)), code(StreamConsumerResult::OK),
             "destructor fixture holds partial stream");
  }
  CHECK_EQ(abandoned.aborts, 1,
           "destructor aborts an abandoned stream exactly once");
}

static void testNoAllocation() {
  Fixture fixture;
  BoundedStreamConsumer consumer;
  const uint8_t byte = 1;
  bool threw = false;
  gDeniedAllocationCalls = 0;
  gDenyAllocations = true;
  try {
    (void)consumer.start(1, callbacks(fixture));
    (void)consumer.write(&byte, 1);
    (void)consumer.finish();
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  gDenyAllocations = false;
  CHECK_TRUE(!threw, "consumer lifecycle runs with allocation denied");
  CHECK_EQ(gDeniedAllocationCalls, 0UL,
           "consumer lifecycle makes no dynamic allocation attempt");
}

int main() {
  testSuccessfulLifecycleAndReuse();
  testInvalidConfigurationAndBeginFailure();
  testWriteBoundsFailuresAndIdempotentAbort();
  testFinishFailuresActiveRestartAndDestructor();
  testNoAllocation();
  return testReport();
}
