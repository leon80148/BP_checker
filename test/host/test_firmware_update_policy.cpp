#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "../../lib/FirmwareUpdatePolicy.h"
#include "test_support.h"

using namespace bp_update;

static int code(Result result) { return static_cast<int>(result); }

static const char* kSha =
  "000102030405060708090a0b0c0d0e0f"
  "101112131415161718191a1b1c1d1e1f";

static std::string manifest(uint64_t sequence = 7,
                            uint64_t minimum = 5,
                            uint32_t size = 4,
                            const char* target = "esp32:esp32:esp32s3") {
  return
    "schema=bp-update-v1\n"
    "version=2.3.4-rc.1\n"
    "target=" + std::string(target) + "\n"
    "source_sha=0123456789abcdef0123456789abcdef01234567\n"
    "sequence=" + std::to_string(sequence) + "\n"
    "minimum_sequence=" + std::to_string(minimum) + "\n"
    "size=" + std::to_string(size) + "\n"
    "sha256=" + std::string(kSha) + "\n";
}

static void testCanonicalManifest() {
  Manifest parsed{};
  const std::string valid = manifest();
  CHECK_EQ(code(parseManifest(valid.data(), valid.size(), parsed)),
           code(Result::OK), "canonical signed manifest parses");
  CHECK_STR(parsed.version, "2.3.4-rc.1", "semantic version retained");
  CHECK_STR(parsed.target, "esp32:esp32:esp32s3", "board target retained");
  CHECK_EQ(parsed.sequence, 7ULL, "release sequence retained");
  CHECK_EQ(parsed.minimumSequence, 5ULL, "minimum sequence retained");
  CHECK_EQ(parsed.artifactSize, 4U, "artifact size retained");
  CHECK_STR(parsed.artifactSha256, kSha, "artifact digest retained");

  std::vector<std::string> malformed;
  malformed.push_back(valid.substr(0, valid.size() - 1));
  malformed.push_back("unknown=x\n" + valid);
  malformed.push_back(valid + "sha256=" + std::string(kSha) + "\n");
  malformed.push_back(valid.substr(valid.find("version=")));
  malformed.push_back(valid);
  malformed.back().replace(malformed.back().find("sequence=7"), 10,
                           "sequence=07");
  malformed.push_back(valid);
  malformed.back().replace(malformed.back().find("size=4"), 6,
                           "size=4294967296");
  malformed.push_back(valid);
  malformed.back().replace(malformed.back().find("2.3.4-rc.1"), 10,
                           "02.3.4-rc.1");
  malformed.push_back(valid);
  malformed.back().replace(malformed.back().find(kSha), 1, "A");
  malformed.push_back(valid);
  malformed.back().insert(malformed.back().find("target="), "target=x\n");
  malformed.push_back(std::string(kManifestMaxBytes + 1, 'x'));
  malformed.push_back(valid);
  malformed.back().insert(4, 1, '\0');
  for (const std::string& value : malformed) {
    Manifest rejected{};
    CHECK_EQ(code(parseManifest(value.data(), value.size(), rejected)),
             code(Result::MALFORMED_MANIFEST),
             "noncanonical malformed duplicate or unknown manifest rejects");
  }
  CHECK_TRUE(validSemanticVersion("1.0.0"), "release semver accepted");
  CHECK_TRUE(validSemanticVersion("1.0.0-alpha.1"),
             "prerelease semver accepted");
  for (const char* invalid : {"", "1", "1.0", "01.0.0", "1.0.0-",
                              "1.0.0-a..b", "1.0.0-01", "1.0.0+meta"}) {
    CHECK_TRUE(!validSemanticVersion(invalid), "noncanonical semver rejects");
  }
}

struct VerifyFixture {
  bool accept = true;
  std::string expectedManifest;
};

static bool verifySignature(void* context, const uint8_t* bytes, size_t length,
                            const uint8_t* signature, size_t signatureLength) {
  VerifyFixture* fixture = static_cast<VerifyFixture*>(context);
  static const uint8_t kSignature[] = {0x30, 0x01, 0x7f};
  return fixture != nullptr && fixture->accept && bytes != nullptr &&
         fixture->expectedManifest ==
           std::string(reinterpret_cast<const char*>(bytes), length) &&
         signatureLength == sizeof(kSignature) &&
         memcmp(signature, kSignature, sizeof(kSignature)) == 0;
}

