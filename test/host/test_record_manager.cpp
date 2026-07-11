// Crash-consistent v3 persistence specification for BP_RecordManager.

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "lib/BPRecordManager.h"
#include "test_support.h"

static uint32_t testCrc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U &
        static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

static void appendTestLe32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    bytes.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xffU));
  }
}

static void appendTestLe64(std::vector<uint8_t>& bytes, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    bytes.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xffU));
  }
}

static std::vector<uint8_t> encodeTestState(uint32_t generation,
                                            uint64_t sequenceFloor,
                                            uint8_t version = 3) {
  std::vector<uint8_t> bytes;
  bytes.push_back(version);
  appendTestLe32(bytes, generation);
  appendTestLe64(bytes, sequenceFloor);
  appendTestLe32(bytes, testCrc32(bytes.data(), bytes.size()));
  return bytes;
}

static void rewriteTestCrc(std::vector<uint8_t>& bytes) {
  if (bytes.size() < 4) return;
  const uint32_t crc = testCrc32(bytes.data(), bytes.size() - 4);
  for (int i = 0; i < 4; ++i) {
    bytes[bytes.size() - 4 + static_cast<size_t>(i)] =
      static_cast<uint8_t>((crc >> (8 * i)) & 0xffU);
  }
}

template <typename T, typename = void>
struct HasOpaqueSequences : std::false_type {};

template <typename T>
struct HasOpaqueSequences<T, std::void_t<
  decltype(std::declval<T>().recordSequence),
  decltype(std::declval<T>().sessionSequence)>> : std::true_type {};

template <typename T>
static void setSessionSequence(T& record, uint64_t sequence) {
  if constexpr (HasOpaqueSequences<T>::value) record.sessionSequence = sequence;
  (void)record;
  (void)sequence;
}

template <typename T>
static uint64_t recordSequenceOf(const T& record) {
  if constexpr (HasOpaqueSequences<T>::value) return record.recordSequence;
  (void)record;
  return 0;
}

template <typename T>
static uint64_t sessionSequenceOf(const T& record) {
  if constexpr (HasOpaqueSequences<T>::value) return record.sessionSequence;
  (void)record;
  return 0;
}

template <typename Manager>
static bool addAndReport(Manager& manager, BPData record) {
  if constexpr (std::is_same_v<
                  decltype(manager.addRecord(std::move(record))), bool>) {
    return manager.addRecord(std::move(record));
  } else {
    manager.addRecord(std::move(record));
    return true;
  }
}

template <typename Manager>
static bool loadAndReport(Manager& manager) {
  if constexpr (std::is_same_v<decltype(manager.loadFromStorage()), bool>) {
    return manager.loadFromStorage();
  } else {
    manager.loadFromStorage();
    return true;
  }
}

template <typename Manager>
static bool clearAndReport(Manager& manager) {
  if constexpr (std::is_same_v<decltype(manager.clearRecords()), bool>) {
    return manager.clearRecords();
  } else {
    manager.clearRecords();
    return true;
  }
}

static BPData makeRecord(const char* ts, int sys, int dia, int pulse,
                         bool valid = true) {
  BPData record;
  record.timestamp = ts;
  record.timestampSource = valid
    ? BPTimestampSource::DEVICE
    : BPTimestampSource::LEGACY_UNSYNCED;
  record.systolic = sys;
  record.diastolic = dia;
  record.pulse = pulse;
  record.valid = valid;
  return record;
}

static void seedV2() {
  Preferences::__reset();
  Preferences p;
  p.begin("bp_records", false);
  p.putString("schema", "v2");
  p.putInt("count", 2);
  p.putInt("index", 2);
  p.putString("slot_0", "2026-07-11 09:00:00|110|70|60|1");
  p.putString("slot_1", "2026-07-11 09:05:00|120|80|65|1");
  p.end();
  Preferences::__startWriteTrace();
}

static void seedLegacy() {
  Preferences::__reset();
  Preferences p;
  p.begin("bp_records", false);
  p.putInt("count", 2);
  p.putString("rec_0", "2026-07-11 09:05:00|120|80|65|1");
  p.putString("rec_1", "2026-07-11 09:00:00|110|70|60");
  p.end();
  Preferences::__startWriteTrace();
}

static void initializeEmpty(BP_RecordManager& manager) {
  CHECK_TRUE(loadAndReport(manager), "fresh store initializes v3 state");
}

static void testApiAndStructuredRoundTrip() {
  CHECK_TRUE(HasOpaqueSequences<BPData>::value,
             "BPData exposes opaque record/session sequences");
  CHECK_TRUE((std::is_same_v<
               decltype(std::declval<BP_RecordManager&>().addRecord(BPData{})),
               bool>),
             "addRecord returns bool");
  CHECK_TRUE((std::is_same_v<
               decltype(std::declval<BP_RecordManager&>().loadFromStorage()),
               bool>),
             "loadFromStorage returns bool");
  CHECK_TRUE((std::is_same_v<
               decltype(std::declval<BP_RecordManager&>().clearRecords()),
               bool>),
             "clearRecords returns bool");

  Preferences::__reset();
  BP_RecordManager manager(3);
  initializeEmpty(manager);

  BPData first = makeRecord("2026-07-11 09:05:00", 120, 80, 72);
  first.movementCount = 1;
  first.quality = BPMeasurementQuality::MOTION;
  setSessionSequence(first, 77);
  CHECK_TRUE(addAndReport(manager, std::move(first)), "structured add persists");

  BPData second = makeRecord("2026-07-11 09:06:00", 130, 85, 75);
  CHECK_TRUE(addAndReport(manager, std::move(second)), "second add persists");
  CHECK_EQ(recordSequenceOf(manager.getRecord(0)), 2ULL,
           "record sequence is assigned monotonically");
  CHECK_EQ(sessionSequenceOf(manager.getRecord(0)), 2ULL,
           "zero session becomes conservative one-record session");
  CHECK_EQ(sessionSequenceOf(manager.getRecord(1)), 77ULL,
           "already-opaque nonzero session is preserved");

  BP_RecordManager rebooted(3);
  CHECK_TRUE(loadAndReport(rebooted), "v3 reload succeeds");
  CHECK_EQ(rebooted.getRecordCount(), 2, "v3 reload count");
  const BPData& loaded = rebooted.getRecord(1);
  CHECK_STR(loaded.timestamp, "2026-07-11 09:05:00", "timestamp round-trip");
  CHECK_EQ(static_cast<int>(loaded.timestampSource),
           static_cast<int>(BPTimestampSource::DEVICE),
           "timestamp source round-trip");
  CHECK_EQ(loaded.systolic, 120, "systolic round-trip");
  CHECK_EQ(loaded.diastolic, 80, "diastolic round-trip");
  CHECK_EQ(loaded.pulse, 72, "pulse round-trip");
  CHECK_EQ(loaded.movementCount, 1, "movement round-trip");
  CHECK_EQ(static_cast<int>(loaded.quality),
           static_cast<int>(BPMeasurementQuality::MOTION),
           "quality round-trip");
  CHECK_TRUE(loaded.valid, "valid round-trip");
  CHECK_EQ(recordSequenceOf(loaded), 1ULL, "record sequence round-trip");
  CHECK_EQ(sessionSequenceOf(loaded), 77ULL, "session sequence round-trip");

  CHECK_EQ(Preferences::__getRawBytes("bp_records", "v3_state").size(),
           17UL, "v3 state has byte-defined size");
  CHECK_EQ(Preferences::__getRawBytes("bp_records", "v3_0").size(),
           64UL, "v3 slot size is field-defined, not native struct padding");
  CHECK_TRUE(Preferences::__longestKeyLength() <= 15,
             "all NVS keys fit ESP32 15-character limit");
}

