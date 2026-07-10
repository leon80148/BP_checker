#include <cstddef>
#include <cstdint>
#include <vector>

#include "lib/ProtocolFramer.h"
#include "test_support.h"

static const uint8_t kSync[] = {0xA5, 0x5A};
static constexpr size_t kFrameLength = 10;

static bool verifiedFrame(const uint8_t* data, size_t length) {
  if (length != kFrameLength || data[0] != kSync[0] || data[1] != kSync[1]) {
    return false;
  }
  uint8_t checksum = 0;
  for (size_t i = 0; i + 1 < length; ++i) checksum += data[i];
  return checksum == data[length - 1];
}

static std::vector<uint8_t> makeFrame(uint8_t seed, bool embeddedLf = false) {
  std::vector<uint8_t> frame(kFrameLength);
  frame[0] = kSync[0];
  frame[1] = kSync[1];
  for (size_t i = 2; i + 1 < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>(seed + i);
  }
  if (embeddedLf) frame[5] = 0x0A;
  uint8_t checksum = 0;
  for (size_t i = 0; i + 1 < frame.size(); ++i) checksum += frame[i];
  frame.back() = checksum;
  return frame;
}

struct Collector {
  ProtocolFramer framer;
  ProtocolFrameContract contract = ProtocolFrameContract::fixedLengthVerified(
    kFrameLength, kSync, sizeof(kSync), verifiedFrame);
  std::vector<std::vector<uint8_t>> frames;

  void feed(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      ProtocolFrameEvent event = framer.feed(data[i], contract);
      if (event == ProtocolFrameEvent::FRAME) {
        frames.emplace_back(framer.frameData(),
                            framer.frameData() + framer.frameLength());
        framer.clearCompletedFrame();
      }
    }
  }

  void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }
};

static void testBurstAndFragmentation() {
  std::vector<uint8_t> first = makeFrame(0x10);
  std::vector<uint8_t> second = makeFrame(0x30);
  std::vector<uint8_t> burst = first;
  burst.insert(burst.end(), second.begin(), second.end());

  Collector burstCollector;
  burstCollector.feed(burst);
  CHECK_EQ(burstCollector.frames.size(), static_cast<size_t>(2),
           "two fixed frames in one burst emit twice");
  CHECK_EQ(burstCollector.frames[0][2], first[2], "fixed burst order first");
  CHECK_EQ(burstCollector.frames[1][2], second[2], "fixed burst order second");

  Collector fragmented;
  fragmented.feed(first.data(), 4);
  CHECK_EQ(fragmented.frames.size(), static_cast<size_t>(0),
           "4-byte fragment waits");
  fragmented.feed(first.data() + 4, first.size() - 4);
  CHECK_EQ(fragmented.frames.size(), static_cast<size_t>(1),
           "4+6 fixed fragment emits once");
}

static void testCompletePlusPartialAndEmbeddedLf() {
  std::vector<uint8_t> first = makeFrame(0x20, true);
  std::vector<uint8_t> second = makeFrame(0x40);
  Collector collector;
  collector.feed(first);
  collector.feed(second.data(), 5);
  CHECK_EQ(collector.frames.size(), static_cast<size_t>(1),
           "complete frame emits while half remains pending");
  CHECK_TRUE(collector.framer.pending(), "fixed half retained");
  CHECK_EQ(collector.frames[0][5], 0x0A, "embedded LF remains binary data");
  collector.feed(second.data() + 5, second.size() - 5);
  CHECK_EQ(collector.frames.size(), static_cast<size_t>(2),
           "retained half completes later");
}

static void testVerifiedResynchronization() {
  std::vector<uint8_t> corrupt = makeFrame(0x50);
  corrupt.back() ^= 0xFF;
  std::vector<uint8_t> valid = makeFrame(0x70);
  const uint8_t garbage[] = {0x00, 0xA5, 0x01, 0x5A, 0xFF};

  Collector collector;
  collector.feed(garbage, sizeof(garbage));
  collector.feed(corrupt);
  CHECK_EQ(collector.frames.size(), static_cast<size_t>(0),
           "header without valid checksum never emits");
  const uint8_t trailing[] = {0x99, 0xA5};
  collector.feed(trailing, sizeof(trailing));
  collector.feed(valid);
  CHECK_EQ(collector.frames.size(), static_cast<size_t>(1),
           "garbage resynchronizes only at verified header/checksum");
  CHECK_EQ(collector.frames[0][2], valid[2], "resynchronized frame is clean");
}

static bool syncOnlyValidator(const uint8_t* data, size_t length) {
  return length == sizeof(kSync) && data[0] == kSync[0] && data[1] == kSync[1];
}

static void testContractBounds() {
  ProtocolFramer line;
  ProtocolFrameContract impossible = ProtocolFrameContract::lineCrlf(SIZE_MAX);
  CHECK_EQ(static_cast<int>(line.feed('x', impossible)),
           static_cast<int>(ProtocolFrameEvent::UNSUPPORTED),
           "SIZE_MAX line contract fails closed before arithmetic overflow");
  CHECK_TRUE(!line.pending(), "invalid line contract retains no byte");

  ProtocolFramer fixed;
  ProtocolFrameContract syncOnly = ProtocolFrameContract::fixedLengthVerified(
    sizeof(kSync), kSync, sizeof(kSync), syncOnlyValidator);
  CHECK_EQ(static_cast<int>(fixed.feed(kSync[0], syncOnly)),
           static_cast<int>(ProtocolFrameEvent::NONE),
           "sync-only fixed frame waits for complete sync");
  CHECK_EQ(static_cast<int>(fixed.feed(kSync[1], syncOnly)),
           static_cast<int>(ProtocolFrameEvent::FRAME),
           "sync length equal to frame length completes without extra write");
  CHECK_EQ(fixed.frameLength(), sizeof(kSync),
           "sync-only fixed frame reports exact length");
}

int main() {
  testBurstAndFragmentation();
  testCompletePlusPartialAndEmbeddedLf();
  testVerifiedResynchronization();
  testContractBounds();
  return testReport();
}
