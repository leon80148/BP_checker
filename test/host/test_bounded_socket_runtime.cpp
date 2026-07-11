#include "lib/BoundedSocketRuntime.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace bp_web;

struct FakeSocket {
  uint32_t now = 0;
  size_t clockCalls = 0;
  size_t readCalls = 0;
  size_t writeCalls = 0;
  size_t shutdownCalls = 0;
  size_t lastReadBudget = 0;
  size_t lastWriteLength = 0;
  const char* readBytes = nullptr;
  size_t readLength = 0;
  SocketReadStatus nextRead = SocketReadStatus::WOULD_BLOCK;
  SocketWriteStatus nextWrite = SocketWriteStatus::WOULD_BLOCK;
  size_t writeLength = 0;
  SocketShutdownStatus nextShutdown = SocketShutdownStatus::COMPLETE;
};

static uint32_t fakeClock(void* context) {
  auto* fake = static_cast<FakeSocket*>(context);
  ++fake->clockCalls;
  return fake->now;
}

static SocketReadResult fakeRead(void* context, int, uint8_t* target,
                                 size_t capacity) {
  auto* fake = static_cast<FakeSocket*>(context);
  ++fake->readCalls;
  fake->lastReadBudget = capacity;
  if (fake->nextRead != SocketReadStatus::DATA) {
    return {fake->nextRead, 0};
  }
  if (fake->readBytes != nullptr && fake->readLength <= capacity) {
    std::memcpy(target, fake->readBytes, fake->readLength);
  }
  return {SocketReadStatus::DATA, fake->readLength};
}

static SocketWriteResult fakeWrite(void* context, int,
                                   const uint8_t*, size_t length) {
  auto* fake = static_cast<FakeSocket*>(context);
  ++fake->writeCalls;
  fake->lastWriteLength = length;
  return {fake->nextWrite, fake->writeLength};
}

static SocketShutdownStatus fakeShutdown(void* context, int) {
  auto* fake = static_cast<FakeSocket*>(context);
  ++fake->shutdownCalls;
  return fake->nextShutdown;
}

static BoundedSocketRuntime makeRuntime(FakeSocket& fake) {
  return BoundedSocketRuntime({&fake, fakeClock, fakeRead, fakeWrite,
                               fakeShutdown});
}

static void testBoundedReceiveAndPartialSend() {
  FakeSocket fake;
  BoundedSocketRuntime runtime = makeRuntime(fake);
  BoundedIngressBuffer ingress;
  static const char payload[] = "headers\r\n\r\nbody-secret";
  fake.readBytes = payload;
  fake.readLength = sizeof(payload) - 1;
  fake.nextRead = SocketReadStatus::DATA;

  CHECK_EQ(static_cast<int>(runtime.receiveInto(7, ingress)),
           static_cast<int>(IngressIoResult::PROGRESS),
           "fake socket data enters bounded ingress");
  CHECK_EQ(fake.lastReadBudget, BoundedIngressBuffer::kCapacity,
           "socket receive never requests hidden read-ahead capacity");
  CHECK_EQ(ingress.length(), sizeof(payload) - 1,
           "receive retains the exact unread TCP tail");
  CHECK_TRUE(std::memcmp(ingress.data(), payload, sizeof(payload) - 1) == 0,
             "receive preserves byte order");

  static const uint8_t response[] = "HTTP";
  fake.nextWrite = SocketWriteStatus::PROGRESS;
  fake.writeLength = 2;
  const SocketWriteResult partial = runtime.sendSome(
    7, response, sizeof(response) - 1);
  CHECK_EQ(static_cast<int>(partial.status),
           static_cast<int>(SocketWriteStatus::PROGRESS),
           "partial nonblocking send reports progress");
  CHECK_EQ(partial.length, 2UL,
           "partial nonblocking send acknowledges exact bytes");
  CHECK_EQ(fake.lastWriteLength, sizeof(response) - 1,
           "socket runtime offers only the caller-owned chunk");
}

