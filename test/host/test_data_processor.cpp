// Host-side integration tests for DataProcessor and the production parser.
// Task 4 replaces the remaining idle framing, clock override, and raw dump.

#include <cstring>
#include <deque>

#include "lib/DataProcessor.h"
#include "test_support.h"

static const char* kFrame120 =
  "2026,07,11,09,05,12345678901234567890,0,120,080,072,0";
static const char* kFrame130 =
  "2026,07,11,09,06,ABCDEFGHIJKLMNOPQRST,0,130,085,075,0";

class FakeTransport : public MonitorTransport {
public:
  bool begin() override { return true; }
  void poll() override {}
  int available() override { return static_cast<int>(q.size()); }
  int read() override {
    if (q.empty()) return -1;
    int value = q.front();
    q.pop_front();
    return value;
  }
  const char* name() const override { return "FAKE"; }
  MonitorTransportState state() const override { return st; }
  String detail() const override { return det; }

  void feed(const char* value) {
    while (*value) q.push_back(static_cast<uint8_t>(*value++));
  }
  void feedBytes(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) q.push_back(data[i]);
  }

  std::deque<uint8_t> q;
  MonitorTransportState st = TRANSPORT_STATE_READY;
  String det = "ok";
};

struct World {
  BP_Parser parser{String("OMRON-HBP9030")};
  BP_RecordManager records{5};
  String lastData, transportName, transportStatus;
  FakeTransport transport;
  DataProcessor proc{&parser, &records, &lastData,
                     &transportName, &transportStatus, &transport};

  World() {
    Preferences::__reset();
    __fakeTimeValid() = false;
    proc.setup();
  }
};

static bool contains(const String& value, const char* needle) {
  return strstr(value.c_str(), needle) != nullptr;
}

static void feedLine(FakeTransport& transport, const char* payload) {
  transport.feed(payload);
  transport.feed("\r\n");
}

static void testCompleteAndSplitLines() {
  World complete;
  feedLine(complete.transport, kFrame120);
  complete.proc.processIncomingData();
  CHECK_EQ(complete.records.getRecordCount(), 1, "CRLF frame -> one record");
  CHECK_EQ(complete.records.getLatestRecord().systolic, 120, "CRLF SYS");

  World split;
  const size_t splitAt = 27;
  split.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120), splitAt);
  split.proc.processIncomingData();
  CHECK_EQ(split.records.getRecordCount(), 0, "partial frame waits");
  delay(5);
  split.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120 + splitAt),
                            strlen(kFrame120) - splitAt);
  split.transport.feed("\r\n");
  split.proc.processIncomingData();
  CHECK_EQ(split.records.getRecordCount(), 1, "split CRLF frame reassembled");
  CHECK_EQ(split.records.getLatestRecord().pulse, 72, "split pulse correct");
}

static void testTwoFramesInOneBurst() {
  World world;
  String burst(kFrame120);
  burst += "\r\n";
  burst += kFrame130;
  burst += "\r\n";
  world.transport.feed(burst.c_str());
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 2, "two CRLF frames -> two records");
  CHECK_EQ(world.records.getLatestRecord().systolic, 130,
           "second frame remains latest");
}

static void testUnsupportedModelsNeverPersist() {
  World custom;
  custom.parser.setModel(String("CUSTOM"));
  custom.transport.feed("SYS:120,DIA:80,PUL:75");
  custom.proc.processIncomingData();
  delay(60);
  custom.proc.processIncomingData();
  CHECK_EQ(custom.records.getRecordCount(), 0, "CUSTOM production data rejected");

  World binary;
  binary.parser.setModel(String("OMRON-HBP1300"));
  const uint8_t bytes[] = {0x01, 0x00, 0x0A, 0x78, 0x00, 0x50};
  binary.transport.feedBytes(bytes, sizeof(bytes));
  binary.proc.processIncomingData();
  delay(60);
  binary.proc.processIncomingData();
  CHECK_EQ(binary.records.getRecordCount(), 0,
           "unverified binary model never persists");
}

