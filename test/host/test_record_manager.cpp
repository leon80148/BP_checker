// Host-side spec tests for lib/BPRecordManager.h
//
// 規格（SDD，edge cases 與 codex 討論定案）：
//   S1. Ring buffer：getRecord(0)=最新；超過 maxRecords 環繞覆蓋最舊；
//       out-of-range 回傳 default（valid=false）記錄。
//   S2. 持久化 round-trip：重啟（新實例 loadFromStorage）後筆數、順序、
//       數值、valid 旗標不變；rawData 不持久化（重啟後為空）。
//   S3. 韌性：壞 count/index 被 clamp；缺 slot 不得偽造有效量測；
//       maxRecords=1 正常運作。
//   S4. Legacy 遷移：rec_0=最新 的舊格式 load 後順序不變並回寫為 v2；
//       4 欄舊記錄以數值合理性推導 valid。
//   S5. clearRecords 清空記憶體與 NVS（新舊格式 key 一併清除）。
//   已知限制（不測）：timestamp 含 '|' 會破壞序列化格式；timestamp 皆由
//   strftime 內部產生，實務上不含 '|'。
//
// 執行：bash scripts/run_host_tests.sh

#include "lib/BPRecordManager.h"
#include "test_support.h"

static BPData makeRecord(const char* ts, int sys, int dia, int pul, bool valid) {
  BPData d;
  d.timestamp = ts;
  d.systolic = sys;
  d.diastolic = dia;
  d.pulse = pul;
  d.valid = valid;
  d.rawData = "<pre>raw</pre>";
  return d;
}

// ---- S1: ring buffer 行為 ----
static void testRingBuffer() {
  Preferences::__reset();
  BP_RecordManager m(3);
  m.loadFromStorage();
  CHECK_EQ(m.getRecordCount(), 0, "fresh store loads empty");
  CHECK_TRUE(!m.getRecord(0).valid, "getRecord on empty -> invalid default");

  m.addRecord(makeRecord("t1", 110, 70, 60, true));
  CHECK_EQ(m.getRecordCount(), 1, "count after first add");
  CHECK_EQ(m.getLatestRecord().systolic, 110, "latest after first add");

  m.addRecord(makeRecord("t2", 120, 80, 65, true));
  m.addRecord(makeRecord("t3", 130, 85, 70, true));
  CHECK_EQ(m.getRecord(0).systolic, 130, "newest-first order [0]");
  CHECK_EQ(m.getRecord(1).systolic, 120, "newest-first order [1]");
  CHECK_EQ(m.getRecord(2).systolic, 110, "newest-first order [2]");

  m.addRecord(makeRecord("t4", 140, 90, 75, true));
  m.addRecord(makeRecord("t5", 150, 95, 80, true));
  CHECK_EQ(m.getRecordCount(), 3, "count capped at maxRecords");
  CHECK_EQ(m.getRecord(0).systolic, 150, "wraparound newest");
  CHECK_EQ(m.getRecord(2).systolic, 130, "wraparound oldest survivor");
  CHECK_TRUE(!m.getRecord(3).valid, "out-of-range index -> invalid default");
  CHECK_TRUE(!m.getRecord(-1).valid, "negative index -> invalid default");
}

// ---- S2: 持久化 round-trip ----
static void testPersistenceRoundTrip() {
  Preferences::__reset();
  {
    BP_RecordManager m(3);
    m.loadFromStorage();
    m.addRecord(makeRecord("2026-07-02 10:00:00", 120, 80, 65, true));
    m.addRecord(makeRecord("2026-07-02 10:05:00", -1, -1, -1, false));
  }
  BP_RecordManager m2(3);
  m2.loadFromStorage();
  CHECK_EQ(m2.getRecordCount(), 2, "reload count");
  CHECK_STR(m2.getRecord(0).timestamp, "2026-07-02 10:05:00", "reload order newest ts");
  CHECK_TRUE(!m2.getRecord(0).valid, "invalid record stays invalid after reload");
  CHECK_EQ(m2.getRecord(0).systolic, -1, "sentinel -1 survives reload");
  CHECK_EQ(m2.getRecord(1).systolic, 120, "values survive reload");
  CHECK_TRUE(m2.getRecord(1).valid, "valid flag survives reload");
  CHECK_EQ((long)m2.getRecord(0).rawData.length(), 0L, "rawData not persisted");

  // 環繞後 reload 順序仍正確
  {
    BP_RecordManager m3(3);
    m3.loadFromStorage();
    m3.addRecord(makeRecord("t3", 130, 85, 70, true));
    m3.addRecord(makeRecord("t4", 140, 90, 75, true));
  }
  BP_RecordManager m4(3);
  m4.loadFromStorage();
  CHECK_EQ(m4.getRecordCount(), 3, "post-wrap reload count");
  CHECK_STR(m4.getRecord(0).timestamp, "t4", "post-wrap reload newest");
  CHECK_EQ(m4.getRecord(2).systolic, -1, "post-wrap reload oldest survivor");
}

