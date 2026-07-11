#include <cstring>
#include <utility>

#include "lib/MeasurementPolicy.h"
#include "lib/BPRecordManager.h"
#include "test_support.h"

static BPData reading(int systolic, int diastolic, int pulse,
                      bool valid = true) {
  BPData value;
  value.timestamp = "2026-07-11 09:05:00";
  value.timestampSource = BPTimestampSource::DEVICE;
  value.systolic = systolic;
  value.diastolic = diastolic;
  value.pulse = pulse;
  value.valid = valid;
  return value;
}

static bool containsForbiddenDiagnosis(const char* value) {
  return value != nullptr &&
    (std::strstr(value, "正常") != nullptr ||
     std::strstr(value, "異常") != nullptr);
}

static void testConfiguredReviewAndUrgentBoundaries() {
  MeasurementPolicyConfig policy;
  policy.reviewSystolic = 135;
  policy.reviewDiastolic = 85;
  policy.reviewPulseLow = 55;
  policy.reviewPulseHigh = 105;
  policy.urgentSystolic = 180;
  policy.urgentDiastolic = 120;

  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(134, 84, 55), policy)),
           static_cast<int>(MeasurementReviewState::WITHIN_REFERENCE),
           "values below configured review boundaries stay in reference range");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(135, 84, 70), policy)),
           static_cast<int>(MeasurementReviewState::REVIEW),
           "configured systolic boundary requests review");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(120, 85, 70), policy)),
           static_cast<int>(MeasurementReviewState::REVIEW),
           "configured diastolic boundary requests review");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(120, 70, 54), policy)),
           static_cast<int>(MeasurementReviewState::REVIEW),
           "pulse below configured lower boundary requests review");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(120, 70, 106), policy)),
           static_cast<int>(MeasurementReviewState::REVIEW),
           "pulse above configured upper boundary requests review");

  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(180, 120, 70), policy)),
           static_cast<int>(MeasurementReviewState::REVIEW),
           "AHA 180/120 values remain at the review boundary");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(181, 80, 70), policy)),
           static_cast<int>(MeasurementReviewState::URGENT),
           "systolic above the urgent boundary is urgent");
  CHECK_EQ(static_cast<int>(classifyMeasurement(reading(120, 121, 70), policy)),
           static_cast<int>(MeasurementReviewState::URGENT),
           "diastolic above the urgent boundary is urgent");
}

static void testInvalidValuesFailClosed() {
  const BPData invalidValues[] = {
    reading(59, 80, 70), reading(261, 80, 70),
    reading(120, 29, 70), reading(120, 216, 70),
    reading(120, 80, 39), reading(120, 80, 181),
    reading(120, 80, 70, false),
  };
  for (const BPData& value : invalidValues) {
    CHECK_EQ(static_cast<int>(classifyMeasurement(value)),
             static_cast<int>(MeasurementReviewState::INVALID),
             "invalid or parser-out-of-range measurement fails closed");
  }
}

static void testFreshnessTransitions() {
  MeasurementFreshnessInput input;
  input.hasRecord = true;
  input.valid = true;
  input.receivedThisBoot = false;
  input.transportConnected = true;
  input.nowMs = 100000;
  input.lastSuccessfulReceiveMs = 0;
  input.staleAfterMs = 300000;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::HISTORICAL),
           "persisted boot history is historical even with a connected transport");

  input.receivedThisBoot = true;
  input.lastSuccessfulReceiveMs = 1000;
  input.nowMs = 300999;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::CURRENT),
           "accepted current-boot reading stays current before stale boundary");
  input.nowMs = 301000;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::STALE),
           "current reading becomes stale at configured interval");

  input.transportConnected = false;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::DISCONNECTED),
           "current-boot reading is disconnected when transport is unavailable");

  input.valid = false;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::INVALID),
           "invalid reading takes precedence over transport state");
  input.hasRecord = false;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::INVALID),
           "missing reading has explicit invalid freshness state");
}