static void testAuthorizationFailsClosed() {
  const std::string wire = manifest();
  const uint8_t signature[] = {0x30, 0x01, 0x7f};
  VerifyFixture fixture{true, wire};
  SignatureVerifier verifier{&fixture, verifySignature, true};
  AuthorizedManifest authorized{};
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", 5, verifier, authorized)),
           code(Result::OK), "trusted signature authorizes compatible release");
  CHECK_TRUE(authorized.valid() && authorized.manifest().sequence == 7,
             "successful verification creates an authorized manifest type");
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32", 5, verifier, authorized)),
           code(Result::WRONG_TARGET), "wrong board target rejects");
  CHECK_TRUE(!authorized.valid(), "failed target check clears authorization");
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", 7, verifier, authorized)),
           code(Result::INCOMPATIBLE_SEQUENCE), "replay sequence rejects");
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", 8, verifier, authorized)),
           code(Result::INCOMPATIBLE_SEQUENCE), "downgrade sequence rejects");
  const std::string requiresIntermediate = manifest(9, 8);
  fixture.expectedManifest = requiresIntermediate;
  CHECK_EQ(code(authorizeManifest(
             requiresIntermediate.data(), requiresIntermediate.size(),
             signature, sizeof(signature), "esp32:esp32:esp32s3", 5,
             verifier, authorized)),
           code(Result::INCOMPATIBLE_SEQUENCE),
           "unmet minimum installed sequence rejects");
  fixture.expectedManifest = wire;
  fixture.accept = false;
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", 5, verifier, authorized)),
           code(Result::SIGNATURE_INVALID), "wrong key or signature rejects");
  SignatureVerifier missing{};
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", 5, missing, authorized)),
           code(Result::SIGNATURE_UNAVAILABLE),
           "missing production trust anchor locks updates");
  CHECK_TRUE(!authorized.valid(), "missing trust leaves no streamable manifest");
}

struct ArtifactFixture {
  uint32_t expectedSize = 0;
  std::vector<uint8_t> bytes;
  std::array<uint8_t, 32> digest{};
  bool beginOk = true;
  bool writeOk = true;
  bool finishOk = true;
  bool aborted = false;
};

static bool artifactBegin(void* context, uint32_t size) {
  ArtifactFixture* fixture = static_cast<ArtifactFixture*>(context);
  fixture->expectedSize = size;
  fixture->bytes.clear();
  return fixture->beginOk;
}
static bool artifactWrite(void* context, const uint8_t* bytes, size_t length) {
  ArtifactFixture* fixture = static_cast<ArtifactFixture*>(context);
  if (!fixture->writeOk) return false;
  fixture->bytes.insert(fixture->bytes.end(), bytes, bytes + length);
  return true;
}
static bool artifactFinish(void* context, uint8_t digest[32]) {
  ArtifactFixture* fixture = static_cast<ArtifactFixture*>(context);
  if (!fixture->finishOk) return false;
  memcpy(digest, fixture->digest.data(), fixture->digest.size());
  return true;
}
static void artifactAbort(void* context) {
  static_cast<ArtifactFixture*>(context)->aborted = true;
}

static ArtifactCallbacks callbacks(ArtifactFixture& fixture) {
  return {&fixture, artifactBegin, artifactWrite, artifactFinish, artifactAbort};
}

static AuthorizedManifest authorizedManifest(uint64_t sequence = 7,
                                             uint64_t minimum = 5,
                                             uint64_t current = 5,
                                             uint32_t size = 4) {
  const std::string wire = manifest(sequence, minimum, size);
  const uint8_t signature[] = {0x30, 0x01, 0x7f};
  VerifyFixture verifierFixture{true, wire};
  SignatureVerifier verifier{&verifierFixture, verifySignature, true};
  AuthorizedManifest result{};
  CHECK_EQ(code(authorizeManifest(
             wire.data(), wire.size(), signature, sizeof(signature),
             "esp32:esp32:esp32s3", current, verifier, result)),
           code(Result::OK), "artifact fixture is signature-authorized");
  return result;
}