static void testGoldenLittleEndianWireLayout() {
  Preferences::__reset();
  BP_RecordManager manager(2);
  initializeEmpty(manager);
  const std::vector<uint8_t> expectedState = {
    // version=3, generation=1 (LE), sequence floor=1 (LE),
    // CRC32[0..12]=0xe5b166bd (LE).
    0x03, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xbd, 0x66, 0xb1, 0xe5,
  };
  CHECK_TRUE(Preferences::__getRawBytes("bp_records", "v3_state") ==
               expectedState,
             "v3_state exact LE golden vector and CRC coverage");

  BPData record = makeRecord("2026-07-11 09:05:00", 120, 80, 72);
  record.movementCount = 1;
  record.quality = BPMeasurementQuality::MOTION;
  CHECK_TRUE(addAndReport(manager, std::move(record)), "golden slot add");
  const std::vector<uint8_t> expectedSlot = {
    // v3, generation=1, record=1, session=1, timestamp len/data/source,
    // four signed LE32 values, quality, valid, CRC32 of all prior bytes.
    0x03, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x13,
    0x32, 0x30, 0x32, 0x36, 0x2d, 0x30, 0x37, 0x2d, 0x31, 0x31,
    0x20, 0x30, 0x39, 0x3a, 0x30, 0x35, 0x3a, 0x30, 0x30,
    0x01,
    0x78, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00,
    0x48, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x01, 0x01,
    0x93, 0xe9, 0x43, 0x5b,
  };
  CHECK_TRUE(Preferences::__getRawBytes("bp_records", "v3_0") == expectedSlot,
             "v3 slot exact LE golden vector and CRC coverage");

  Preferences::__reset();
  const std::vector<uint8_t> explicitState = {
    0x03, 0x04, 0x03, 0x02, 0x01,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x00, 0x81, 0x3a, 0xfb,
  };
  Preferences::__putRawBytes("bp_records", "v3_state", explicitState);
  BP_RecordManager explicitOffsets(2);
  CHECK_TRUE(loadAndReport(explicitOffsets),
             "production decodes explicit LE state offsets");
  CHECK_TRUE(addAndReport(explicitOffsets,
                         makeRecord("2026-07-11 09:05:00", 120, 80, 72)),
             "decoded explicit floor is usable");
  CHECK_EQ(recordSequenceOf(explicitOffsets.getLatestRecord()),
           0x0102030405060708ULL,
           "state sequence-floor offset is fixed and 64-bit LE");
}

static void testFreshStateInitializationCuts() {
  for (const auto mode : {Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
                          Preferences::FailureMode::HARD_CUT_AFTER_APPLY}) {
    Preferences::__reset();
    Preferences::__failWrite(1, mode);
    BP_RecordManager interrupted(3);
    CHECK_TRUE(!loadAndReport(interrupted),
               "fresh v3 initialization cut reports failure");
    CHECK_EQ(interrupted.getRecordCount(), 0,
             "fresh initialization cut cannot fabricate records");
    const bool stateApplied =
      mode == Preferences::FailureMode::HARD_CUT_AFTER_APPLY;
    CHECK_EQ(Preferences::__hasKey("bp_records", "v3_state"), stateApplied,
             "fresh state atomic value is absent or fully durable");

    Preferences::__simulateReboot();
    BP_RecordManager rebooted(3);
    CHECK_TRUE(loadAndReport(rebooted),
               "fresh initialization resumes after power cut");
    CHECK_EQ(rebooted.getRecordCount(), 0,
             "resumed fresh initialization remains empty");
  }
}

static void testPreferencesLifecycleAndBeginFailures() {
  Preferences::__reset();
  Preferences raw;
  CHECK_EQ(raw.putBytes("x", "a", 1), 0UL,
           "fake rejects write before begin like ESP32 core");
  CHECK_TRUE(!raw.isKey("x"), "fake rejects read before begin");
  CHECK_TRUE(raw.begin("bp_records", false), "first begin succeeds");
  CHECK_TRUE(!raw.begin("bp_records", false), "reentrant begin is rejected");
  raw.end();

  Preferences::__reset();
  Preferences::__failNextBegin();
  BP_RecordManager fresh(3);
  CHECK_TRUE(!loadAndReport(fresh), "fresh load fails closed when begin fails");
  CHECK_EQ(fresh.getRecordCount(), 0, "failed begin cannot fabricate history");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_state"),
             "failed begin cannot create state");

  Preferences::__reset();
  BP_RecordManager manager(3);
  initializeEmpty(manager);
  Preferences::__failNextBegin();
  CHECK_TRUE(!addAndReport(manager,
                          makeRecord("2026-07-11 09:00:00", 120, 80, 65)),
             "add fails closed when Preferences begin fails");
  CHECK_EQ(manager.getRecordCount(), 0, "add begin failure leaves RAM unchanged");
  Preferences::__failNextBegin();
  CHECK_TRUE(!clearAndReport(manager),
             "clear fails closed when Preferences begin fails");
}

static void testRingWrapAndSequenceFloor() {
  Preferences::__reset();
  BP_RecordManager manager(3);
  initializeEmpty(manager);
  for (int i = 1; i <= 5; ++i) {
    String timestamp("2026-07-11 09:0");
    timestamp += i;
    timestamp += ":00";
    CHECK_TRUE(addAndReport(manager,
                           makeRecord(timestamp.c_str(), 100 + i, 70, 60)),
               "ring add persists");
  }
  CHECK_EQ(manager.getRecordCount(), 3, "ring caps count");
  CHECK_EQ(recordSequenceOf(manager.getRecord(0)), 5ULL,
           "newest sequence survives physical-slot wrap");
  CHECK_EQ(recordSequenceOf(manager.getRecord(2)), 3ULL,
           "oldest retained sequence survives wrap");

  BP_RecordManager rebooted(3);
  CHECK_TRUE(loadAndReport(rebooted), "wrapped v3 reload succeeds");
  CHECK_EQ(rebooted.getRecord(0).systolic, 105,
           "wrapped reload newest first");
  CHECK_EQ(recordSequenceOf(rebooted.getRecord(2)), 3ULL,
           "wrapped reload compacts and sorts");

  CHECK_TRUE(clearAndReport(rebooted), "clear commits generation tombstone");
  CHECK_TRUE(addAndReport(rebooted,
                         makeRecord("2026-07-11 10:00:00", 125, 82, 68)),
             "post-clear add persists");
  CHECK_EQ(recordSequenceOf(rebooted.getLatestRecord()), 6ULL,
           "clear retains sequence floor and never reuses sequence");
}

