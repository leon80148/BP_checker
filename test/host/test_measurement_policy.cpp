#include <cstring>
#include <utility>
#include <vector>

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

static int policyResult(MeasurementPolicyResult result) {
  return static_cast<int>(result);
}

static MeasurementPolicyConfig updatedPolicy(
    const MeasurementPolicyConfig& current) {
  MeasurementPolicyConfig next = current;
  CHECK_TRUE(copyMeasurementPolicyName(next, "Clinic-A Adult Review"),
             "test policy name fits validated storage");
  next.policyVersion = current.policyVersion + 1U;
  next.reviewSystolic = 135;
  next.reviewDiastolic = 85;
  next.reviewPulseLow = 55;
  next.reviewPulseHigh = 105;
  next.urgentSystolic = 181;
  next.urgentDiastolic = 121;
  next.staleAfterMs = 600000U;
  return next;
}

static void testAtomicPolicyPersistenceAndRoundTrip() {
  Preferences::__reset();
  Preferences preferences;
  MeasurementPolicyStore store(&preferences);
  CHECK_EQ(policyResult(store.loadOrCreate()),
           policyResult(MeasurementPolicyResult::OK),
           "missing policy creates one validated default image");
  CHECK_TRUE(store.ready(), "created policy becomes ready");
  CHECK_TRUE(validMeasurementPolicy(store.config()),
             "created default policy is semantically valid");
  CHECK_TRUE(store.config().policyVersion == 1U,
             "default policy has explicit first version");
  CHECK_TRUE(std::strlen(store.config().policyName) > 0,
             "default policy has an explicit name");
  CHECK_EQ(Preferences::__writeCount(), 1UL,
           "fresh policy creation uses one atomic blob write");
  const std::vector<uint8_t> initial =
    Preferences::__getRawBytes("bp_policy", "policy_state");
  CHECK_EQ(initial.size(), MeasurementPolicyStore::encodedSize(),
           "policy image has a byte-defined fixed size");

  const MeasurementPolicyConfig next = updatedPolicy(store.config());
  Preferences::__startWriteTrace();
  CHECK_EQ(policyResult(store.update(next)),
           policyResult(MeasurementPolicyResult::OK),
           "valid administrator policy update commits");
  CHECK_EQ(Preferences::__writeCount(), 1UL,
           "policy transition is exactly one NVS value write");
  CHECK_TRUE(measurementPolicyEqual(store.config(), next),
             "verified update replaces runtime policy");

  MeasurementPolicyStore rebooted(&preferences);
  CHECK_EQ(policyResult(rebooted.loadOrCreate()),
           policyResult(MeasurementPolicyResult::OK),
           "policy reload succeeds");
  CHECK_TRUE(rebooted.ready(), "reloaded policy is ready");
  CHECK_TRUE(measurementPolicyEqual(rebooted.config(), next),
             "name version and every threshold round-trip");
}

static void testInvalidPolicyNeverWritesOrReplacesRuntime() {
  Preferences::__reset();
  Preferences preferences;
  MeasurementPolicyStore store(&preferences);
  CHECK_EQ(policyResult(store.loadOrCreate()),
           policyResult(MeasurementPolicyResult::OK), "invalid fixture loads");
  const MeasurementPolicyConfig original = store.config();

  MeasurementPolicyConfig invalid[] = {
    updatedPolicy(original), updatedPolicy(original),
    updatedPolicy(original), updatedPolicy(original),
    updatedPolicy(original), updatedPolicy(original),
  };
  invalid[0].reviewSystolic = invalid[0].urgentSystolic;
  invalid[1].reviewDiastolic = invalid[1].urgentDiastolic;
  invalid[2].reviewPulseLow = invalid[2].reviewPulseHigh;
  invalid[3].staleAfterMs = 0;
  invalid[4].policyVersion = original.policyVersion;
  invalid[5].policyName[0] = '\0';
  for (const MeasurementPolicyConfig& candidate : invalid) {
    Preferences::__startWriteTrace();
    CHECK_EQ(policyResult(store.update(candidate)),
             policyResult(MeasurementPolicyResult::INVALID_POLICY),
             "invalid or non-incrementing policy is rejected");
    CHECK_EQ(Preferences::__writeCount(), 0UL,
             "invalid policy performs no NVS write");
    CHECK_TRUE(measurementPolicyEqual(store.config(), original),
               "invalid policy leaves runtime config unchanged");
  }
}