static void testBoundedArtifactStream() {
  ArtifactFixture fixture;
  for (size_t i = 0; i < fixture.digest.size(); ++i) {
    fixture.digest[i] = static_cast<uint8_t>(i);
  }
  ArtifactStreamPolicy stream;
  AuthorizedManifest expected = authorizedManifest();
  CHECK_EQ(code(stream.begin(expected, callbacks(fixture))), code(Result::OK),
           "verified manifest starts fixed-size artifact stream");
  const uint8_t first[] = {1, 2};
  const uint8_t second[] = {3, 4};
  CHECK_EQ(code(stream.write(first, sizeof(first))), code(Result::OK),
           "first fixed chunk writes");
  CHECK_EQ(code(stream.write(second, sizeof(second))), code(Result::OK),
           "second fixed chunk writes");
  CHECK_EQ(code(stream.finish()), code(Result::OK),
           "exact size and SHA-256 finalize");
  CHECK_TRUE(!stream.active() && fixture.bytes.size() == 4,
             "successful stream releases state without whole image buffering");

  ArtifactFixture shortFixture = fixture;
  shortFixture.aborted = false;
  CHECK_EQ(code(stream.begin(expected, callbacks(shortFixture))), code(Result::OK),
           "short stream starts");
  CHECK_EQ(code(stream.write(first, sizeof(first))), code(Result::OK),
           "short stream accepts partial chunk");
  CHECK_EQ(code(stream.finish()), code(Result::ARTIFACT_SIZE),
           "interrupted artifact never finalizes");
  CHECK_TRUE(shortFixture.aborted, "interrupted artifact aborts sink");

  ArtifactFixture longFixture = fixture;
  longFixture.aborted = false;
  const uint8_t tooLong[] = {1, 2, 3, 4, 5};
  CHECK_EQ(code(stream.begin(expected, callbacks(longFixture))), code(Result::OK),
           "overflow stream starts");
  CHECK_EQ(code(stream.write(tooLong, sizeof(tooLong))),
           code(Result::ARTIFACT_SIZE), "declared size is a hard stream cap");
  CHECK_TRUE(longFixture.aborted, "oversized stream aborts sink");

  ArtifactFixture wrongHash = fixture;
  wrongHash.digest[0] ^= 0xffU;
  wrongHash.aborted = false;
  CHECK_EQ(code(stream.begin(expected, callbacks(wrongHash))), code(Result::OK),
           "wrong-hash stream starts");
  CHECK_EQ(code(stream.write(tooLong, 4)), code(Result::OK),
           "wrong-hash bytes stream");
  CHECK_EQ(code(stream.finish()), code(Result::ARTIFACT_HASH),
           "wrong artifact SHA-256 fails closed");
  CHECK_TRUE(wrongHash.aborted, "wrong hash aborts inactive partition sink");

  ArtifactFixture failedWrite = fixture;
  failedWrite.writeOk = false;
  failedWrite.aborted = false;
  CHECK_EQ(code(stream.begin(expected, callbacks(failedWrite))), code(Result::OK),
           "write-failure stream starts");
  CHECK_EQ(code(stream.write(first, sizeof(first))), code(Result::ARTIFACT_WRITE),
           "inactive partition write failure propagates");
  CHECK_TRUE(failedWrite.aborted, "write failure aborts sink");

  ArtifactFixture failedBegin = fixture;
  failedBegin.beginOk = false;
  failedBegin.aborted = false;
  CHECK_EQ(code(stream.begin(expected, callbacks(failedBegin))),
           code(Result::ARTIFACT_WRITE), "failed OTA begin rejects stream");
  CHECK_TRUE(failedBegin.aborted,
             "partially initialized sink is cleaned after failed begin");

  AuthorizedManifest unsignedManifest;
  CHECK_EQ(code(stream.begin(unsignedManifest, callbacks(failedBegin))),
           code(Result::SIGNATURE_INVALID),
           "parsed but unsigned metadata cannot start an artifact stream");

  ArtifactFixture abandoned = fixture;
  abandoned.aborted = false;
  {
    ArtifactStreamPolicy scoped;
    CHECK_EQ(code(scoped.begin(expected, callbacks(abandoned))), code(Result::OK),
             "scoped interrupted stream starts");
    CHECK_EQ(code(scoped.write(first, sizeof(first))), code(Result::OK),
             "scoped interrupted stream receives a partial chunk");
  }
  CHECK_TRUE(abandoned.aborted,
             "destruction aborts an unfinished inactive-partition stream");
}

struct SlotStorage {
  std::array<std::array<uint8_t, kSequenceSlotBytes>, 2> slots{};
  std::array<bool, 2> present{};
  enum class Failure { NONE, READ, WRITE_BEFORE, WRITE_AFTER } failure = Failure::NONE;
  size_t writes = 0;
  size_t unrelatedApplicationResetCount = 0;
};

static bool slotRead(void* context, uint8_t slot, uint8_t* bytes,
                     size_t length, bool& present) {
  SlotStorage* storage = static_cast<SlotStorage*>(context);
  if (storage == nullptr || slot > 1 || length != kSequenceSlotBytes ||
      storage->failure == SlotStorage::Failure::READ) {
    return false;
  }
  present = storage->present[slot];
  if (present) memcpy(bytes, storage->slots[slot].data(), length);
  return true;
}