static void testSmallCapacityBoundsAndInvalidRoundTrip() {
  Preferences::__reset();
  BP_RecordManager manager(1);
  initializeEmpty(manager);
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "maxRecords=1 first add");
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
             "maxRecords=1 overwrite");
  CHECK_EQ(manager.getRecordCount(), 1, "maxRecords=1 remains bounded");
  CHECK_EQ(manager.getLatestRecord().systolic, 120,
           "maxRecords=1 keeps newest");
  CHECK_TRUE(!manager.getRecord(1).valid && !manager.getRecord(-1).valid,
             "out-of-range access returns invalid default");

  Preferences::__reset();
  BP_RecordManager invalidManager(2);
  initializeEmpty(invalidManager);
  BPData invalid = makeRecord("時間未同步", -1, -1, -1, false);
  CHECK_TRUE(addAndReport(invalidManager, std::move(invalid)),
             "structured invalid legacy record persists");
  BP_RecordManager invalidReboot(2);
  CHECK_TRUE(loadAndReport(invalidReboot), "invalid legacy record reloads");
  CHECK_EQ(invalidReboot.getRecordCount(), 1, "invalid record is retained");
  CHECK_TRUE(!invalidReboot.getLatestRecord().valid,
             "invalid flag survives v3 round-trip");
  CHECK_EQ(invalidReboot.getLatestRecord().systolic, -1,
           "invalid sentinel survives v3 round-trip");

  Preferences::__reset();
  BP_RecordManager clamped(0);
  initializeEmpty(clamped);
  CHECK_EQ(clamped.getMaxRecords(), 1,
           "nonpositive capacity clamps to one bounded slot");
  CHECK_TRUE(addAndReport(clamped,
                         makeRecord("2026-07-11 09:00:00", 120, 80, 65)),
             "clamped capacity remains usable");
}

static void prepareOneRecord(BP_RecordManager& manager) {
  initializeEmpty(manager);
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "baseline record persists");
}

static void testAddFaultReconciliation() {
  for (const auto mode : {Preferences::FailureMode::BEFORE_APPLY,
                          Preferences::FailureMode::AFTER_APPLY}) {
    Preferences::__reset();
    BP_RecordManager manager(3);
    prepareOneRecord(manager);
    Preferences::__failWrite(1, mode);
    const bool result = addAndReport(
      manager, makeRecord("2026-07-11 09:05:00", 120, 80, 65));
    CHECK_TRUE(!result, "failed or ambiguous slot write reports false");
    const int expected = mode == Preferences::FailureMode::BEFORE_APPLY ? 1 : 2;
    CHECK_EQ(manager.getRecordCount(), expected,
             "runtime reconciles to pre- or post-write durable set");

    Preferences::__clearFailure();
    BP_RecordManager rebooted(3);
    CHECK_TRUE(loadAndReport(rebooted), "reboot after add fault loads valid set");
    CHECK_EQ(rebooted.getRecordCount(), expected,
             "reboot agrees with reconciled add set");
    CHECK_EQ(rebooted.getLatestRecord().systolic,
             expected == 1 ? 110 : 120,
             "newest surviving complete record is returned first");
  }

  Preferences::__reset();
  BP_RecordManager full(3);
  initializeEmpty(full);
  CHECK_TRUE(addAndReport(full,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "full ambiguous fixture add 1");
  CHECK_TRUE(addAndReport(full,
                         makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
             "full ambiguous fixture add 2");
  CHECK_TRUE(addAndReport(full,
                         makeRecord("2026-07-11 09:10:00", 130, 85, 70)),
             "full ambiguous fixture add 3");
  Preferences::__failWrite(1, Preferences::FailureMode::AFTER_APPLY);
  CHECK_TRUE(!addAndReport(full,
                          makeRecord("2026-07-11 09:15:00", 140, 90, 75)),
             "full-ring applied/report-failed add reports false");
  CHECK_EQ(full.getRecordCount(), 3,
           "full-ring ambiguous add reconciles bounded post-write count");
  CHECK_EQ(full.getRecord(0).systolic, 140,
           "full-ring ambiguous add runtime reloads durable newest");
  CHECK_EQ(full.getRecord(2).systolic, 120,
           "full-ring ambiguous add runtime drops exactly overwritten oldest");
}

static size_t successfulAddWriteCount(bool full) {
  Preferences::__reset();
  BP_RecordManager manager(3);
  initializeEmpty(manager);
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "write-count fixture add 1");
  if (full) {
    CHECK_TRUE(addAndReport(manager,
                           makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
               "write-count fixture add 2");
    CHECK_TRUE(addAndReport(manager,
                           makeRecord("2026-07-11 09:10:00", 130, 85, 70)),
               "write-count fixture add 3");
  }
  Preferences::__startWriteTrace();
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:15:00", 140, 90, 75)),
             "write-count add succeeds");
  return Preferences::__writeCount();
}

static void testHardCutAppendAndFullRingOverwrite() {
  for (const bool full : {false, true}) {
    const size_t writes = successfulAddWriteCount(full);
    CHECK_EQ(writes, 1UL, "add is one atomic binary slot write");
    for (size_t ordinal = 1; ordinal <= writes; ++ordinal) {
      for (const auto mode : {Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
                              Preferences::FailureMode::HARD_CUT_AFTER_APPLY}) {
        Preferences::__reset();
        BP_RecordManager manager(3);
        initializeEmpty(manager);
        CHECK_TRUE(addAndReport(manager,
                               makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
                   "hard-cut fixture add 1");
        if (full) {
          CHECK_TRUE(addAndReport(manager,
                                 makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
                     "hard-cut fixture add 2");
          CHECK_TRUE(addAndReport(manager,
                                 makeRecord("2026-07-11 09:10:00", 130, 85, 70)),
                     "hard-cut fixture add 3");
        }

        Preferences::__failWrite(ordinal, mode);
        CHECK_TRUE(!addAndReport(
                     manager,
                     makeRecord("2026-07-11 09:15:00", 140, 90, 75)),
                   "power-cut add reports false");
        Preferences::__simulateReboot();
        BP_RecordManager rebooted(3);
        CHECK_TRUE(loadAndReport(rebooted), "power-cut add reboots cleanly");
        const bool applied =
          mode == Preferences::FailureMode::HARD_CUT_AFTER_APPLY;
        CHECK_EQ(rebooted.getRecordCount(), full ? 3 : (applied ? 2 : 1),
                 "append/overwrite cut exposes one complete set");
        CHECK_EQ(rebooted.getLatestRecord().systolic,
                 applied ? 140 : (full ? 130 : 110),
                 "append/overwrite cut newest belongs to old or new set");
        if (full) {
          CHECK_EQ(recordSequenceOf(rebooted.getRecord(2)),
                   applied ? 2ULL : 1ULL,
                   "full-ring overwrite has no hybrid survivor set");
        }
      }
    }
  }
}

static void testLargeAndExhaustedSequenceFloor() {
  Preferences::__reset();
  const uint64_t large = static_cast<uint64_t>(
    std::numeric_limits<uint32_t>::max()) + 42ULL;
  Preferences::__putRawBytes("bp_records", "v3_state",
                             encodeTestState(9, large));
  BP_RecordManager manager(3);
  CHECK_TRUE(loadAndReport(manager), ">UINT32 sequence floor loads");
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 11:00:00", 125, 82, 68)),
             ">UINT32 sequence persists");
  CHECK_EQ(recordSequenceOf(manager.getLatestRecord()), large,
           "record sequence is not truncated to 32 bits");
  CHECK_EQ(sessionSequenceOf(manager.getLatestRecord()), large,
           "default opaque session is not truncated to 32 bits");
  BP_RecordManager rebooted(3);
  CHECK_TRUE(loadAndReport(rebooted), ">UINT32 slot reboots");
  CHECK_EQ(recordSequenceOf(rebooted.getLatestRecord()), large,
           ">UINT32 record sequence round-trips");

  Preferences::__reset();
  Preferences::__putRawBytes(
    "bp_records", "v3_state",
    encodeTestState(1, std::numeric_limits<uint64_t>::max()));
  BP_RecordManager exhausted(3);
  CHECK_TRUE(loadAndReport(exhausted), "UINT64_MAX sequence floor is readable");
  CHECK_TRUE(!addAndReport(
               exhausted,
               makeRecord("2026-07-11 11:05:00", 126, 83, 69)),
             "sequence exhaustion fails closed instead of reusing zero/one");
  CHECK_EQ(exhausted.getRecordCount(), 0,
           "sequence exhaustion does not mutate runtime history");
}

static void testGenerationAndFloorBoundariesFailClosed() {
  for (const uint64_t floor : {0ULL, 1ULL}) {
    Preferences::__reset();
    Preferences::__putRawBytes("bp_records", "v3_state",
                               encodeTestState(0, floor));
    BP_RecordManager zeroGeneration(2);
    CHECK_TRUE(!loadAndReport(zeroGeneration),
               "generation zero is never a committed active generation");
    CHECK_EQ(zeroGeneration.getRecordCount(), 0,
             "generation zero state fails closed");
  }

  Preferences::__reset();
  Preferences::__putRawBytes("bp_records", "v3_state",
                             encodeTestState(1, 0));
  BP_RecordManager zeroFloor(2);
  CHECK_TRUE(!loadAndReport(zeroFloor),
             "zero next-sequence floor is rejected, not normalized silently");
  CHECK_EQ(zeroFloor.getRecordCount(), 0,
           "zero floor cannot permit sequence reuse");

  Preferences::__reset();
  Preferences::__putRawBytes(
    "bp_records", "v3_state",
    encodeTestState(std::numeric_limits<uint32_t>::max(), 1));
  BP_RecordManager maxGeneration(2);
  CHECK_TRUE(loadAndReport(maxGeneration), "maximum generation can be read");
  CHECK_TRUE(addAndReport(maxGeneration,
                         makeRecord("2026-07-11 12:00:00", 120, 80, 65)),
             "maximum generation can still persist a record");
  const std::vector<uint8_t> stateBefore =
    Preferences::__getRawBytes("bp_records", "v3_state");
  CHECK_TRUE(!clearAndReport(maxGeneration),
             "clear fails closed instead of wrapping generation to zero");
  CHECK_EQ(maxGeneration.getRecordCount(), 1,
           "generation-overflow clear retains old runtime history");
  CHECK_TRUE(Preferences::__getRawBytes("bp_records", "v3_state") == stateBefore,
             "generation-overflow clear leaves durable state unchanged");
}

static void buildThreeV3Records() {
  Preferences::__reset();
  BP_RecordManager manager(4);
  initializeEmpty(manager);
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "corruption fixture add 1");
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
             "corruption fixture add 2");
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:10:00", 130, 85, 70)),
             "corruption fixture add 3");
}

