// Host-side spec tests for lib/DataProcessor.h — frame assembly 目標行為
//
// 規格（SDD，設計與 codex 討論定案）：
//   S1. Line-based 型號（isLineDelimited）：以 '\n' 為 frame 邊界累積 bytes，
//       跨多次 poll 的分段在 timeout 內要組成同一個 frame（一筆記錄）。
//       trailing '\r' 要剝除。
//   S2. Timeout flush fallback：頭尾無換行的 frame 在 idle 超過 flush timeout
//       後以現有內容解析（binary 型號唯一路徑；line 型號的保底）。
//       binary frame 內含 0x0A 不得被切割。
//   S3. 持久化政策：只有 parse 成功（valid）的 frame 寫入 ring buffer/NVS；
//       invalid frame 只更新 lastData（RAM 診斷），不污染歷史、不磨損 flash。
//   S4. Overflow：frame 超過緩衝上限 → 丟棄不持久化，lastData 留下截斷診斷。
//   S5. lastData 呈現：hex dump + HTML-escape 過的 ASCII；
//       getLocalTime 失敗 → timestamp「時間未同步」。
//
// 執行：bash scripts/run_host_tests.sh

#include <cstring>
#include <deque>

#include "lib/DataProcessor.h"
#include "test_support.h"

class FakeTransport : public MonitorTransport {
public:
  bool begin() override { return true; }
  void poll() override {}
  int available() override { return (int)q.size(); }
  int read() override {
    if (q.empty()) return -1;
    int b = q.front();
    q.pop_front();
    return b;
  }
  const char* name() const override { return "FAKE"; }
  MonitorTransportState state() const override { return st; }
  String detail() const override { return det; }

  void feed(const char* s) {
    while (*s) q.push_back((uint8_t)*s++);
  }
  void feedBytes(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) q.push_back(p[i]);
  }

  std::deque<uint8_t> q;
  MonitorTransportState st = TRANSPORT_STATE_READY;
  String det = "ok";
};

// 每個 fixture 一組乾淨的世界
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

static bool contains(const String& s, const char* needle) {
  return strstr(s.c_str(), needle) != nullptr;
}

// ---- S1: line framing ----
static void testCompleteLineFrame() {
  World w;
  w.transport.feed("1,2,3,4,5,6,7,120,80,72\r\n");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 1, "complete line -> one record");
  CHECK_TRUE(w.records.getLatestRecord().valid, "complete line valid");
  CHECK_EQ(w.records.getLatestRecord().systolic, 120, "complete line sys");
}

static void testSplitFrameWithinTimeout() {
  World w;
  w.transport.feed("1,2,3,4,5,6,7,12");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "partial frame not yet a record");
  delay(5); // < flush timeout
  w.transport.feed("0,80,72\n");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 1, "split frame reassembled to one record");
  CHECK_EQ(w.records.getLatestRecord().systolic, 120, "reassembled sys correct");
}

static void testTwoFramesInOneBurst() {
  World w;
  w.transport.feed("1,2,3,4,5,6,7,120,80,72\n1,2,3,4,5,6,7,130,85,75\n");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 2, "two lines -> two records");
  CHECK_EQ(w.records.getLatestRecord().systolic, 130, "second frame is latest");
}

// ---- S2: timeout flush ----
static void testTimeoutFlushValidFrame() {
  World w;
  w.parser.setModel(String("CUSTOM"));
  w.transport.feed("SYS:120,DIA:80,PUL:75"); // 無換行
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "no flush before timeout");
  delay(60); // > flush timeout
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 1, "timeout flush parses pending frame");
  CHECK_EQ(w.records.getLatestRecord().pulse, 75, "timeout flush values");
}

static void testBinaryFrameWithNewlineByte() {
  World w;
  w.parser.setModel(String("OMRON-HBP1300"));
  // dia 低位 byte 剛好是 0x0A（10 mmHg... 假設值），不得被當 frame 邊界
  const uint8_t frame[] = {0x01, 0x00, 0x00, 0x78, 0x00, 0x0A,
                           0x00, 0x48, 0x00, 0x00};
  w.transport.feedBytes(frame, sizeof(frame));
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "binary: no split on 0x0A");
  delay(60);
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 1, "binary: timeout flush whole frame");
  CHECK_EQ(w.records.getLatestRecord().diastolic, 10, "binary 0x0A byte kept as data");
  CHECK_EQ(w.records.getLatestRecord().systolic, 120, "binary sys");
}

