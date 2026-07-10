// Host-side integration tests for DataProcessor and the production parser.
// Task 4 replaces the remaining idle framing, clock override, and raw dump.

#include <cstring>
#include <deque>
#include <type_traits>
#include <utility>

#include "lib/CsvExport.h"
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
    MonitorRxEvent event = q.front();
    q.pop_front();
    return event.type == MonitorRxEventType::BYTE ? event.byte : -1;
  }
  bool nextRxEvent(MonitorRxEvent& event) override {
    if (q.empty()) return false;
    event = q.front();
    q.pop_front();
    return true;
  }
  const char* name() const override { return "FAKE"; }
  MonitorTransportState state() const override { return st; }
  String detail() const override { return det; }

  void feed(const char* value) {
    while (*value) feedByte(static_cast<uint8_t>(*value++));
  }
  void feedBytes(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) feedByte(data[i]);
  }
  void feedDiscontinuity() {
    lossCount++;
    MonitorRxEvent event;
    event.type = MonitorRxEventType::DISCONTINUITY;
    event.epoch = lossCount;
    q.push_back(event);
  }
  void feedStreamReset() {
    lossCount++;
    MonitorRxEvent event;
    event.type = MonitorRxEventType::STREAM_RESET;
    event.epoch = lossCount;
    q.push_back(event);
  }
  uint32_t dataLossCount() const override { return lossCount; }

  std::deque<MonitorRxEvent> q;
  uint32_t lossCount = 0;
  MonitorTransportState st = TRANSPORT_STATE_READY;
  String det = "ok";

private:
  void feedByte(uint8_t byte) {
    MonitorRxEvent event;
    event.type = MonitorRxEventType::BYTE;
    event.byte = byte;
    event.epoch = lossCount;
    q.push_back(event);
  }
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
    __millisCounter() = 0;
    __delayCallCount() = 0;
    __getLocalTimeCallCount() = 0;
    __serialOutput().clear();
    __fakeTimeValid() = false;
    proc.setup();
    __serialOutput().clear();
  }
};

static bool contains(const String& value, const char* needle) {
  return strstr(value.c_str(), needle) != nullptr;
}

static bool contains(const std::string& value, const char* needle) {
  return value.find(needle) != std::string::npos;
}

static void expectNoEncodedIdentity(const String& value, const char* label) {
  const char* representations[] = {
    "LEAK-MARKER-12345678",
    "4c 45 41 4b 2d 4d",
    "4C 45 41 4B 2D 4D",
    "TEVBSy1NQVJLRVItMTIzNDU2Nzg=",
    "LEAK%2DMARKER%2D12345678",
  };
  for (const char* representation : representations) {
    CHECK_TRUE(!contains(value, representation), label);
  }
}

static void expectNoEncodedIdentity(const std::string& value,
                                    const char* label) {
  const char* representations[] = {
    "LEAK-MARKER-12345678",
    "4c 45 41 4b 2d 4d",
    "4C 45 41 4B 2D 4D",
    "TEVBSy1NQVJLRVItMTIzNDU2Nzg=",
    "LEAK%2DMARKER%2D12345678",
  };
  for (const char* representation : representations) {
    CHECK_TRUE(!contains(value, representation), label);
  }
}

template <typename T, typename = void>
struct HasRawData : std::false_type {};

template <typename T>
struct HasRawData<T, std::void_t<decltype(std::declval<T>().rawData)>>
  : std::true_type {};

template <typename T>
static bool recordContains(const T& value, const char* needle) {
  if constexpr (HasRawData<T>::value) {
    return contains(value.rawData, needle);
  }
  return false;
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
  CHECK_TRUE(contains(invalid.lastData, "malformed"),
             "invalid frame exposes stable sanitized reason");
  CHECK_TRUE(!contains(invalid.lastData, "hello"),
             "invalid diagnostic never echoes input");

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
  CHECK_TRUE(contains(world.lastData, "overflow"),
             "overflow exposes stable sanitized reason");

  feedLine(world.transport, kFrame120);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1, "clean frame after overflow accepted");
}