static void testSlotCorruptionAndDuplicates() {
  buildThreeV3Records();
  std::vector<uint8_t> corrupt =
    Preferences::__getRawBytes("bp_records", "v3_1");
  CHECK_TRUE(!corrupt.empty(), "binary slot fixture exists");
  if (!corrupt.empty()) corrupt[10] ^= 0x80;
  Preferences::__putRawBytes("bp_records", "v3_1", corrupt);
  BP_RecordManager withCorruption(4);
  CHECK_TRUE(!loadAndReport(withCorruption),
             "checksum-corrupt current-generation slot reports degraded load");
  CHECK_EQ(withCorruption.getRecordCount(), 2,
           "checksum-corrupt slot is ignored without fabrication");
  CHECK_EQ(withCorruption.getLatestRecord().systolic, 130,
           "newest surviving record remains first");
  CHECK_TRUE(!addAndReport(
               withCorruption,
               makeRecord("2026-07-11 09:15:00", 140, 90, 75)),
             "degraded slot set is read-only to prevent sequence reuse");
  CHECK_TRUE(!clearAndReport(withCorruption),
             "degraded slot set cannot clear without a trustworthy floor");
  CHECK_EQ(withCorruption.getRecordCount(), 2,
           "degraded add/clear failures preserve surviving runtime records");

  buildThreeV3Records();
  Preferences::__putRawBytes(
    "bp_records", "v3_1",
    Preferences::__getRawBytes("bp_records", "v3_0"));
  BP_RecordManager duplicate(4);
  CHECK_TRUE(!loadAndReport(duplicate), "duplicate sequence reports degraded load");
  CHECK_EQ(duplicate.getRecordCount(), 1,
           "both ambiguous duplicate-sequence slots are rejected");
  CHECK_EQ(recordSequenceOf(duplicate.getLatestRecord()), 3ULL,
           "unrelated valid slot survives duplicate rejection");

  buildThreeV3Records();
  Preferences::__putRawBytes("bp_records", "v3_1", {3, 0xff});
  BP_RecordManager malformed(4);
  CHECK_TRUE(!loadAndReport(malformed), "malformed slot reports degraded load");
  CHECK_EQ(malformed.getRecordCount(), 2, "malformed slot is ignored");

  buildThreeV3Records();
  std::vector<uint8_t> wrongVersion =
    Preferences::__getRawBytes("bp_records", "v3_1");
  if (!wrongVersion.empty()) {
    wrongVersion[0] = 2;
    rewriteTestCrc(wrongVersion);
  }
  Preferences::__putRawBytes("bp_records", "v3_1", wrongVersion);
  BP_RecordManager versioned(4);
  CHECK_TRUE(!loadAndReport(versioned),
             "valid-CRC wrong-version slot reports degraded load");
  CHECK_EQ(versioned.getRecordCount(), 2, "wrong-version slot is ignored");

  buildThreeV3Records();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("v3_1", "wrong-type");
    p.end();
  }
  BP_RecordManager wrongType(4);
  CHECK_TRUE(!loadAndReport(wrongType),
             "present wrong-type slot reports degraded load");
  CHECK_EQ(wrongType.getRecordCount(), 2, "wrong-type slot is ignored");
}

