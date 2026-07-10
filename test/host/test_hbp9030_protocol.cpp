#include <cstring>

#include "lib/BPProtocol.h"
#include "lib/BP_Parser.h"
#include "test_support.h"

static const char* kId = "12345678901234567890";
static const char* kValid =
  "2026,07,11,09,05,12345678901234567890,0,120,080,072,0";

static BPParseResult parseText(const char* payload) {
  BP_Parser parser{String("OMRON-HBP9030")};
  return parser.parseResult(reinterpret_cast<const uint8_t*>(payload),
                            static_cast<int>(strlen(payload)));
}

static String frame(const char* date, const char* id, const char* error,
                    const char* sys, const char* dia, const char* pulse,
                    const char* movement) {
  String value(date);
  value += ",";
  value += id;
  value += ",";
  value += error;
  value += ",";
  value += sys;
  value += ",";
  value += dia;
  value += ",";
  value += pulse;
  value += ",";
  value += movement;
  return value;
}

static BPParseResult parseFrame(const String& payload) {
  BP_Parser parser{String("OMRON-HBP9030")};
  return parser.parseResult(
    reinterpret_cast<const uint8_t*>(payload.c_str()),
    static_cast<int>(payload.length()));
}

static void expectError(const char* payload, BPParseError error,
                        const char* label) {
  BPParseResult result = parseText(payload);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(error), label);
  CHECK_TRUE(!result.measurement.valid, "error result is never valid");
}

static void testCanonicalFrame() {
  CHECK_EQ(static_cast<int>(strlen(kValid)), 53, "canonical payload is 53 bytes");

  BPParseResult result = parseText(kValid);
  CHECK_EQ(static_cast<int>(result.error), static_cast<int>(BPParseError::NONE),
           "canonical parse status");
  CHECK_TRUE(result.measurement.valid, "canonical frame valid");
  CHECK_EQ(result.measurement.systolic, 120, "canonical SYS");
  CHECK_EQ(result.measurement.diastolic, 80, "canonical DIA");
  CHECK_EQ(result.measurement.pulse, 72, "canonical pulse");
  CHECK_EQ(result.measurement.movementCount, 0, "canonical movement");
  CHECK_EQ(static_cast<int>(result.measurement.timestampSource),
           static_cast<int>(BPTimestampSource::DEVICE), "device time source");
  CHECK_STR(result.measurement.timestamp, "2026-07-11 09:05:00",
            "device timestamp retained");
  CHECK_STR(result.transientSubjectId, kId, "subject ID returned separately");
  CHECK_EQ(static_cast<int>(result.measurement.rawData.length()), 0,
           "persistable measurement has no raw identity");
}

static void testCalendarAndId() {
  BPParseResult result = parseFrame(frame("2024,02,29,23,59",
                                          "                    ",
                                          "0", "120", "080", "072", "0"));
  CHECK_TRUE(result.measurement.valid, "leap day and padded blank ID valid");
  CHECK_STR(result.transientSubjectId, "                    ",
            "blank padded ID stays transient");

  expectError("2025,02,29,09,05,12345678901234567890,0,120,080,072,0",
              BPParseError::MALFORMED, "non-leap February 29 rejected");
  expectError("2026,13,11,09,05,12345678901234567890,0,120,080,072,0",
              BPParseError::MALFORMED, "month 13 rejected");
  expectError("2026,07,11,24,05,12345678901234567890,0,120,080,072,0",
              BPParseError::MALFORMED, "hour 24 rejected");
  expectError("2026,07,11,09,60,12345678901234567890,0,120,080,072,0",
              BPParseError::MALFORMED, "minute 60 rejected");
  expectError("2026,07,11,09,05,1234567890123456789,0,120,080,072,0",
              BPParseError::MALFORMED, "short ID rejected");
  expectError("2026,07,11,09,05,123456789012345678901,0,120,080,072,0",
              BPParseError::MALFORMED, "long ID rejected");
}

static void testStrictGrammar() {
  expectError("2026,07,11,09,05,12345678901234567890,0,120,080,072",
              BPParseError::MALFORMED, "missing field rejected");
  expectError("2026,07,11,09,05,12345678901234567890,0,120,080,072,0,9",
              BPParseError::MALFORMED, "extra field rejected");
  expectError("2026,07,11,09,05,12345678901234567890,0,+20,080,072,0",
              BPParseError::MALFORMED, "sign rejected");
  expectError("2026,07,11,09,05,12345678901234567890,0, 20,080,072,0",
              BPParseError::MALFORMED, "numeric whitespace rejected");
  expectError("2026,07,11,09,05,12345678901234567890,0,120junk,080,072,0",
              BPParseError::MALFORMED, "numeric prefix rejected");
  expectError("2026;07,11,09,05,12345678901234567890,0,120,080,072,0",
              BPParseError::MALFORMED, "misplaced comma rejected");
  expectError("2026,07,11,09,05,12345678901234567890,0,120,080,072,0\r\n",
              BPParseError::MALFORMED, "parser rejects CRLF delimiters");

  uint8_t embeddedNul[53];
  memcpy(embeddedNul, kValid, sizeof(embeddedNul));
  embeddedNul[25] = 0;
  BP_Parser parser{String("OMRON-HBP9030")};
  BPParseResult result = parser.parseResult(embeddedNul, sizeof(embeddedNul));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::MALFORMED), "embedded NUL rejected");
}