static void testRetryAndInvalidIoFailClosed() {
  FakeSocket fake;
  BoundedSocketRuntime runtime = makeRuntime(fake);
  BoundedIngressBuffer ingress;

  fake.nextRead = SocketReadStatus::WOULD_BLOCK;
  CHECK_EQ(static_cast<int>(runtime.receiveInto(9, ingress)),
           static_cast<int>(IngressIoResult::WOULD_BLOCK),
           "EAGAIN or EINTR leaves ingress untouched for retry");
  CHECK_EQ(ingress.length(), 0UL,
           "retryable receive retains no phantom bytes");

  fake.nextRead = SocketReadStatus::DATA;
  fake.readLength = BoundedIngressBuffer::kCapacity + 1;
  CHECK_EQ(static_cast<int>(runtime.receiveInto(9, ingress)),
           static_cast<int>(IngressIoResult::ERROR),
           "impossible receive progress fails closed");
  CHECK_EQ(ingress.length(), 0UL,
           "invalid receive progress wipes ingress");

  static const uint8_t response[] = "OK";
  fake.nextWrite = SocketWriteStatus::PROGRESS;
  fake.writeLength = sizeof(response);
  const SocketWriteResult invalid = runtime.sendSome(
    9, response, sizeof(response) - 1);
  CHECK_EQ(static_cast<int>(invalid.status),
           static_cast<int>(SocketWriteStatus::ERROR),
           "impossible send progress fails closed");
}

static void testDeferredDrainRequiresLaterPoll() {
  FakeSocket fake;
  BoundedSocketRuntime runtime = makeRuntime(fake);
  fake.now = 100;

  CHECK_TRUE(runtime.beginDrain(),
             "terminal response starts one deferred drain");
  CHECK_TRUE(runtime.drainActive(),
             "beginning drain keeps deferred action pending");
  CHECK_EQ(fake.shutdownCalls, 0UL,
           "drain never shuts down or completes in final send call stack");

  fake.nextShutdown = SocketShutdownStatus::COMPLETE;
  fake.nextRead = SocketReadStatus::WOULD_BLOCK;
  fake.now = 101;
  CHECK_EQ(static_cast<int>(runtime.pollDrain(11)),
           static_cast<int>(DrainIoResult::WAITING),
           "later loop half-closes then waits without blocking");
  CHECK_EQ(fake.shutdownCalls, 1UL,
           "half-close happens once in a later loop");
  CHECK_TRUE(runtime.drainActive(),
             "deferred action remains pending while peer is open");

  fake.nextRead = SocketReadStatus::PEER_CLOSED;
  fake.now = 102;
  CHECK_EQ(static_cast<int>(runtime.pollDrain(11)),
           static_cast<int>(DrainIoResult::PEER_CLOSED),
           "peer close supplies bounded drain evidence");
  CHECK_TRUE(!runtime.drainActive(),
             "peer evidence releases deferred action gate");
  CHECK_TRUE(fake.clockCalls >= 3,
             "each drain transition samples the injected fresh clock");
}

static void testDrainDeadlineAndWraparound() {
  FakeSocket fake;
  BoundedSocketRuntime runtime = makeRuntime(fake);
  fake.now = UINT32_MAX - 10U;
  CHECK_TRUE(runtime.beginDrain(),
             "wrapped-clock drain begins");
  fake.nextShutdown = SocketShutdownStatus::WOULD_BLOCK;
  fake.now = UINT32_MAX - 10U +
             BoundedSocketRuntime::kDrainDeadlineMs - 1U;
  CHECK_EQ(static_cast<int>(runtime.pollDrain(13)),
           static_cast<int>(DrainIoResult::WAITING),
           "wrapped drain remains active before deadline");
  fake.now = UINT32_MAX - 10U +
             BoundedSocketRuntime::kDrainDeadlineMs;
  CHECK_EQ(static_cast<int>(runtime.pollDrain(13)),
           static_cast<int>(DrainIoResult::DEADLINE),
           "bounded drain releases action exactly at deadline");
  CHECK_TRUE(!runtime.drainActive(),
             "deadline cannot pin the single client slot");
}

int main() {
  testBoundedReceiveAndPartialSend();
  testRetryAndInvalidIoFailClosed();
  testDeferredDrainRequiresLaterPoll();
  testDrainDeadlineAndWraparound();
  return testReport();
}