static void testRevisionAndReceiveAgeSurviveRingWrapSafely() {
  Preferences::__reset();
  __millisCounter() = 0;
  BP_RecordManager manager(2);
  CHECK_TRUE(manager.loadFromStorage(), "fresh store initializes");
  CHECK_EQ(manager.getRevision(), 0ULL, "empty store starts at revision zero");

  CHECK_TRUE(manager.addRecord(reading(120, 80, 70)), "first reading persists");
  CHECK_EQ(manager.getRevision(), 1ULL, "record sequence is the first revision");
  delay(25);
  CHECK_TRUE(manager.addRecord(reading(121, 81, 71)), "second reading persists");
  delay(25);
  CHECK_TRUE(manager.addRecord(reading(122, 82, 72)), "ring-wrapping reading persists");
  CHECK_EQ(manager.getRecordCount(), 2, "ring remains bounded after wrap");
  CHECK_EQ(manager.getRevision(), 3ULL,
           "opaque uint64 record revision advances after ring wrap");
  CHECK_TRUE(manager.latestReceivedThisBoot(),
             "latest accepted record is current-boot data");
  uint32_t age = 99;
  CHECK_TRUE(manager.lastSuccessfulReceiveAgeMs(millis(), age),
             "current boot exposes bounded receive age");
  CHECK_EQ(age, 0U, "age starts at zero after durable receive");
  delay(40);
  CHECK_TRUE(manager.lastSuccessfulReceiveAgeMs(millis(), age),
             "receive age remains available");
  CHECK_EQ(age, 40U, "receive age uses wrap-safe millis subtraction");

  BP_RecordManager rebooted(2);
  CHECK_TRUE(rebooted.loadFromStorage(), "persisted ring reloads");
  CHECK_EQ(rebooted.getRevision(), 3ULL,
           "persisted opaque revision remains suitable for change detection");
  CHECK_TRUE(!rebooted.latestReceivedThisBoot(),
             "persisted latest record is never marked current");
  CHECK_TRUE(!rebooted.lastSuccessfulReceiveAgeMs(millis(), age),
             "persisted history does not invent a receive age");
}

static void testResultWordingAndRepeatGuidance() {
  const MeasurementReviewState reviewStates[] = {
    MeasurementReviewState::INVALID,
    MeasurementReviewState::WITHIN_REFERENCE,
    MeasurementReviewState::REVIEW,
    MeasurementReviewState::URGENT,
  };
  for (MeasurementReviewState state : reviewStates) {
    CHECK_TRUE(!containsForbiddenDiagnosis(measurementReviewLabel(state)),
               "review result never diagnoses normal or abnormal");
  }
  const MeasurementFreshnessState freshnessStates[] = {
    MeasurementFreshnessState::INVALID,
    MeasurementFreshnessState::CURRENT,
    MeasurementFreshnessState::STALE,
    MeasurementFreshnessState::HISTORICAL,
    MeasurementFreshnessState::DISCONNECTED,
  };
  for (MeasurementFreshnessState state : freshnessStates) {
    CHECK_TRUE(!containsForbiddenDiagnosis(measurementFreshnessLabel(state)),
               "freshness result never diagnoses normal or abnormal");
  }
  CHECK_TRUE(std::strstr(repeatedMeasurementGuidance(), "1 分鐘") != nullptr,
             "guidance asks for a second reading one minute apart");
  CHECK_TRUE(std::strstr(repeatedMeasurementGuidance(), "不會自動平均") != nullptr,
             "guidance says unidentified readings are not automatically averaged");
  CHECK_TRUE(!containsForbiddenDiagnosis(repeatedMeasurementGuidance()),
             "repeat guidance contains no diagnostic normal/abnormal wording");
  CHECK_TRUE(std::strstr(measurementReferencePolicyName(), "2025") != nullptr,
             "configured reference policy names its guideline version");
  CHECK_TRUE(std::strstr(measurementReferencePolicyName(), "血壓參考") != nullptr &&
             std::strstr(measurementReferencePolicyName(), "脈搏規則由診所設定") != nullptr,
             "policy attributes only blood pressure thresholds to AHA");
}

static void testOpaqueSequenceDecimalFormatting() {
  char encoded[24] = {};
  CHECK_TRUE(formatOpaqueSequence(UINT64_MAX, encoded, sizeof(encoded)),
             "maximum uint64 opaque sequence formats exactly");
  CHECK_TRUE(std::strcmp(encoded, "18446744073709551615") == 0,
             "opaque sequence decimal text preserves values above JSON safe integer");
  char tooSmall[20] = {};
  CHECK_TRUE(!formatOpaqueSequence(UINT64_MAX, tooSmall, sizeof(tooSmall)),
             "opaque sequence formatting rejects truncation");
}

int main() {
  testConfiguredReviewAndUrgentBoundaries();
  testInvalidValuesFailClosed();
  testFreshnessTransitions();
  testRevisionAndReceiveAgeSurviveRingWrapSafely();
  testResultWordingAndRepeatGuidance();
  testOpaqueSequenceDecimalFormatting();
  return testReport();
}