static void testDeviceTimeIsAuthoritativeAndNonblocking() {
  World world;
  __fakeTimeValid() = true;
  __fakeTm().tm_year = 126;
  __fakeTm().tm_mon = 6;
  __fakeTm().tm_mday = 2;
  __fakeTm().tm_hour = 10;
  __fakeTm().tm_min = 30;
  __fakeTm().tm_sec = 0;
  unsigned long before = millis();
  feedLine(world.transport, kFrame120);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1, "valid device frame persists");
  CHECK_STR(world.records.getLatestRecord().timestamp, "2026-07-11 09:05:00",
            "device timestamp wins over system/NTP time");
  CHECK_EQ(static_cast<int>(world.records.getLatestRecord().timestampSource),
           static_cast<int>(BPTimestampSource::DEVICE),
           "persisted timestamp source is device");
  CHECK_EQ(__getLocalTimeCallCount(), 0UL,
           "measurement path never calls system clock");
  CHECK_EQ(__delayCallCount(), 0UL, "measurement path never calls delay");
  CHECK_EQ(millis(), before, "measurement processing does not advance fake clock");
}

static void testIdentityNeverReachesDiagnosticsOrRecord() {
  static const char* kLeakMarker = "LEAK-MARKER-12345678";
  static const char* kLeakMarkerHex = "4c 45 41 4b 2d 4d";
  World world;
  String payload("2026,07,11,09,05,");
  payload += kLeakMarker;
  payload += ",0,120,080,072,0";
  feedLine(world.transport, payload.c_str());
  world.proc.processIncomingData();

  CHECK_EQ(world.records.getRecordCount(), 1, "privacy probe remains valid");
  CHECK_TRUE(!contains(world.lastData, kLeakMarker),
             "subject ID absent from web diagnostic");
  CHECK_TRUE(!contains(world.lastData, kLeakMarkerHex),
             "subject ID hex absent from web diagnostic");
  CHECK_TRUE(!contains(__serialOutput(), kLeakMarker),
             "subject ID absent from production Serial output");
  CHECK_TRUE(!recordContains(world.records.getLatestRecord(), kLeakMarker),
             "subject ID absent from in-memory record");
  CHECK_TRUE(!recordContains(world.records.getLatestRecord(), kLeakMarkerHex),
             "subject ID hex absent from in-memory record");
  CHECK_TRUE(!HasRawData<BPData>::value,
             "persistable BPData has no raw-frame field");

  expectNoEncodedIdentity(world.lastData,
                          "encoded identity absent from valid diagnostic");
  expectNoEncodedIdentity(__serialOutput(),
                          "encoded identity absent from valid Serial output");
  CHECK_TRUE(!Preferences::__containsSubstring(kLeakMarker),
             "subject ID absent from every fake NVS value");

  String csv;
  appendHistoryCsv(csv, world.records);
  expectNoEncodedIdentity(csv, "encoded identity absent from CSV");

  World error;
  String errorPayload("2026,07,11,09,05,");
  errorPayload += kLeakMarker;
  errorPayload += ",7,   ,   ,   ,0";
  feedLine(error.transport, errorPayload.c_str());
  error.proc.processIncomingData();
  CHECK_EQ(error.records.getRecordCount(), 0,
           "identity canary error frame never persists");
  expectNoEncodedIdentity(error.lastData,
                          "encoded identity absent from rejected diagnostic");
  expectNoEncodedIdentity(__serialOutput(),
                          "encoded identity absent from rejected Serial output");
  CHECK_TRUE(!Preferences::__containsSubstring(kLeakMarker),
             "rejected subject ID absent from fake NVS");
}

static void testInvalidDeviceTimeIsNotRepaired() {
  World world;
  __fakeTimeValid() = true;
  __fakeTm().tm_year = 126;
  __fakeTm().tm_mon = 6;
  __fakeTm().tm_mday = 11;
  world.transport.feed(
    "2025,02,29,09,05,12345678901234567890,0,120,080,072,0\r\n");
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 0,
           "invalid device date is never repaired with system time");
  CHECK_EQ(__getLocalTimeCallCount(), 0UL,
           "invalid device date does not consult system clock");
  CHECK_TRUE(contains(world.lastData, "invalid_timestamp"),
             "invalid device date has stable sanitized reason");
}

