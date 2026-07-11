// Host-side spec tests for lib/CsvExport.h
//
// 規格（SDD，設計與 codex 討論定案）：
//   S1. 輸出 = UTF-8 BOM + 中文標題列 + 資料列；CRLF 行尾（Excel 相容）。
//   S2. 所有欄位以雙引號包裹；欄位內的 '"' 以 "" 跳脫。
//   S3. 資料列由舊到新（時間升冪，供診所存檔試算表接續使用）。
//   S4. invalid 記錄（legacy -1 資料）不輸出 —— 這是臨床報表不是診斷 dump。
//
// 執行：bash scripts/run_host_tests.sh

#include <cstring>

#include "lib/CsvExport.h"
#include "test_support.h"

static BPData makeRecord(const char* ts, int sys, int dia, int pul, bool valid) {
  BPData d;
  d.timestamp = ts;
  d.timestampSource = valid
    ? BPTimestampSource::DEVICE
    : BPTimestampSource::LEGACY_UNSYNCED;
  d.systolic = sys;
  d.diastolic = dia;
  d.pulse = pul;
  d.valid = valid;
  return d;
}

static bool contains(const String& s, const char* needle) {
  return strstr(s.c_str(), needle) != nullptr;
}

static void testEmptyHistory() {
  Preferences::__reset();
  BP_RecordManager m(5);
  String csv;
  appendHistoryCsv(csv, m);
  CHECK_STR(csv,
            "\xEF\xBB\xBF\"測量時間\",\"收縮壓(mmHg)\",\"舒張壓(mmHg)\",\"脈搏(bpm)\"\r\n",
            "empty history -> BOM + header only");
}

static void testRowsOldestFirstAndQuoted() {
  Preferences::__reset();
  BP_RecordManager m(5);
  m.addRecord(makeRecord("2026-07-01 09:00:00", 118, 76, 64, true));
  m.addRecord(makeRecord("2026-07-02 10:30:00", 132, 84, 70, true));

  String csv;
  appendHistoryCsv(csv, m);
  CHECK_STR(csv,
            "\xEF\xBB\xBF\"測量時間\",\"收縮壓(mmHg)\",\"舒張壓(mmHg)\",\"脈搏(bpm)\"\r\n"
            "\"2026-07-01 09:00:00\",\"118\",\"76\",\"64\"\r\n"
            "\"2026-07-02 10:30:00\",\"132\",\"84\",\"70\"\r\n",
            "rows oldest-first, all fields quoted, CRLF");
}

static void testInvalidRecordsSkipped() {
  Preferences::__reset();
  BP_RecordManager m(5);
  m.addRecord(makeRecord("2026-07-01 09:00:00", 118, 76, 64, true));
  m.addRecord(makeRecord("時間未同步", -1, -1, -1, false)); // legacy invalid
  m.addRecord(makeRecord("2026-07-02 10:30:00", 132, 84, 70, true));

  String csv;
  appendHistoryCsv(csv, m);
  CHECK_TRUE(!contains(csv, "-1"), "invalid record not exported");
  CHECK_TRUE(contains(csv, "\"118\""), "valid rows still exported");
  CHECK_TRUE(contains(csv, "\"132\""), "valid rows after invalid still exported");
}

static void testQuoteEscaping() {
  String csv;
  __appendCsvField(csv, String("t\"x")); // 防禦性純函式測試
  CHECK_STR(csv, "\"t\"\"x\"", "embedded quote doubled");
}

int main() {
  testEmptyHistory();
  testRowsOldestFirstAndQuoted();
  testInvalidRecordsSkipped();
  testQuoteEscaping();
  return testReport();
}
