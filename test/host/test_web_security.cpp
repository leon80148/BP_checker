// Host-side spec tests for lib/WebSecurity.h
//
// 規格（SDD，設計與 codex 討論定案）：
//   S1. CSRF same-origin 檢查：Origin 存在時必須與 Host header 同 host:port
//       （大小寫不敏感、:80 視為預設可省略）；"null" 與格式錯誤一律拒絕；
//       Origin 缺席時 fallback 檢查 Referer；兩者皆缺（curl 等非瀏覽器）放行。
//       Host header 缺失但 Origin/Referer 存在 → 拒絕（無從比對）。
//   S2. 記錄索引參數採嚴格解析：非純數字（含 "12abc"、負數、空字串）回 -1，
//       避免 String::toInt() 把垃圾輸入變成 record 0。
//
// 執行：bash scripts/run_host_tests.sh

#include "lib/WebSecurity.h"
#include "test_support.h"

static void testCsrfCheck() {
  // 非瀏覽器請求（無 Origin / Referer）放行
  CHECK_TRUE(csrfCheckPasses(String(), String(), String("192.168.4.1")),
             "no origin/referer -> allow");

  // 同源允許
  CHECK_TRUE(csrfCheckPasses(String("http://192.168.4.1"), String(), String("192.168.4.1")),
             "matching origin -> allow");
  CHECK_TRUE(csrfCheckPasses(String("http://192.168.4.1:80"), String(), String("192.168.4.1")),
             "origin with default :80 -> allow");
  CHECK_TRUE(csrfCheckPasses(String("http://192.168.4.1"), String(), String("192.168.4.1:80")),
             "host with default :80 -> allow");
  CHECK_TRUE(csrfCheckPasses(String("http://BP_checker.local"), String(), String("bp_checker.local")),
             "case-insensitive host -> allow");

  // 跨源 / 異常拒絕
  CHECK_TRUE(!csrfCheckPasses(String("http://evil.example"), String(), String("192.168.4.1")),
             "cross-origin -> reject");
  CHECK_TRUE(!csrfCheckPasses(String("http://192.168.4.1:8080"), String(), String("192.168.4.1")),
             "port mismatch -> reject");
  CHECK_TRUE(!csrfCheckPasses(String("null"), String(), String("192.168.4.1")),
             "Origin null -> reject");
  CHECK_TRUE(!csrfCheckPasses(String("garbage-no-scheme"), String(), String("192.168.4.1")),
             "malformed origin -> reject");
  CHECK_TRUE(!csrfCheckPasses(String("http://192.168.4.1"), String(), String()),
             "origin present but Host missing -> reject");

  // Referer fallback（Origin 缺席時）
  CHECK_TRUE(csrfCheckPasses(String(), String("http://192.168.4.1/history"), String("192.168.4.1")),
             "matching referer -> allow");
  CHECK_TRUE(!csrfCheckPasses(String(), String("http://evil.example/x"), String("192.168.4.1")),
             "cross-origin referer -> reject");

  // Origin 存在時 Referer 不參與判定
  CHECK_TRUE(!csrfCheckPasses(String("http://evil.example"), String("http://192.168.4.1/"), String("192.168.4.1")),
             "bad origin not rescued by good referer");
}

static void testParseIndexParam() {
  CHECK_EQ(parseIndexParam(String("0")), 0, "index 0");
  CHECK_EQ(parseIndexParam(String("19")), 19, "index 19");
  CHECK_EQ(parseIndexParam(String("abc")), -1, "letters -> -1");
  CHECK_EQ(parseIndexParam(String("12abc")), -1, "trailing letters -> -1");
  CHECK_EQ(parseIndexParam(String("-1")), -1, "negative -> -1");
  CHECK_EQ(parseIndexParam(String("")), -1, "empty -> -1");
  CHECK_EQ(parseIndexParam(String(" 3")), -1, "leading space -> -1");
  CHECK_EQ(parseIndexParam(String("9999999")), -1, "overlong -> -1");
}

int main() {
  testCsrfCheck();
  testParseIndexParam();
  return testReport();
}