static void testPresentCorruptStateFailsClosed() {
  seedLegacy();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("v3_state", "wrong-type");
    p.end();
  }
  BP_RecordManager wrongType(3);
  CHECK_TRUE(!loadAndReport(wrongType),
             "present wrong-type v3 state fails closed");
  CHECK_EQ(wrongType.getRecordCount(), 0,
           "wrong-type state never falls back to stale legacy history");

  seedLegacy();
  Preferences::__putRawBytes("bp_records", "v3_state",
                             std::vector<uint8_t>(17, 0xa5));
  BP_RecordManager badCrc(3);
  CHECK_TRUE(!loadAndReport(badCrc), "present bad-CRC state fails closed");
  CHECK_EQ(badCrc.getRecordCount(), 0,
           "bad-CRC state never falls back to stale legacy history");

  seedLegacy();
  Preferences::__putRawBytes("bp_records", "v3_state", {3});
  BP_RecordManager shortState(3);
  CHECK_TRUE(!loadAndReport(shortState), "present short v3 state fails closed");
  CHECK_EQ(shortState.getRecordCount(), 0,
           "short state cannot fall back to stale history");

  seedLegacy();
  Preferences::__putRawBytes("bp_records", "v3_state",
                             encodeTestState(1, 1, 2));
  BP_RecordManager wrongVersion(3);
  CHECK_TRUE(!loadAndReport(wrongVersion),
             "present valid-CRC wrong-version state fails closed");
  CHECK_EQ(wrongVersion.getRecordCount(), 0,
           "wrong state version cannot fall back to stale history");
}

static void testEveryStateTruncationAndAppendFailsClosed() {
  const std::vector<uint8_t> valid = encodeTestState(1, 1);
  for (size_t length = 0; length < valid.size(); ++length) {
    seedLegacy();
    Preferences::__putRawBytes(
      "bp_records", "v3_state",
      std::vector<uint8_t>(valid.begin(), valid.begin() + length));
    BP_RecordManager manager(3);
    CHECK_TRUE(!loadAndReport(manager),
               "every present truncated state length fails closed");
    CHECK_EQ(manager.getRecordCount(), 0,
             "truncated state never falls back to legacy history");
  }
  seedLegacy();
  std::vector<uint8_t> appended = valid;
  appended.push_back(0);
  Preferences::__putRawBytes("bp_records", "v3_state", appended);
  BP_RecordManager extra(3);
  CHECK_TRUE(!loadAndReport(extra), "appended state bytes fail closed");
  CHECK_EQ(extra.getRecordCount(), 0,
           "appended state never falls back to legacy history");

  for (const size_t offset : {0UL, 1UL, 5UL, 12UL, 13UL, 16UL}) {
    seedLegacy();
    std::vector<uint8_t> flipped = valid;
    flipped[offset] ^= 0x01;
    Preferences::__putRawBytes("bp_records", "v3_state", flipped);
    BP_RecordManager manager(3);
    CHECK_TRUE(!loadAndReport(manager), "seeded state bit flip fails closed");
    CHECK_EQ(manager.getRecordCount(), 0,
             "state bit flip cannot resurrect legacy records");
  }
}

static void restoreV3Fixture(const std::vector<uint8_t>& state,
                             const std::vector<uint8_t>& slot0,
                             const std::vector<uint8_t>& slot1,
                             const std::vector<uint8_t>& slot2) {
  Preferences::__reset();
  Preferences::__putRawBytes("bp_records", "v3_state", state);
  Preferences::__putRawBytes("bp_records", "v3_0", slot0);
  Preferences::__putRawBytes("bp_records", "v3_1", slot1);
  Preferences::__putRawBytes("bp_records", "v3_2", slot2);
}

static void testEverySlotTruncationAppendAndSemanticField() {
  buildThreeV3Records();
  const std::vector<uint8_t> state =
    Preferences::__getRawBytes("bp_records", "v3_state");
  const std::vector<uint8_t> slot0 =
    Preferences::__getRawBytes("bp_records", "v3_0");
  const std::vector<uint8_t> valid =
    Preferences::__getRawBytes("bp_records", "v3_1");
  const std::vector<uint8_t> slot2 =
    Preferences::__getRawBytes("bp_records", "v3_2");
  CHECK_EQ(valid.size(), 64UL, "canonical semantic slot fixture is 64 bytes");
  if (valid.size() != 64 || state.empty() || slot0.empty() || slot2.empty()) return;

  for (size_t length = 0; length < valid.size(); ++length) {
    restoreV3Fixture(state, slot0,
                     std::vector<uint8_t>(valid.begin(), valid.begin() + length),
                     slot2);
    BP_RecordManager manager(4);
    CHECK_TRUE(!loadAndReport(manager),
               "every present truncated active slot degrades load");
    CHECK_EQ(manager.getRecordCount(), 2,
             "truncated active slot alone is dropped");
  }

  std::vector<uint8_t> appended = valid;
  appended.push_back(0);
  restoreV3Fixture(state, slot0, appended, slot2);
  BP_RecordManager extra(4);
  CHECK_TRUE(!loadAndReport(extra), "appended active slot degrades load");
  CHECK_EQ(extra.getRecordCount(), 2, "appended active slot alone is dropped");

  for (const size_t offset : {0UL, 1UL, 5UL, 13UL, 21UL, 41UL, 59UL, 63UL}) {
    std::vector<uint8_t> flipped = valid;
    flipped[offset] ^= 0x01;
    restoreV3Fixture(state, slot0, flipped, slot2);
    BP_RecordManager manager(4);
    CHECK_TRUE(!loadAndReport(manager), "seeded slot bit flip degrades load");
    CHECK_EQ(manager.getRecordCount(), 2,
             "seeded slot bit flip drops only corrupted record");
  }

  const struct Mutation {
    size_t offset;
    size_t width;
    uint32_t value;
  } mutations[] = {
    {1, 4, 0},       // generation zero
    {5, 8, 0},       // record sequence zero
    {13, 8, 0},      // session sequence zero
    {21, 1, 0},      // timestamp length zero / inconsistent total length
    {41, 1, 0xff},   // timestamp-source enum
    {42, 4, 261},    // systolic out of validated bounds
    {54, 4, 10},     // movement count out of one-digit bounds
    {58, 1, 2},      // quality enum
    {59, 1, 2},      // boolean encoding
  };
  for (const Mutation& mutation : mutations) {
    std::vector<uint8_t> semantic = valid;
    for (size_t byte = 0; byte < mutation.width; ++byte) {
      semantic[mutation.offset + byte] = static_cast<uint8_t>(
        (static_cast<uint64_t>(mutation.value) >> (8U * byte)) & 0xffU);
    }
    rewriteTestCrc(semantic);
    restoreV3Fixture(state, slot0, semantic, slot2);
    BP_RecordManager manager(4);
    CHECK_TRUE(!loadAndReport(manager),
               "valid-CRC semantic slot corruption degrades load");
    CHECK_EQ(manager.getRecordCount(), 2,
             "semantic-corrupt slot alone is rejected");
  }
}

