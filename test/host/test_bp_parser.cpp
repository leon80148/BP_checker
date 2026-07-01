// Host-side spec tests for lib/BP_Parser.h
//
// 規格（SDD）：
//   S1. String shim 必須複製 Arduino String 的關鍵語意（toInt 無效→0、
//       substring 參數顛倒時互換、charAt 越界回 '\0'、trim 去 CR/LF）。
//   S2. OMRON-HBP9030：CSV ≥10 欄，第 7/8/9 欄（0-indexed）= SYS/DIA/PUL；
//       欄位不足或數值非正 → valid=false。CRLF 結尾與欄位空白必須容忍。
//   S3. Generic parser：支援 "SYS:x,DIA:y,PUL:z" 與 "BP: x/y, PR: z" 兩種
//       ASCII 格式；不可解析輸入 → valid=false，不可 crash。
//   S4. parse() 統一判定 valid（SYS/DIA/PUL 皆 > 0）。
//
// 執行：bash scripts/run_host_tests.sh

#include <cstring>

#include "lib/BP_Parser.h"
#include "test_support.h"

static BPData parseWith(const char* model, const char* payload) {
  BP_Parser parser{String(model)};
  return parser.parse(reinterpret_cast<const uint8_t*>(payload),
                      (int)strlen(payload));
}

// ---- S1: String shim 語意 ----
static void testStringShim() {
  CHECK_EQ(String("123").toInt(), 123L, "toInt parses digits");
  CHECK_EQ(String("abc").toInt(), 0L, "toInt invalid -> 0");
  CHECK_EQ(String("  42").toInt(), 42L, "toInt skips leading spaces");
  CHECK_EQ(String("-8").toInt(), -8L, "toInt negative");
  CHECK_EQ(String("72xyz").toInt(), 72L, "toInt stops at non-digit");

  String s("hello");
  CHECK_TRUE(s.substring(3, 1) == "el", "substring swaps reversed args");
  CHECK_TRUE(s.substring(1, 99) == "ello", "substring clamps end");
  CHECK_TRUE(s.substring(2) == "llo", "substring to end");
  CHECK_EQ((int)s.charAt(99), 0, "charAt out of range -> NUL");

  String t("  a\r\n");
  t.trim();
  CHECK_TRUE(t == "a", "trim removes spaces and CRLF");

  String c;
  c.reserve(8);
  c.concat("ab,cd", 5);
  CHECK_EQ((long)c.length(), 5L, "concat(ptr,len) length");
  CHECK_EQ((long)c.indexOf(','), 2L, "indexOf char");
  CHECK_EQ((long)c.indexOf("cd"), 3L, "indexOf cstr");
  CHECK_EQ((long)c.indexOf('x'), -1L, "indexOf miss -> -1");

  String n;
  n += 120;
  CHECK_TRUE(n == "120", "operator+=(int) appends decimal text");
}

// ---- S2: OMRON-HBP9030 ----
static void testHbp9030() {
  BPData d = parseWith("OMRON-HBP9030", "1,2,3,4,5,6,7,120,80,72");
  CHECK_EQ(d.systolic, 120, "HBP9030 SYS from field 7");
  CHECK_EQ(d.diastolic, 80, "HBP9030 DIA from field 8");
  CHECK_EQ(d.pulse, 72, "HBP9030 PUL from field 9");
  CHECK_TRUE(d.valid, "HBP9030 full frame valid");

  d = parseWith("OMRON-HBP9030", "1,2,3,4,5,6,7,120,80,72\r\n");
  CHECK_EQ(d.pulse, 72, "HBP9030 tolerates CRLF tail");
  CHECK_TRUE(d.valid, "HBP9030 CRLF frame valid");

  d = parseWith("OMRON-HBP9030", "1,2,3,4,5,6,7, 120 , 80 , 72");
  CHECK_EQ(d.systolic, 120, "HBP9030 tolerates padded fields");

  d = parseWith("OMRON-HBP9030", "1,2,3,120,80,72");
  CHECK_TRUE(!d.valid, "HBP9030 short frame invalid");

  d = parseWith("OMRON-HBP9030", "1,2,3,4,5,6,7,0,0,0");
  CHECK_TRUE(!d.valid, "HBP9030 zero vitals invalid");

  d = parseWith("OMRON-HBP9030", "");
  CHECK_TRUE(!d.valid, "HBP9030 empty frame invalid");
}

// ---- S3: Generic parser ----
static void testGeneric() {
  BPData d = parseWith("CUSTOM", "SYS:120,DIA:80,PUL:75");
  CHECK_EQ(d.systolic, 120, "generic SYS: format");
  CHECK_EQ(d.diastolic, 80, "generic DIA: format");
  CHECK_EQ(d.pulse, 75, "generic PUL: format");
  CHECK_TRUE(d.valid, "generic SYS/DIA/PUL valid");

  d = parseWith("CUSTOM", "BP: 120/80, PR: 75");
  CHECK_EQ(d.systolic, 120, "generic BP: sys");
  CHECK_EQ(d.diastolic, 80, "generic BP: dia");
  CHECK_EQ(d.pulse, 75, "generic PR: pulse");
  CHECK_TRUE(d.valid, "generic BP/PR valid");

  d = parseWith("CUSTOM", "hello world");
  CHECK_TRUE(!d.valid, "generic garbage invalid");

  d = parseWith("CUSTOM", "SYS:,DIA:80,PUL:75");
  CHECK_TRUE(!d.valid, "generic empty SYS invalid");

  // 不可列印 bytes 混入時要被過濾、不可 crash
  const uint8_t noisy[] = {0x02, 'S', 'Y', 'S', ':', '9', '9', ',',
                           'D', 'I', 'A', ':', '6', '6', ',',
                           'P', 'U', 'L', ':', '7', '0', 0x03};
  BP_Parser parser{String("CUSTOM")};
  d = parser.parse(noisy, (int)sizeof(noisy));
  CHECK_EQ(d.systolic, 99, "generic filters non-printable bytes");
  CHECK_TRUE(d.valid, "generic noisy frame valid");
}

// ---- S4: 型號分派與 valid 統一判定 ----
static void testDispatchAndValidity() {
  // 未知型號 → generic
  BPData d = parseWith("UNKNOWN-MODEL", "SYS:110,DIA:70,PUL:65");
  CHECK_TRUE(d.valid, "unknown model falls back to generic");

  // HBP-1300 binary frame: 0x01 header, big-endian 16-bit values
  const uint8_t frame1300[] = {0x01, 0x00, 0x00, 0x78, 0x00, 0x50,
                               0x00, 0x48, 0x00, 0x00};
  BP_Parser p1300{String("OMRON-HBP1300")};
  d = p1300.parse(frame1300, (int)sizeof(frame1300));
  CHECK_EQ(d.systolic, 120, "HBP1300 sys big-endian");
  CHECK_EQ(d.diastolic, 80, "HBP1300 dia big-endian");
  CHECK_EQ(d.pulse, 72, "HBP1300 pulse big-endian");
  CHECK_TRUE(d.valid, "HBP1300 frame valid");

  BP_Parser pm{String("OMRON-HBP9030")};
  pm.setModel(String("CUSTOM"));
  d = pm.parse(reinterpret_cast<const uint8_t*>("SYS:100,DIA:60,PUL:60"), 21);
  CHECK_TRUE(d.valid, "setModel switches dispatch");
}

int main() {
  testStringShim();
  testHbp9030();
  testGeneric();
  testDispatchAndValidity();
  return testReport();
}
