// Host-side compatibility and dispatch tests for lib/BP_Parser.h.
// The full HBP-9030 format-5 corpus lives in test_hbp9030_protocol.cpp.

#include <cstring>

#include "lib/BP_Parser.h"
#include "test_support.h"

static const char* kValid =
  "2026,07,11,09,05,12345678901234567890,0,120,080,072,0";

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
  CHECK_EQ(static_cast<int>(s.charAt(99)), 0, "charAt out of range -> NUL");

  String t("  a\r\n");
  t.trim();
  CHECK_TRUE(t == "a", "trim removes spaces and CRLF");

  String c;
  c.reserve(8);
  c.concat("ab,cd", 5);
  CHECK_EQ(static_cast<long>(c.length()), 5L, "concat(ptr,len) length");
  CHECK_EQ(static_cast<long>(c.indexOf(',')), 2L, "indexOf char");
  CHECK_EQ(static_cast<long>(c.indexOf("cd")), 3L, "indexOf cstr");
  CHECK_EQ(static_cast<long>(c.indexOf('x')), -1L, "indexOf miss -> -1");

  String n;
  n += 120;
  CHECK_TRUE(n == "120", "operator+=(int) appends decimal text");
}

static void testCompatibilityAndDispatch() {
  BP_Parser parser{String("OMRON-HBP9030")};
  BPData data = parser.parse(reinterpret_cast<const uint8_t*>(kValid),
                             static_cast<int>(strlen(kValid)));
  CHECK_TRUE(data.valid, "compatibility parse returns strict measurement");
  CHECK_EQ(data.systolic, 120, "compatibility SYS");
  CHECK_EQ(data.diastolic, 80, "compatibility DIA");
  CHECK_EQ(data.pulse, 72, "compatibility pulse");
  CHECK_TRUE(parser.isLineDelimited(), "HBP-9030 is line-delimited");

  const uint8_t unsupported[] = {'S', 'Y', 'S', ':', '1'};
  parser.setModel(String("CUSTOM"));
  BPParseResult result = parser.parseResult(unsupported, sizeof(unsupported));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::UNSUPPORTED_MODEL),
           "CUSTOM cannot silently parse in production");
  CHECK_TRUE(!parser.isLineDelimited(), "unsupported model has no framing claim");

  parser.setModel(String("OMRON-HBP1300"));
  result = parser.parseResult(unsupported, sizeof(unsupported));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::UNSUPPORTED_MODEL),
           "unverified binary model disabled");

  parser.setModel(String("OMRON-HBP9030"));
  result = parser.parseResult(reinterpret_cast<const uint8_t*>(kValid),
                              static_cast<int>(strlen(kValid)));
  CHECK_TRUE(result.ok(), "setModel restores supported parser");
}

int main() {
  testStringShim();
  testCompatibilityAndDispatch();
  return testReport();
}