static void testSchemaAmbiguityAndMalformedLegacyFailClosed() {
  for (const bool wrongType : {false, true}) {
    Preferences::__reset();
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 1);
    p.putString("rec_0", "2026-07-11 09:00:00|120|80|65|1");
    if (wrongType) {
      p.putBytes("schema", "v2", 2);
    } else {
      p.putString("schema", "v9");
    }
    p.end();
    BP_RecordManager manager(3);
    CHECK_TRUE(!loadAndReport(manager),
               "present unknown/wrong-type schema fails closed");
    CHECK_EQ(manager.getRecordCount(), 0,
             "ambiguous schema never falls back to pre-schema legacy");
  }

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 3);
    p.putString("rec_0", "2026-07-11 09:10:00|130|85|70|1");
    p.putString("rec_1", "bad|120junk|80|65|1");
    p.putString("rec_2", "2026-07-11 09:00:00|110|70|60");
    p.end();
  }
  BP_RecordManager legacy(4);
  CHECK_TRUE(!loadAndReport(legacy),
             "malformed legacy entry reports degraded migration");
  CHECK_EQ(legacy.getRecordCount(), 2,
           "malformed legacy numeric field cannot fabricate a record");
  CHECK_EQ(legacy.getRecord(0).systolic, 130,
           "legacy five-field newest order preserved");
  CHECK_TRUE(legacy.getRecord(1).valid,
             "legacy four-field validity is inferred safely");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_state") &&
             Preferences::__hasKey("bp_records", "rec_1"),
             "degraded legacy source remains authoritative and is not cleaned");
  BP_RecordManager legacyReboot(4);
  CHECK_TRUE(!loadAndReport(legacyReboot),
             "degraded legacy migration stays degraded after reboot");
  CHECK_EQ(legacyReboot.getRecordCount(), 2,
           "degraded legacy reboot still exposes validated survivors only");
  CHECK_TRUE(!addAndReport(
               legacyReboot,
               makeRecord("2026-07-11 09:15:00", 140, 90, 75)) &&
             !clearAndReport(legacyReboot),
             "degraded legacy source blocks add and clear");

  for (const bool v2 : {false, true}) {
    Preferences::__reset();
    Preferences p;
    p.begin("bp_records", false);
    if (v2) p.putString("schema", "v2");
    p.putBytes("count", "1", 1);
    p.putInt("index", 0);
    p.putString(v2 ? "slot_0" : "rec_0",
                "2026-07-11 09:00:00|120|80|65|1");
    p.end();
    BP_RecordManager wrongCount(3);
    CHECK_TRUE(!loadAndReport(wrongCount),
               "present wrong-type legacy/v2 count fails closed");
    CHECK_EQ(wrongCount.getRecordCount(), 0,
             "wrong-type count cannot fabricate empty/partial metadata");
  }

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 1);
    p.putBytes("index", "0", 1);
    p.putString("slot_0", "2026-07-11 09:00:00|120|80|65|1");
    p.end();
  }
  BP_RecordManager wrongIndex(3);
  CHECK_TRUE(!loadAndReport(wrongIndex),
             "present wrong-type v2 index fails closed");
  CHECK_EQ(wrongIndex.getRecordCount(), 0,
           "wrong-type index cannot select an unsafe ring order");

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 2);
    p.putInt("index", 2);
    p.putString("slot_0", "2026-07-11 09:00:00|120|80|65|2");
    p.putString("slot_1", "2026-07-11 09:05:00|130|85|70|1");
    p.end();
  }
  BP_RecordManager badValid(3);
  CHECK_TRUE(!loadAndReport(badValid),
             "v2 malformed valid flag reports degraded migration");
  CHECK_EQ(badValid.getRecordCount(), 1,
           "legacy valid flag other than 0/1 is rejected");
  CHECK_EQ(badValid.getLatestRecord().systolic, 130,
           "valid v2 survivor remains available");
}

static void testWrappedAndMissingV2SlotsCompactSafely() {
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 3);
    p.putInt("index", 1);
    p.putString("slot_0", "2026-07-11 09:10:00|130|85|70|1");
    p.putString("slot_1", "2026-07-11 09:00:00|110|70|60|1");
    p.putString("slot_2", "2026-07-11 09:05:00|120|80|65|1");
    p.end();
  }
  BP_RecordManager wrapped(3);
  CHECK_TRUE(loadAndReport(wrapped), "wrapped v2 ring migrates");
  CHECK_EQ(wrapped.getRecordCount(), 3, "wrapped v2 retains count");
  CHECK_EQ(wrapped.getRecord(0).systolic, 130,
           "wrapped v2 newest follows index metadata");
  CHECK_EQ(wrapped.getRecord(1).systolic, 120,
           "wrapped v2 middle order preserved");
  CHECK_EQ(wrapped.getRecord(2).systolic, 110,
           "wrapped v2 oldest order preserved");

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 3);
    p.putInt("index", 0);
    p.putString("slot_0", "2026-07-11 09:00:00|110|70|60|1");
    // slot_1 is physically missing.
    p.putString("slot_2", "2026-07-11 09:10:00|130|85|70|1");
    p.end();
  }
  BP_RecordManager missing(3);
  CHECK_TRUE(!loadAndReport(missing),
             "v2 missing slot reports degraded migration");
  CHECK_EQ(missing.getRecordCount(), 2,
           "missing v2 slot compacts instead of fabricating invalid record");
  CHECK_TRUE(missing.getRecord(0).valid && missing.getRecord(1).valid,
             "all compacted v2 records came from present valid slots");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_state") &&
             Preferences::__hasKey("bp_records", "schema"),
             "degraded v2 source stays authoritative and is not cleaned");
  BP_RecordManager missingReboot(3);
  CHECK_TRUE(!loadAndReport(missingReboot),
             "missing v2 slot remains degraded after reboot");
  CHECK_EQ(missingReboot.getRecordCount(), 2,
           "missing v2 reboot exposes the same validated survivors");
}