static bool slotWrite(void* context, uint8_t slot, const uint8_t* bytes,
                      size_t length) {
  SlotStorage* storage = static_cast<SlotStorage*>(context);
  storage->writes++;
  if (storage->failure == SlotStorage::Failure::WRITE_BEFORE) return false;
  memcpy(storage->slots[slot].data(), bytes, length);
  storage->present[slot] = true;
  return storage->failure != SlotStorage::Failure::WRITE_AFTER;
}

static SequenceStorage sequenceStorage(SlotStorage& storage) {
  return {&storage, slotRead, slotWrite};
}

static void applicationResetOnly(SlotStorage& storage) {
  storage.unrelatedApplicationResetCount++;
}

static void testCrashConsistentMonotonicSequence() {
  SlotStorage storage;
  MonotonicSequenceStore store(sequenceStorage(storage));
  CHECK_EQ(code(store.load()), code(Result::STORAGE_UNINITIALIZED),
           "missing monotonic state does not silently default");
  CHECK_EQ(code(store.initialize(5)), code(Result::OK),
           "manufacturing explicitly initializes monotonic state");
  CHECK_TRUE(store.ready() && store.sequence() == 5 && store.generation() == 1,
             "initial monotonic state becomes ready");
  CHECK_EQ(code(store.advance(5)), code(Result::INCOMPATIBLE_SEQUENCE),
           "equal sequence replay rejects without write");
  CHECK_EQ(code(store.advance(4)), code(Result::INCOMPATIBLE_SEQUENCE),
           "sequence downgrade rejects without write");
  CHECK_EQ(code(store.advance(7)), code(Result::OK),
           "higher verified sequence advances atomically");
  MonotonicSequenceStore reboot(sequenceStorage(storage));
  CHECK_EQ(code(reboot.load()), code(Result::OK), "newest valid slot reloads");
  CHECK_EQ(reboot.sequence(), 7ULL, "accepted sequence survives reboot");

  applicationResetOnly(storage);
  applicationResetOnly(storage);
  applicationResetOnly(storage);
  MonotonicSequenceStore afterResets(sequenceStorage(storage));
  CHECK_EQ(code(afterResets.load()), code(Result::OK),
           "credential network and decommission app resets leave update state");
  CHECK_EQ(afterResets.sequence(), 7ULL,
           "application resets cannot reduce accepted sequence");

  SlotStorage before = storage;
  before.failure = SlotStorage::Failure::WRITE_BEFORE;
  MonotonicSequenceStore beforeStore(sequenceStorage(before));
  CHECK_EQ(code(beforeStore.load()), code(Result::OK), "before-cut fixture loads");
  CHECK_EQ(code(beforeStore.advance(9)), code(Result::STORAGE_FAILURE),
           "cut before inactive-slot write reports failure");
  before.failure = SlotStorage::Failure::NONE;
  MonotonicSequenceStore beforeReboot(sequenceStorage(before));
  CHECK_EQ(code(beforeReboot.load()), code(Result::OK),
           "old slot survives cut before write");
  CHECK_EQ(beforeReboot.sequence(), 7ULL, "cut before write retains old sequence");

  SlotStorage after = storage;
  after.failure = SlotStorage::Failure::WRITE_AFTER;
  MonotonicSequenceStore afterStore(sequenceStorage(after));
  CHECK_EQ(code(afterStore.load()), code(Result::OK), "after-cut fixture loads");
  CHECK_EQ(code(afterStore.advance(9)), code(Result::OK),
           "write-report failure reconciles durable new slot");
  after.failure = SlotStorage::Failure::NONE;
  MonotonicSequenceStore afterReboot(sequenceStorage(after));
  CHECK_EQ(code(afterReboot.load()), code(Result::OK),
           "new slot survives cut after apply");
  CHECK_EQ(afterReboot.sequence(), 9ULL, "cut after apply retains new sequence");

  SlotStorage corrupt = storage;
  corrupt.slots[0][0] ^= 0xffU;
  corrupt.slots[1][0] ^= 0xffU;
  MonotonicSequenceStore corruptStore(sequenceStorage(corrupt));
  CHECK_EQ(code(corruptStore.load()), code(Result::STORAGE_CORRUPT),
           "two corrupt slots fail closed without reset to zero");

  SlotStorage newestCorrupt = storage;
  newestCorrupt.slots[1][0] ^= 0xffU;
  MonotonicSequenceStore newestCorruptStore(sequenceStorage(newestCorrupt));
  CHECK_EQ(code(newestCorruptStore.load()), code(Result::STORAGE_CORRUPT),
           "present corrupt slot cannot roll anti-replay back to older slot");

  SlotStorage rollbackImage;
  encodeSequenceSlot(SequenceSlot{8, 10}, rollbackImage.slots[0].data());
  encodeSequenceSlot(SequenceSlot{9, 9}, rollbackImage.slots[1].data());
  rollbackImage.present = {true, true};
  MonotonicSequenceStore rollbackStore(sequenceStorage(rollbackImage));
  CHECK_EQ(code(rollbackStore.load()), code(Result::STORAGE_CORRUPT),
           "newer generation cannot encode a lower accepted sequence");

  SlotStorage exhausted;
  SequenceSlot maxGeneration{UINT64_MAX, 11};
  encodeSequenceSlot(maxGeneration, exhausted.slots[0].data());
  exhausted.present[0] = true;
  MonotonicSequenceStore exhaustedStore(sequenceStorage(exhausted));
  CHECK_EQ(code(exhaustedStore.load()), code(Result::OK),
           "generation-exhaustion fixture loads");
  CHECK_EQ(code(exhaustedStore.advance(12)), code(Result::SEQUENCE_EXHAUSTED),
           "generation exhaustion cannot wrap or erase anti-replay state");
}