static void testRanges() {
  BPParseResult result = parseFrame(frame("2026,07,11,09,05", kId, "0",
                                          "060", "030", "040", "0"));
  CHECK_TRUE(result.measurement.valid, "inclusive lower ranges valid");
  result = parseFrame(frame("2026,07,11,09,05", kId, "0",
                            "260", "215", "180", "0"));
  CHECK_TRUE(result.measurement.valid, "inclusive upper ranges valid");

  const char* low[] = {"059", "029", "039"};
  const char* high[] = {"261", "216", "181"};
  for (int i = 0; i < 3; ++i) {
    const char* sys = i == 0 ? low[i] : "120";
    const char* dia = i == 1 ? low[i] : "080";
    const char* pul = i == 2 ? low[i] : "072";
    result = parseFrame(frame("2026,07,11,09,05", kId, "0",
                              sys, dia, pul, "0"));
    CHECK_EQ(static_cast<int>(result.error),
             static_cast<int>(BPParseError::OUT_OF_RANGE),
             "one-below range rejected");

    sys = i == 0 ? high[i] : "120";
    dia = i == 1 ? high[i] : "080";
    pul = i == 2 ? high[i] : "072";
    result = parseFrame(frame("2026,07,11,09,05", kId, "0",
                              sys, dia, pul, "0"));
    CHECK_EQ(static_cast<int>(result.error),
             static_cast<int>(BPParseError::OUT_OF_RANGE),
             "one-above range rejected");
  }
}

static void testMonitorErrorAndMovement() {
  BPParseResult result = parseFrame(frame("2026,07,11,09,05", kId, "7",
                                          "   ", "   ", "   ", "0"));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::DEVICE_ERROR),
           "space-filled error frame classified");
  CHECK_EQ(result.deviceErrorCode, 7, "device error code retained");
  CHECK_TRUE(!result.measurement.valid, "error frame not valid");

  result = parseFrame(frame("2026,07,11,09,05", kId, "7",
                            "120", "080", "072", "0"));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::DEVICE_ERROR),
           "numeric error frame still DEVICE_ERROR");
  CHECK_TRUE(!result.measurement.valid, "numeric error frame never valid");

  result = parseFrame(frame("2026,07,11,09,05", kId, "7",
                            "120", "   ", "072", "0"));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::MALFORMED),
           "mixed error vitals malformed");

  result = parseFrame(frame("2026,07,11,09,05", kId, "0",
                            "120", "080", "072", "1"));
  CHECK_TRUE(result.measurement.valid, "motion reading remains a measurement");
  CHECK_EQ(result.measurement.movementCount, 1, "motion count retained");
  CHECK_EQ(static_cast<int>(result.measurement.quality),
           static_cast<int>(BPMeasurementQuality::MOTION),
           "motion quality warning retained");
}

static void testUnsupportedModels() {
  const uint8_t bytes[] = {'1', ',', '2'};
  BP_Parser custom{String("CUSTOM")};
  BPParseResult result = custom.parseResult(bytes, sizeof(bytes));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::UNSUPPORTED_MODEL),
           "CUSTOM disabled in production");

  BP_Parser unknown{String("UNKNOWN")};
  result = unknown.parseResult(bytes, sizeof(bytes));
  CHECK_EQ(static_cast<int>(result.error),
           static_cast<int>(BPParseError::UNSUPPORTED_MODEL),
           "unknown model never falls back");
}

static void testUnsupportedHbpFormats() {
  const char* oldFormats[] = {
    "MMBP203N\r2026.07.11\r120\r080\r072\r",
    "ID99999999B26/07/11/09:05 120 080 072 ",
    "bp,12345678901234567890,2026/07/11,09:05,120,090,080,072,0\r",
  };
  for (const char* payload : oldFormats) {
    BPParseResult result = parseText(payload);
    CHECK_EQ(static_cast<int>(result.error),
             static_cast<int>(BPParseError::UNSUPPORTED_FORMAT),
             "HBP formats 1-4 report unsupported format");
    CHECK_TRUE(!result.measurement.valid, "unsupported format not valid");
  }
}

int main() {
  testCanonicalFrame();
  testCalendarAndId();
  testStrictGrammar();
  testRanges();
  testMonitorErrorAndMovement();
  testUnsupportedModels();
  testUnsupportedHbpFormats();
  return testReport();
}