static void testUnderreportedSourceKeysNeverActivateLossyMigration() {
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 0);
    p.putString("rec_0", "2026-07-11 09:00:00|120|80|65|1");
    p.end();
  }
  BP_RecordManager legacy(3);
  CHECK_TRUE(!loadAndReport(legacy),
             "legacy key beyond count degrades source metadata");
  CHECK_EQ(legacy.getRecordCount(), 0,
           "underreported legacy key is not guessed into history");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_state") &&
             Preferences::__hasKey("bp_records", "rec_0"),
             "underreported legacy source remains intact and authoritative");

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 1);
    p.putInt("index", 1);
    p.putString("slot_0", "2026-07-11 09:00:00|120|80|65|1");
    p.putString("slot_1", "2026-07-11 09:05:00|130|85|70|1");
    p.end();
  }
  BP_RecordManager v2(3);
  CHECK_TRUE(!loadAndReport(v2), "v2 slot beyond count degrades metadata");
  CHECK_EQ(v2.getRecordCount(), 1,
           "underreported v2 history exposes only metadata-selected record");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_state") &&
             Preferences::__hasKey("bp_records", "slot_1"),
             "underreported v2 source is retained without lossy activation");
}

static void testStrictLegacyGrammarAndTimestampProvenance() {
  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 6);
    p.putString("rec_0", "2026-07-11 09:10:00|130|85|70|1");
    p.putString("rec_1", "x|120junk|80|65|1");
    p.putString("rec_2", "x|120|80|65|2");
    p.putString("rec_3", "x|120|80");
    p.putString("rec_4", "x|120|80|65|1|extra");
    p.putString("rec_5", "x|999999999999999999999|80|65|1");
    p.end();
  }
  BP_RecordManager strict(8);
  CHECK_TRUE(!loadAndReport(strict),
             "legacy missing/extra/overflow/prefix fields degrade migration");
  CHECK_EQ(strict.getRecordCount(), 1,
           "strict legacy grammar retains only the complete valid record");
  CHECK_EQ(strict.getLatestRecord().systolic, 130,
           "strict legacy survivor is not fabricated from numeric prefixes");

  Preferences::__reset();
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putInt("count", 2);
    p.putString("rec_0", "2026-07-11 09:05:00|120|80|65|1");
    p.putString("rec_1", "時間未同步|-1|-1|-1|0");
    p.end();
  }
  BP_RecordManager legacy(3);
  CHECK_TRUE(loadAndReport(legacy), "legacy timestamp provenance migrates");
  CHECK_EQ(static_cast<int>(legacy.getRecord(0).timestampSource),
           static_cast<int>(BPTimestampSource::LEGACY_SYSTEM),
           "dated legacy timestamp is explicitly legacy-system");
  CHECK_EQ(static_cast<int>(legacy.getRecord(1).timestampSource),
           static_cast<int>(BPTimestampSource::LEGACY_UNSYNCED),
           "unsynced legacy sentinel remains explicitly legacy-unsynced");
  CHECK_TRUE(static_cast<int>(legacy.getRecord(0).timestampSource) !=
               static_cast<int>(BPTimestampSource::DEVICE),
             "migration never upgrades legacy provenance to device");

  seedV2();
  BP_RecordManager v2(4);
  CHECK_TRUE(loadAndReport(v2), "v2 timestamp provenance migrates");
  CHECK_EQ(static_cast<int>(v2.getLatestRecord().timestampSource),
           static_cast<int>(BPTimestampSource::LEGACY_SYSTEM),
           "dated v2 timestamp is explicitly legacy-system");
}

static size_t successfulMigrationWriteCount(bool v2) {
  if (v2) seedV2(); else seedLegacy();
  BP_RecordManager manager(4);
  Preferences::__startWriteTrace();
  CHECK_TRUE(loadAndReport(manager), "migration baseline succeeds");
  return Preferences::__writeCount();
}

static void assertMigratedHistory(const char* label) {
  Preferences::__simulateReboot();
  BP_RecordManager rebooted(4);
  CHECK_TRUE(loadAndReport(rebooted), label);
  CHECK_EQ(rebooted.getRecordCount(), 2, "migration retains both records");
  CHECK_EQ(rebooted.getRecord(0).systolic, 120,
           "migration retains newest-first order");
  CHECK_EQ(rebooted.getRecord(1).systolic, 110,
           "migration retains oldest record");
  CHECK_EQ(recordSequenceOf(rebooted.getRecord(0)), 2ULL,
           "migration assigns deterministic chronological sequences");
  CHECK_TRUE(Preferences::__hasKey("bp_records", "v3_state"),
             "migration eventually activates v3 state");
}

static void testMigrationEveryCutIsIdempotent() {
  for (const bool v2 : {true, false}) {
    const size_t writes = successfulMigrationWriteCount(v2);
    CHECK_TRUE(writes >= 3, "migration stages slots and activates state last");
    for (size_t ordinal = 1; ordinal <= writes; ++ordinal) {
      for (const auto mode : {
             Preferences::FailureMode::BEFORE_APPLY,
             Preferences::FailureMode::AFTER_APPLY,
             Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
             Preferences::FailureMode::HARD_CUT_AFTER_APPLY}) {
        if (v2) seedV2(); else seedLegacy();
        Preferences::__failWrite(ordinal, mode);
        BP_RecordManager interrupted(4);
        (void)loadAndReport(interrupted);
        CHECK_EQ(interrupted.getRecordCount(), 2,
                 "interrupted migration exposes authoritative valid history");
        assertMigratedHistory("migration resumes idempotently after reboot");
      }
    }
  }

  seedV2();
  BP_RecordManager migrated(4);
  CHECK_TRUE(loadAndReport(migrated), "v2 migration succeeds");
  Preferences::__eraseRaw("bp_records", "count");
  Preferences::__eraseRaw("bp_records", "index");
  Preferences::__eraseRaw("bp_records", "schema");
  BP_RecordManager noMetadata(4);
  CHECK_TRUE(loadAndReport(noMetadata), "v3 ignores missing count/index metadata");
  CHECK_EQ(noMetadata.getRecordCount(), 2,
           "self-validating slots survive missing legacy metadata");
}

static void testStaleStagedSlotsCannotJoinSmallerMigration() {
  buildThreeV3Records();
  Preferences::__eraseRaw("bp_records", "v3_state");
  {
    Preferences p;
    p.begin("bp_records", false);
    p.putString("schema", "v2");
    p.putInt("count", 1);
    p.putInt("index", 1);
    p.putString("slot_0", "2026-07-12 09:00:00|125|82|68|1");
    p.end();
  }

  BP_RecordManager migrated(4);
  CHECK_TRUE(loadAndReport(migrated), "smaller authoritative migration succeeds");
  CHECK_EQ(migrated.getRecordCount(), 1,
           "stale staged v3 slots never join smaller migration");
  CHECK_EQ(migrated.getLatestRecord().systolic, 125,
           "only authoritative v2 record activates");
  BP_RecordManager rebooted(4);
  CHECK_TRUE(loadAndReport(rebooted), "smaller migration reboots");
  CHECK_EQ(rebooted.getRecordCount(), 1,
           "stale staged slots remain filtered after reboot");
}