static void testStrictPolicyFormNumberParsing() {
  uint32_t value = 99;
  CHECK_TRUE(parseMeasurementPolicyUnsigned("135", value) && value == 135U,
             "strict policy parser accepts canonical decimal");
  CHECK_TRUE(parseMeasurementPolicyUnsigned("0", value) && value == 0U,
             "strict policy parser accepts zero for later semantic validation");
  const char* invalid[] = {
    "", "+1", "-1", " 1", "1 ", "1x", "4294967296",
  };
  for (const char* text : invalid) {
    value = 99;
    CHECK_TRUE(!parseMeasurementPolicyUnsigned(text, value),
               "policy form parser rejects syntax or overflow");
  }
}

static void testCorruptPolicyFailsClosedWithoutDefaultFallback() {
  Preferences::__reset();
  Preferences preferences;
  MeasurementPolicyStore good(&preferences);
  CHECK_EQ(policyResult(good.loadOrCreate()),
           policyResult(MeasurementPolicyResult::OK), "corrupt fixture loads");
  std::vector<uint8_t> encoded =
    Preferences::__getRawBytes("bp_policy", "policy_state");
  encoded[12] ^= 0x01U;
  Preferences::__putRawBytes("bp_policy", "policy_state", encoded);
  MeasurementPolicyStore corrupt(&preferences);
  CHECK_EQ(policyResult(corrupt.loadOrCreate()),
           policyResult(MeasurementPolicyResult::CORRUPT_STATE),
           "CRC-corrupt policy fails closed");
  CHECK_TRUE(!corrupt.ready(), "corrupt policy exposes no runtime config");
  CHECK_TRUE(!validMeasurementPolicy(corrupt.config()),
             "locked policy cannot expose a usable factory fallback config");
  CHECK_TRUE(Preferences::__getRawBytes("bp_policy", "policy_state") == encoded,
             "corrupt policy is not overwritten by defaults");

  Preferences::__putRawBytes("bp_policy", "policy_state",
                             std::vector<uint8_t>(5, 0));
  MeasurementPolicyStore shortImage(&preferences);
  CHECK_EQ(policyResult(shortImage.loadOrCreate()),
           policyResult(MeasurementPolicyResult::CORRUPT_STATE),
           "wrong policy length fails closed");

  Preferences::__reset();
  Preferences raw;
  CHECK_TRUE(raw.begin("bp_policy", false), "wrong-type namespace opens");
  CHECK_TRUE(raw.putString("policy_state", "not-a-blob") != 0,
             "wrong-type policy fixture writes");
  raw.end();
  MeasurementPolicyStore wrongType(&preferences);
  CHECK_EQ(policyResult(wrongType.loadOrCreate()),
           policyResult(MeasurementPolicyResult::CORRUPT_STATE),
           "wrong policy NVS type fails closed");

  Preferences::__reset();
  Preferences::__failNextBegin();
  MeasurementPolicyStore unavailable(&preferences);
  CHECK_EQ(policyResult(unavailable.loadOrCreate()),
           policyResult(MeasurementPolicyResult::STORAGE_FAILURE),
           "NVS open failure is not mistaken for missing policy");
  CHECK_TRUE(!unavailable.ready(), "storage failure exposes no policy");
  CHECK_TRUE(!validMeasurementPolicy(unavailable.config()),
             "storage failure leaves no accidentally usable policy");
}