// codex review P2：閒置逾時的舊 partial frame 必須在讀新 bytes「之前」先
// flush，否則舊殘渣會與剛到的新 frame 合併成一個壞 frame（binary 型號只靠
// timeout 斷界，最受影響）。
static void testStaleFrameNotMergedWithNewBurst() {
  World w;
  w.parser.setModel(String("OMRON-HBP1300"));
  const uint8_t stale[] = {0xFF, 0xFF, 0xFF}; // 垃圾殘渣（非 0x01 開頭）
  w.transport.feedBytes(stale, sizeof(stale));
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "stale partial buffered, no record");

  delay(60); // 舊 frame 已閒置逾時，但下次 poll 前新 frame 已排隊
  const uint8_t fresh[] = {0x01, 0x00, 0x00, 0x78, 0x00, 0x50,
                           0x00, 0x48, 0x00, 0x00};
  w.transport.feedBytes(fresh, sizeof(fresh));
  w.proc.processIncomingData();
  delay(60);
  w.proc.processIncomingData(); // flush 新 frame

  CHECK_EQ(w.records.getRecordCount(), 1, "stale junk flushed separately, fresh frame parsed");
  CHECK_EQ(w.records.getLatestRecord().systolic, 120, "fresh frame not contaminated by stale bytes");
}

// ---- S3: 只持久化 valid ----
static void testInvalidFrameNotPersisted() {
  World w;
  w.transport.feed("hello garbage\n");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "invalid frame not persisted");
  CHECK_TRUE(w.lastData.length() > 0, "invalid frame still visible in lastData");
  CHECK_TRUE(contains(w.lastData, "68 65 6c 6c 6f"), "lastData hex dump of frame");
}

// ---- S4: overflow ----
static void testOverflowDropped() {
  World w;
  String big;
  for (int i = 0; i < 300; i++) big += 'x';
  big += '\n';
  w.transport.feed(big.c_str());
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 0, "overflow frame not persisted");
  CHECK_TRUE(contains(w.lastData, "截斷"), "overflow leaves truncation diagnostic");
  // overflow 之後的正常 frame 不受影響
  w.transport.feed("1,2,3,4,5,6,7,120,80,72\n");
  w.proc.processIncomingData();
  CHECK_EQ(w.records.getRecordCount(), 1, "recovers after overflow");
}

// ---- S5: lastData 呈現與 timestamp ----
static void testLastDataEscapingAndTimestamp() {
  World w;
  w.transport.feed("<b>1,2,3,4,5,6,7,120,80,72\n");
  w.proc.processIncomingData();
  CHECK_TRUE(contains(w.lastData, "&lt;b&gt;"), "ascii section html-escaped");
  CHECK_TRUE(!contains(w.lastData, "<b>"), "raw tag not present in lastData");

  // getLocalTime 失敗 → 時間未同步
  CHECK_EQ(w.records.getRecordCount(), 1, "escaped frame still parses");
  CHECK_TRUE(contains(w.records.getLatestRecord().timestamp, "時間未同步"),
             "unsynced clock marked");

  // getLocalTime 成功 → 格式化時間
  __fakeTimeValid() = true;
  __fakeTm().tm_year = 126; __fakeTm().tm_mon = 6; __fakeTm().tm_mday = 2;
  __fakeTm().tm_hour = 10; __fakeTm().tm_min = 30; __fakeTm().tm_sec = 0;
  w.transport.feed("1,2,3,4,5,6,7,130,85,75\n");
  w.proc.processIncomingData();
  CHECK_STR(w.records.getLatestRecord().timestamp, "2026-07-02 10:30:00",
            "synced clock formatted");
}

static void testTransportStatusSync() {
  World w;
  CHECK_TRUE(contains(w.transportStatus, "就緒"), "status label from state");
  CHECK_TRUE(contains(w.transportStatus, "ok"), "status includes detail");
  CHECK_STR(w.transportName, "FAKE", "transport name synced once");
  w.transport.st = TRANSPORT_STATE_ERROR;
  w.transport.det = "boom";
  w.proc.processIncomingData();
  CHECK_TRUE(contains(w.transportStatus, "錯誤"), "status follows state change");
  CHECK_TRUE(contains(w.transportStatus, "boom"), "detail follows change");
}

int main() {
  testCompleteLineFrame();
  testSplitFrameWithinTimeout();
  testTwoFramesInOneBurst();
  testTimeoutFlushValidFrame();
  testBinaryFrameWithNewlineByte();
  testStaleFrameNotMergedWithNewBurst();
  testInvalidFrameNotPersisted();
  testOverflowDropped();
  testLastDataEscapingAndTimestamp();
  testTransportStatusSync();
  return testReport();
}