static void prepareTwoRecordsForClear() {
  Preferences::__reset();
  BP_RecordManager manager(4);
  initializeEmpty(manager);
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:00:00", 110, 70, 60)),
             "clear fixture add 1");
  CHECK_TRUE(addAndReport(manager,
                         makeRecord("2026-07-11 09:05:00", 120, 80, 65)),
             "clear fixture add 2");
}

static void seedObsoleteStorageKeys() {
  Preferences p;
  CHECK_TRUE(p.begin("bp_records", false), "seed obsolete keys begin");
  p.putString("schema", "v2");
  p.putInt("count", 1);
  p.putInt("index", 1);
  p.putString("slot_0", "stale|120|80|65|1");
  p.putString("rec_0", "stale|120|80|65|1");
  p.end();
}

static size_t successfulClearWriteCount() {
  prepareTwoRecordsForClear();
  BP_RecordManager manager(4);
  CHECK_TRUE(loadAndReport(manager), "clear baseline reload");
  seedObsoleteStorageKeys();
  Preferences::__startWriteTrace();
  CHECK_TRUE(clearAndReport(manager), "clear baseline succeeds");
  return Preferences::__writeCount();
}

static void testClearEveryCutUsesGenerationTombstone() {
  const size_t writes = successfulClearWriteCount();
  CHECK_TRUE(writes >= 3, "clear writes tombstone before slot cleanup");
  for (size_t ordinal = 1; ordinal <= writes; ++ordinal) {
    for (const auto mode : {
           Preferences::FailureMode::BEFORE_APPLY,
           Preferences::FailureMode::AFTER_APPLY,
           Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
           Preferences::FailureMode::HARD_CUT_AFTER_APPLY}) {
      prepareTwoRecordsForClear();
      BP_RecordManager manager(4);
      CHECK_TRUE(loadAndReport(manager), "clear cut fixture reload");
      seedObsoleteStorageKeys();
      Preferences::__failWrite(ordinal, mode);
      CHECK_TRUE(!clearAndReport(manager),
                 "tombstone or cleanup failure reports false");
      const bool beforeTombstone = ordinal == 1 &&
        (mode == Preferences::FailureMode::BEFORE_APPLY ||
         mode == Preferences::FailureMode::HARD_CUT_BEFORE_APPLY);
      CHECK_EQ(manager.getRecordCount(), beforeTombstone ? 2 : 0,
               "cut exposes complete old set or committed empty generation");

      Preferences::__simulateReboot();
      BP_RecordManager rebooted(4);
      (void)loadAndReport(rebooted);
      CHECK_EQ(rebooted.getRecordCount(), beforeTombstone ? 2 : 0,
               "reboot obeys atomic generation boundary");
    }
  }

  prepareTwoRecordsForClear();
  BP_RecordManager success(4);
  CHECK_TRUE(loadAndReport(success), "successful clear fixture reload");
  seedObsoleteStorageKeys();
  CHECK_TRUE(clearAndReport(success), "successful clear reports true");
  CHECK_TRUE(Preferences::__hasKey("bp_records", "v3_state"),
             "successful clear never deletes active tombstone");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "v3_0") &&
             !Preferences::__hasKey("bp_records", "v3_1"),
             "successful clear garbage-collects old generation slots");
  CHECK_TRUE(!Preferences::__hasKey("bp_records", "schema") &&
             !Preferences::__hasKey("bp_records", "count") &&
             !Preferences::__hasKey("bp_records", "index") &&
             !Preferences::__hasKey("bp_records", "slot_0") &&
             !Preferences::__hasKey("bp_records", "rec_0"),
             "successful clear removes v2 and legacy keys but keeps state");
}

static void testSameProcessRetryReconcilesCleanupFailures() {
  for (const auto mode : {Preferences::FailureMode::BEFORE_APPLY,
                          Preferences::FailureMode::AFTER_APPLY}) {
    prepareTwoRecordsForClear();
    BP_RecordManager clearing(4);
    CHECK_TRUE(loadAndReport(clearing), "same-process clear fixture loads");
    seedObsoleteStorageKeys();
    Preferences::__failWrite(2, mode);  // tombstone succeeds; v3 GC reports failure
    CHECK_TRUE(!clearAndReport(clearing),
               "clear cleanup fault reports failure before retry");
    CHECK_EQ(clearing.getRecordCount(), 0,
             "committed clear remains logically empty after cleanup fault");
    CHECK_TRUE(clearAndReport(clearing),
               "same manager retries clear after reconciling unhealthy state");
    CHECK_EQ(clearing.getRecordCount(), 0,
             "same-process clear retry remains empty");

    prepareTwoRecordsForClear();
    seedObsoleteStorageKeys();
    Preferences::__failWrite(1, mode);  // active v3 load's legacy cleanup
    BP_RecordManager adding(4);
    CHECK_TRUE(!loadAndReport(adding),
               "v3 load cleanup fault reports degraded state");
    CHECK_TRUE(addAndReport(
                 adding,
                 makeRecord("2026-07-11 09:10:00", 130, 85, 70)),
               "same manager reloads unhealthy state before add retry");
    CHECK_EQ(adding.getRecordCount(), 3,
             "same-process add retry preserves history and appends once");

    prepareTwoRecordsForClear();
    seedObsoleteStorageKeys();
    Preferences::__failWrite(1, mode);
    BP_RecordManager clearingAfterLoad(4);
    CHECK_TRUE(!loadAndReport(clearingAfterLoad),
               "clear retry fixture begins unhealthy");
    CHECK_TRUE(clearAndReport(clearingAfterLoad),
               "same manager reloads unhealthy state before clear retry");
    CHECK_EQ(clearingAfterLoad.getRecordCount(), 0,
             "same-process clear retry commits empty history");
  }
}

int main() {
  testApiAndStructuredRoundTrip();
  testGoldenLittleEndianWireLayout();
  testFreshStateInitializationCuts();
  testPreferencesLifecycleAndBeginFailures();
  testRingWrapAndSequenceFloor();
  testSmallCapacityBoundsAndInvalidRoundTrip();
  testAddFaultReconciliation();
  testHardCutAppendAndFullRingOverwrite();
  testLargeAndExhaustedSequenceFloor();
  testGenerationAndFloorBoundariesFailClosed();
  testSlotCorruptionAndDuplicates();
  testPresentCorruptStateFailsClosed();
  testEveryStateTruncationAndAppendFailsClosed();
  testEverySlotTruncationAppendAndSemanticField();
  testSchemaAmbiguityAndMalformedLegacyFailClosed();
  testWrappedAndMissingV2SlotsCompactSafely();
  testUnderreportedSourceKeysNeverActivateLossyMigration();
  testStrictLegacyGrammarAndTimestampProvenance();
  testMigrationEveryCutIsIdempotent();
  testStaleStagedSlotsCannotJoinSmallerMigration();
  testClearEveryCutUsesGenerationTombstone();
  testSameProcessRetryReconcilesCleanupFailures();
  return testReport();
}