static void testPolicyWriteFailuresResolveToOldOrNewImage() {
  for (const auto mode : {
         Preferences::FailureMode::BEFORE_APPLY,
         Preferences::FailureMode::AFTER_APPLY,
         Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
         Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
       }) {
    Preferences::__reset();
    Preferences preferences;
    MeasurementPolicyStore store(&preferences);
    CHECK_EQ(policyResult(store.loadOrCreate()),
             policyResult(MeasurementPolicyResult::OK),
             "write-failure fixture loads");
    const MeasurementPolicyConfig original = store.config();
    const MeasurementPolicyConfig next = updatedPolicy(original);
    Preferences::__failWrite(1, mode);
    const MeasurementPolicyResult result = store.update(next);
    const bool applied =
      mode == Preferences::FailureMode::AFTER_APPLY ||
      mode == Preferences::FailureMode::HARD_CUT_AFTER_APPLY;
    CHECK_EQ(policyResult(result), policyResult(applied
               ? MeasurementPolicyResult::OK
               : MeasurementPolicyResult::STORAGE_FAILURE),
             "reported policy result matches verified durable image");
    CHECK_TRUE(store.ready(), "old-or-new durable result remains usable");
    CHECK_TRUE(measurementPolicyEqual(store.config(), applied ? next : original),
               "runtime policy matches the verified durable image");

    Preferences::__simulateReboot();
    MeasurementPolicyStore rebooted(&preferences);
    CHECK_EQ(policyResult(rebooted.loadOrCreate()),
             policyResult(MeasurementPolicyResult::OK),
             "old-or-new image reloads after simulated cut");
    CHECK_TRUE(measurementPolicyEqual(rebooted.config(), applied ? next : original),
               "reboot observes exactly the reconciled policy image");
  }
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
    CHECK_EQ(static_cast<int>(classifyMeasurement(
               value, MeasurementPolicyConfig{})),
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

static void testMonotonicUptimeSurvivesRepeatedMillisWrap() {
  MonotonicMillis64 clock;
  clock.observe(UINT32_MAX - 10U);
  CHECK_EQ(clock.nowMs(), static_cast<uint64_t>(UINT32_MAX - 10U),
           "first observation preserves current device uptime");
  clock.observe(9U);
  CHECK_EQ(clock.nowMs(), static_cast<uint64_t>(UINT32_MAX) + 10ULL,
           "one millis wrap extends into uint64 uptime");
  clock.observe(UINT32_MAX - 5U);
  clock.observe(20U);
  CHECK_TRUE(clock.nowMs() > (2ULL * static_cast<uint64_t>(UINT32_MAX)),
             "repeated observations accumulate beyond two millis epochs");

  MeasurementFreshnessInput input;
  input.hasRecord = true;
  input.valid = true;
  input.receivedThisBoot = true;
  input.transportConnected = true;
  input.lastSuccessfulReceiveMs = 100ULL;
  input.nowMs = (2ULL << 32U) + 1000ULL;
  input.staleAfterMs = 300000U;
  CHECK_EQ(static_cast<int>(measurementFreshness(input)),
           static_cast<int>(MeasurementFreshnessState::STALE),
           "multi-wrap old measurement can never become current again");
}

static void testRevisionAndReceiveAgeSurviveRingWrapSafely() {
  Preferences::__reset();
  __millisCounter() = 0;
  MonotonicMillis64 clock;
  clock.observe(0);
  BP_RecordManager manager(2, &clock);
  CHECK_TRUE(manager.loadFromStorage(), "fresh store initializes");
  CHECK_EQ(manager.getRevision(), 0ULL, "empty store starts at revision zero");

  CHECK_TRUE(manager.addRecord(reading(120, 80, 70)), "first reading persists");
  CHECK_EQ(manager.getRevision(), 1ULL, "record sequence is the first revision");
  delay(25);
  clock.observe(millis());
  CHECK_TRUE(manager.addRecord(reading(121, 81, 71)), "second reading persists");
  delay(25);
  clock.observe(millis());
  CHECK_TRUE(manager.addRecord(reading(122, 82, 72)), "ring-wrapping reading persists");
  CHECK_EQ(manager.getRecordCount(), 2, "ring remains bounded after wrap");
  CHECK_EQ(manager.getRevision(), 3ULL,
           "opaque uint64 record revision advances after ring wrap");
  CHECK_TRUE(manager.latestReceivedThisBoot(),
             "latest accepted record is current-boot data");
  uint64_t age = 99;
  CHECK_TRUE(manager.lastSuccessfulReceiveAgeMs(clock.nowMs(), age),
             "current boot exposes bounded receive age");
  CHECK_EQ(age, 0ULL, "age starts at zero after durable receive");
  delay(40);
  clock.observe(millis());
  CHECK_TRUE(manager.lastSuccessfulReceiveAgeMs(clock.nowMs(), age),
             "receive age remains available");
  CHECK_EQ(age, 40ULL, "receive age uses monotonic uint64 uptime");

  clock.observe(UINT32_MAX - 5U);
  clock.observe(50U);
  CHECK_TRUE(manager.lastSuccessfulReceiveAgeMs(clock.nowMs(), age),
             "receive age remains available after millis wrap");
  CHECK_TRUE(age > static_cast<uint64_t>(UINT32_MAX),
             "receive age crosses the uint32 epoch without resetting");

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
  testAtomicPolicyPersistenceAndRoundTrip();
  testInvalidPolicyNeverWritesOrReplacesRuntime();
  testStrictPolicyFormNumberParsing();
  testCorruptPolicyFailsClosedWithoutDefaultFallback();
  testPolicyWriteFailuresResolveToOldOrNewImage();
  testConfiguredReviewAndUrgentBoundaries();
  testInvalidValuesFailClosed();
  testFreshnessTransitions();
  testMonotonicUptimeSurvivesRepeatedMillisWrap();
  testRevisionAndReceiveAgeSurviveRingWrapSafely();
  testResultWordingAndRepeatGuidance();
  testOpaqueSequenceDecimalFormatting();
  return testReport();
}