static void testStrictCrlfAndNoIdleCompletion() {
  World split;
  const size_t splitAt = 27;
  split.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120), splitAt);
  split.proc.processIncomingData();
  delay(5001);
  split.proc.processIncomingData();
  CHECK_EQ(split.records.getRecordCount(), 0,
           "half line remains pending after five seconds");
  split.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120 + splitAt),
                            strlen(kFrame120) - splitAt);
  split.transport.feed("\r\n");
  split.proc.processIncomingData();
  CHECK_EQ(split.records.getRecordCount(), 1,
           "pending half completes only at CRLF");

  World lfOnly;
  lfOnly.transport.feed(kFrame120);
  lfOnly.transport.feed("\n");
  lfOnly.proc.processIncomingData();
  CHECK_EQ(lfOnly.records.getRecordCount(), 0, "LF-only line rejected");
  feedLine(lfOnly.transport, kFrame120);
  lfOnly.proc.processIncomingData();
  CHECK_EQ(lfOnly.records.getRecordCount(), 1,
           "clean CRLF line after LF-only input accepted");

  World crOnly;
  crOnly.transport.feed(kFrame120);
  crOnly.transport.feed("\r");
  crOnly.proc.processIncomingData();
  delay(5001);
  crOnly.proc.processIncomingData();
  CHECK_EQ(crOnly.records.getRecordCount(), 0, "CR-only line never completes");
}

static void testOrderedTransportLossRecovery() {
  World world;
  const size_t splitAt = 24;
  world.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120), splitAt);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 0, "pre-loss partial remains pending");

  world.transport.feedDiscontinuity();
  world.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120 + splitAt),
                            strlen(kFrame120) - splitAt);
  world.transport.feed("\r\n");
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 0,
           "bytes spanning loss are discarded through CRLF");
  CHECK_TRUE(contains(world.lastData, "discontinuity"),
             "loss exposes stable sanitized diagnostic");
  CHECK_EQ(world.transport.dataLossCount(), 1U,
           "data-loss counter is monotonic and explicit");

  feedLine(world.transport, kFrame130);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1,
           "first clean frame after loss is accepted");
  CHECK_EQ(world.records.getLatestRecord().systolic, 130,
           "post-loss frame is not contaminated by old bytes");
}

static void testModelSwitchClearsPartialFrame() {
  World world;
  const size_t splitAt = 19;
  world.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120), splitAt);
  world.proc.processIncomingData();

  world.parser.setModel(String("CUSTOM"));
  world.proc.processIncomingData();
  world.parser.setModel(String("OMRON-HBP9030"));
  world.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120 + splitAt),
                            strlen(kFrame120) - splitAt);
  world.transport.feed("\r\n");
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 0,
           "old suffix cannot cross a model switch");

  feedLine(world.transport, kFrame120);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1,
           "clean frame after model switch is accepted");
}

static void testCleanReconnectBoundaryKeepsFirstNewFrame() {
  World world;
  world.transport.feedBytes(reinterpret_cast<const uint8_t*>(kFrame120), 18);
  world.proc.processIncomingData();
  world.transport.feedStreamReset();
  feedLine(world.transport, kFrame130);
  world.proc.processIncomingData();
  CHECK_EQ(world.records.getRecordCount(), 1,
           "trusted reconnect boundary accepts first clean frame");
  CHECK_EQ(world.records.getLatestRecord().systolic, 130,
           "pre-reconnect partial cannot contaminate clean frame");
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
  testDeviceTimeIsAuthoritativeAndNonblocking();
  testIdentityNeverReachesDiagnosticsOrRecord();
  testInvalidDeviceTimeIsNotRepaired();
  testStrictCrlfAndNoIdleCompletion();
  testOrderedTransportLossRecovery();
  testModelSwitchClearsPartialFrame();
  testCleanReconnectBoundaryKeepsFirstNewFrame();
  testTransportStatusSync();
  return testReport();
}
