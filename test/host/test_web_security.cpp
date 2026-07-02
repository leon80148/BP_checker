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

// S1b. DNS rebinding 防護（codex review P1）：Origin==Host 不夠 —— 攻擊者
//      可讓自己的網域解析到裝置 IP，此時兩者一致但都不是裝置身分。
//      Host 必須命中裝置自身身分 allowlist（AP IP / STA IP / mDNS 主機名，
//      大小寫不敏感、:80 視為預設）。
static void testHostIsDevice() {
  String apIp("192.168.4.1"), staIp("192.168.1.50"), mdns("bp_checker.local");

  CHECK_TRUE(hostIsDevice(String("192.168.4.1"), apIp, staIp, mdns), "AP IP -> allow");
  CHECK_TRUE(hostIsDevice(String("192.168.1.50"), apIp, staIp, mdns), "STA IP -> allow");
  CHECK_TRUE(hostIsDevice(String("bp_checker.local"), apIp, staIp, mdns), "mDNS name -> allow");
  CHECK_TRUE(hostIsDevice(String("BP_checker.local"), apIp, staIp, mdns), "mDNS case-insensitive");
  CHECK_TRUE(hostIsDevice(String("192.168.4.1:80"), apIp, staIp, mdns), "default :80 -> allow");

  CHECK_TRUE(!hostIsDevice(String("evil.example"), apIp, staIp, mdns), "foreign host -> reject");
  CHECK_TRUE(!hostIsDevice(String("192.168.4.1:8080"), apIp, staIp, mdns), "other port -> reject");
  CHECK_TRUE(!hostIsDevice(String(), apIp, staIp, mdns), "missing Host -> reject");
  CHECK_TRUE(!hostIsDevice(String("192.168.1.50"), apIp, String(), mdns),
             "stale STA IP after disconnect -> reject");
}

// S3. 管理密碼（PIN）：未設定（stored 空）→ 一律放行（向後相容）；
//     已設定 → 必須完全相符。格式限 4-16 個可見 ASCII 字元。
static void testPinCheck() {
  CHECK_TRUE(pinCheckPasses(String(), String()), "no pin configured -> allow");
  CHECK_TRUE(pinCheckPasses(String("whatever"), String()), "no pin ignores provided");
  CHECK_TRUE(pinCheckPasses(String("1234"), String("1234")), "matching pin -> allow");
  CHECK_TRUE(!pinCheckPasses(String("0000"), String("1234")), "wrong pin -> reject");
  CHECK_TRUE(!pinCheckPasses(String(), String("1234")), "missing pin -> reject");
  CHECK_TRUE(!pinCheckPasses(String("12345"), String("1234")), "prefix+extra -> reject");
}

static void testIsValidPin() {
  CHECK_TRUE(isValidPin(String("1234")), "4 digits ok");
  CHECK_TRUE(isValidPin(String("abcDEF1234567890")), "16 chars ok");
  CHECK_TRUE(isValidPin(String("p@ss-w0rd!")), "symbols ok");
  CHECK_TRUE(!isValidPin(String("123")), "too short");
  CHECK_TRUE(!isValidPin(String("12345678901234567")), "too long");
  CHECK_TRUE(!isValidPin(String("ab cd")), "space rejected");
  CHECK_TRUE(!isValidPin(String("")), "empty rejected");
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
  testHostIsDevice();
  testPinCheck();
  testIsValidPin();
  testParseIndexParam();
  return testReport();
}