static bool confirmBoot(void* context) {
  return context != nullptr && *static_cast<bool*>(context);
}

static void testPendingBootHealthConfirmation() {
  SlotStorage storage;
  MonotonicSequenceStore store(sequenceStorage(storage));
  CHECK_EQ(code(store.initialize(5)), code(Result::OK), "boot fixture initializes");
  PendingBootPolicy pending(&store);
  const AuthorizedManifest release7 = authorizedManifest(7, 5, 5);
  CHECK_EQ(code(pending.beginPending(release7)), code(Result::OK),
           "verified candidate enters pending-health state");
  bool confirm = true;
  CHECK_EQ(code(pending.evaluate({true, false, true}, &confirm, confirmBoot)),
           code(Result::HEALTH_FAILED),
           "missing USB transport health requires bootloader rollback");
  CHECK_EQ(static_cast<int>(pending.state()),
           static_cast<int>(BootState::ROLLBACK_REQUIRED),
           "failed self-check never confirms boot");
  CHECK_EQ(store.sequence(), 5ULL,
           "unconfirmed image does not advance accepted sequence");

  PendingBootPolicy healthy(&store);
  CHECK_EQ(code(healthy.beginPending(release7)), code(Result::OK),
           "healthy candidate enters pending state");
  CHECK_EQ(code(healthy.evaluate({true, true, true}, &confirm, confirmBoot)),
           code(Result::OK), "all storage transport and Web checks confirm boot");
  CHECK_EQ(static_cast<int>(healthy.state()),
           static_cast<int>(BootState::CONFIRMED), "healthy boot is confirmed");
  CHECK_EQ(store.sequence(), 7ULL,
           "sequence advances before successful boot confirmation");

  PendingBootPolicy confirmFailure(&store);
  const AuthorizedManifest release9 = authorizedManifest(9, 7, 7);
  CHECK_EQ(code(confirmFailure.beginPending(release9)), code(Result::OK),
           "confirmation-failure candidate begins");
  confirm = false;
  CHECK_EQ(code(confirmFailure.evaluate({true, true, true}, &confirm, confirmBoot)),
           code(Result::BOOT_CONFIRM_FAILED),
           "platform confirmation failure requests rollback");
  CHECK_EQ(static_cast<int>(confirmFailure.state()),
           static_cast<int>(BootState::ROLLBACK_REQUIRED),
           "failed platform confirmation is never reported healthy");
  CHECK_EQ(store.sequence(), 9ULL,
           "anti-replay sequence remains monotonic after confirm boundary cut");

  PendingBootPolicy unsignedPending(&store);
  AuthorizedManifest unsignedManifest;
  CHECK_EQ(code(unsignedPending.beginPending(unsignedManifest)),
           code(Result::INCOMPATIBLE_SEQUENCE),
           "unsigned metadata cannot enter pending boot state");

  const AuthorizedManifest staleAuthorization = authorizedManifest(11, 9, 9);
  CHECK_EQ(code(store.advance(10)), code(Result::OK),
           "concurrent accepted release advances anti-replay state");
  PendingBootPolicy stalePending(&store);
  CHECK_EQ(code(stalePending.beginPending(staleAuthorization)),
           code(Result::INCOMPATIBLE_SEQUENCE),
           "authorization bound to an older sequence cannot be reused");
}

int main() {
  testCanonicalManifest();
  testAuthorizationFailsClosed();
  testBoundedArtifactStream();
  testCrashConsistentMonotonicSequence();
  testPendingBootHealthConfirmation();
  return testReport();
}