// ---- S3: 壞資料韌性 ----
static void testCorruptionResilience() {
  // 壞 count / index：clamp、不 crash
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 999);
    p.putInt("index", -5);
    p.end();
  }
  BP_RecordManager m(3);
  m.loadFromStorage();
  CHECK_EQ(m.getRecordCount(), 3, "count clamped to maxRecords");
  CHECK_TRUE(!m.getRecord(0).valid, "missing slots load as invalid defaults");

  // 缺 slot：不得偽造有效量測
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 2);
    p.putInt("index", 2);
    p.putString("slot_0", "t0|120|80|65|1");
    // slot_1 缺
    p.end();
  }
  BP_RecordManager m2(3);
  m2.loadFromStorage();
  CHECK_EQ(m2.getRecordCount(), 2, "count from metadata");
  CHECK_TRUE(!m2.getRecord(0).valid, "missing slot -> invalid, not fabricated");
  CHECK_TRUE(m2.getRecord(1).valid, "present slot loads normally");

  // maxRecords=1
  Preferences::__reset();
  BP_RecordManager m3(1);
  m3.loadFromStorage();
  m3.addRecord(makeRecord("a", 100, 60, 55, true));
  m3.addRecord(makeRecord("b", 105, 65, 58, true));
  CHECK_EQ(m3.getRecordCount(), 1, "maxRecords=1 count");
  CHECK_STR(m3.getLatestRecord().timestamp, "b", "maxRecords=1 keeps newest");
}

// ---- S4: legacy rec_* 遷移 ----
static void testLegacyMigration() {
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 3); // 無 schema key = 舊格式
    p.putString("rec_0", "newest|130|85|70|1");   // 5 欄
    p.putString("rec_1", "middle|120|80|65|0");   // 5 欄 invalid
    p.putString("rec_2", "oldest|110|70|60");     // 4 欄舊格式
    p.end();
  }
  BP_RecordManager m(5);
  m.loadFromStorage();
  CHECK_EQ(m.getRecordCount(), 3, "legacy count");
  CHECK_STR(m.getRecord(0).timestamp, "newest", "legacy order [0]");
  CHECK_STR(m.getRecord(1).timestamp, "middle", "legacy order [1]");
  CHECK_STR(m.getRecord(2).timestamp, "oldest", "legacy order [2]");
  CHECK_TRUE(!m.getRecord(1).valid, "legacy 5-field valid flag honored");
  CHECK_TRUE(m.getRecord(2).valid, "legacy 4-field valid inferred from values");

  // 遷移後回寫 v2
  {
    Preferences p;
    p.begin("bp_records", true);
    CHECK_STR(p.getString("schema", ""), "v2", "migration writes v2 schema");
    CHECK_TRUE(p.getString("slot_0", "").length() > 0, "migration writes slots");
    p.end();
  }

  // 遷移後 reload（走 v2 路徑）順序不變
  BP_RecordManager m2(5);
  m2.loadFromStorage();
  CHECK_STR(m2.getRecord(0).timestamp, "newest", "post-migration reload order");
  CHECK_EQ(m2.getRecordCount(), 3, "post-migration reload count");
}

// ---- S5: clearRecords ----
static void testClear() {
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("rec_0", "old|1|1|1|1"); // 殘留舊格式 key
    p.end();
  }
  BP_RecordManager m(3);
  m.loadFromStorage();
  m.addRecord(makeRecord("x", 120, 80, 65, true));
  m.clearRecords();
  CHECK_EQ(m.getRecordCount(), 0, "clear resets count");
  CHECK_TRUE(!m.getLatestRecord().valid, "clear resets records");

  BP_RecordManager m2(3);
  m2.loadFromStorage();
  CHECK_EQ(m2.getRecordCount(), 0, "clear persists (reload empty)");
  {
    Preferences p;
    p.begin("bp_records", true);
    CHECK_EQ((long)p.getString("rec_0", "").length(), 0L, "clear removes legacy keys");
    CHECK_EQ((long)p.getString("slot_0", "").length(), 0L, "clear removes v2 keys");
    p.end();
  }
}

int main() {
  testRingBuffer();
  testPersistenceRoundTrip();
  testCorruptionResilience();
  testLegacyMigration();
  testClear();
  return testReport();
}