static void testInvalidErrorAndMotionPolicy() {
  World invalid;
  invalid.transport.feed("hello garbage\n");
  invalid.proc.processIncomingData();
  CHECK_EQ(invalid.records.getRecordCount(), 0, "invalid frame not persisted");
  CHECK_TRUE(contains(invalid.lastData, "68 65 6c 6c 6f"),
             "invalid diagnostic retained in RAM");

  World monitorError;
  feedLine(monitorError.transport,
           "2026,07,11,09,05,12345678901234567890,7,   ,   ,   ,0");
  monitorError.proc.processIncomingData();
  CHECK_EQ(monitorError.records.getRecordCount(), 0,
           "well-formed monitor error not persisted");

  World motion;
  feedLine(motion.transport,
           "2026,07,11,09,05,12345678901234567890,0,120,080,072,1");
  motion.proc.processIncomingData();
  CHECK_EQ(motion.records.getRecordCount(), 1, "motion reading persists with warning");
  CHECK_EQ(motion.records.getLatestRecord().movementCount, 1,
           "movement metadata reaches record manager");
  CHECK_EQ(static_cast<int>(motion.records.getLatestRecord().quality),
           static_cast<int>(BPMeasurementQuality::MOTION),
           "motion quality reaches record manager");
}

static void testOverflowDroppedThenRecovers() {
  World world;
  String oversized;
  for (int i = 0; i < 300; ++i) oversized += 'x';
  oversized += "\r\n";
  world.transport.feed(oversized.c_str());
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 0, "overflow frame not persisted");
  CHECK_TRUE(contains(world.lastData, "截斷"), "overflow diagnostic retained");

  feedLine(world.transport, kFrame120);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1, "clean frame after overflow accepted");
}

static void testRawEscapingAndCurrentClockCompatibility() {
  World world;
  feedLine(world.transport,
           "2026,07,11,09,05,<b>12345678901234567,0,120,080,072,0");
  world.proc.processIncomingData();
  CHECK_TRUE(contains(world.lastData, "&lt;b&gt;"), "diagnostic HTML escaped");
  CHECK_TRUE(!contains(world.lastData, "<b>"), "raw tag absent from HTML");
  CHECK_EQ(world.records.getRecordCount(), 1, "escaped strict frame parses");
  CHECK_TRUE(contains(world.records.getLatestRecord().timestamp, "時間未同步"),
             "Task 4 clock override remains explicit");

  __fakeTimeValid() = true;
  __fakeTm().tm_year = 126;
  __fakeTm().tm_mon = 6;
  __fakeTm().tm_mday = 2;
  __fakeTm().tm_hour = 10;
  __fakeTm().tm_min = 30;
  __fakeTm().tm_sec = 0;
  feedLine(world.transport, kFrame130);
  world.proc.processIncomingData();
  CHECK_STR(world.records.getLatestRecord().timestamp, "2026-07-02 10:30:00",
            "Task 4 will replace system clock compatibility");
}

static void testTransportStatusSync() {
  World world;
  CHECK_TRUE(contains(world.transportStatus, "就緒"), "status label from state");
  CHECK_TRUE(contains(world.transportStatus, "ok"), "status includes detail");
  CHECK_STR(world.transportName, "FAKE", "transport name synced");
  world.transport.st = TRANSPORT_STATE_ERROR;
  world.transport.det = "boom";
  world.proc.processIncomingData();
  CHECK_TRUE(contains(world.transportStatus, "錯誤"), "status follows change");
  CHECK_TRUE(contains(world.transportStatus, "boom"), "detail follows change");
}

int main() {
  testCompleteAndSplitLines();
  testTwoFramesInOneBurst();
  testUnsupportedModelsNeverPersist();
  testInvalidErrorAndMotionPolicy();
  testOverflowDroppedThenRecovers();
  testRawEscapingAndCurrentClockCompatibility();
  testTransportStatusSync();
  return testReport();
}
