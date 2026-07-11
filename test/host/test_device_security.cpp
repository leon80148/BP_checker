// Host-side specification for the byte-defined device-security bundle.
//
// The production header intentionally owns no Wi-Fi/WebServer behavior. These
// tests cover only entropy, Preferences persistence, claim/rotation/recovery,
// and the wipe marker consumed by a future external coordinator.

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "lib/DeviceSecurity.h"
#include "test_support.h"

namespace {

static_assert(!std::is_copy_constructible<DeviceSecurity>::value,
              "security state must not create unmanaged plaintext copies");
static_assert(!std::is_copy_assignable<DeviceSecurity>::value,
              "security state must not copy-assign plaintext credentials");

constexpr size_t kBundleSize = 112;
constexpr size_t kSecretSize = 22;
constexpr size_t kApOffset = 20;
constexpr size_t kBootstrapOffset = 42;
constexpr size_t kAdminOffset = 64;
constexpr size_t kStaffOffset = 86;
constexpr size_t kCrcOffset = 108;

constexpr const char* kApSecret = "AAECAwQFBgcICQoLDA0ODw";
constexpr const char* kBootstrapSecret = "EBESExQVFhcYGRobHB0eHw";
constexpr const char* kAdminSecret = "ICEiIyQlJicoKSorLC0uLw";
constexpr const char* kStaffSecret = "MDEyMzQ1Njc4OTo7PD0-Pw";
constexpr const char* kRotated64 = "QEFCQ0RFRkdISUpLTE1OTw";
constexpr const char* kRotated80 = "UFFSU1RVVldYWVpbXF1eXw";

struct EntropyScript {
  std::vector<std::array<uint8_t, 16>> blocks;
  size_t calls = 0;
  size_t failCall = 0;  // one-based; zero means never fail

  static bool fill(void* context, uint8_t* output, size_t length) {
    auto* self = static_cast<EntropyScript*>(context);
    self->calls++;
    if (length != 16 || output == nullptr ||
        (self->failCall != 0 && self->calls == self->failCall) ||
        self->calls > self->blocks.size()) {
      return false;
    }
    memcpy(output, self->blocks[self->calls - 1].data(), length);
    return true;
  }

  DeviceEntropySource source() {
    return DeviceEntropySource{this, &EntropyScript::fill};
  }
};

EntropyScript sequentialEntropy(uint8_t first, size_t blockCount = 8) {
  EntropyScript script;
  for (size_t block = 0; block < blockCount; ++block) {
    std::array<uint8_t, 16> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<uint8_t>(first + block * 16U + i);
    }
    script.blocks.push_back(bytes);
  }
  return script;
}

EntropyScript repeatedEntropy(uint8_t value, size_t blockCount = 4) {
  EntropyScript script;
  for (size_t i = 0; i < blockCount; ++i) {
    std::array<uint8_t, 16> bytes{};
    bytes.fill(value);
    script.blocks.push_back(bytes);
  }
  return script;
}

uint32_t testCrc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0
        ? (crc >> 1U) ^ 0xedb88320U
        : crc >> 1U;
    }
  }
  return ~crc;
}

void writeLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[offset + i] =
      static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
  }
}

void writeLe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    bytes[offset + i] =
      static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
  }
}

void refreshCrc(std::vector<uint8_t>& bytes) {
  writeLe32(bytes, kCrcOffset, testCrc32(bytes.data(), kCrcOffset));
}

std::vector<uint8_t> validBundle(
    uint8_t claimState = 1, uint8_t tokenConsumed = 0,
    uint8_t wipeKind = 0, uint8_t legacySdkErased = 1,
    uint64_t revision = 7,
    const char* ap = kApSecret,
    const char* bootstrap = kBootstrapSecret,
    const char* admin = kAdminSecret,
    const char* staff = kStaffSecret) {
  std::vector<uint8_t> bytes(kBundleSize, 0);
  bytes[0] = 'B';
  bytes[1] = 'P';
  bytes[2] = 'S';
  bytes[3] = 'C';
  bytes[4] = 1;
  bytes[5] = claimState;
  bytes[6] = tokenConsumed;
  bytes[7] = wipeKind;
  bytes[8] = legacySdkErased;
  writeLe64(bytes, 12, revision);
  memcpy(bytes.data() + kApOffset, ap, kSecretSize);
  memcpy(bytes.data() + kBootstrapOffset, bootstrap, kSecretSize);
  memcpy(bytes.data() + kAdminOffset, admin, kSecretSize);
  memcpy(bytes.data() + kStaffOffset, staff, kSecretSize);
  refreshCrc(bytes);
  return bytes;
}

void storeBundle(const std::vector<uint8_t>& bytes) {
  Preferences::__putRawBytes("bp_sec", "sec_state", bytes);
}

std::vector<uint8_t> storedBundle() {
  return Preferences::__getRawBytes("bp_sec", "sec_state");
}

int resultValue(DeviceSecurityResult result) {
  return static_cast<int>(result);
}

int availabilityValue(DeviceSecurityAvailability availability) {
  return static_cast<int>(availability);
}

int claimValue(DeviceClaimState state) {
  return static_cast<int>(state);
}

int wipeValue(DeviceWipeKind kind) {
  return static_cast<int>(kind);
}

bool isAfterApply(Preferences::FailureMode mode) {
  return mode == Preferences::FailureMode::AFTER_APPLY ||
         mode == Preferences::FailureMode::HARD_CUT_AFTER_APPLY;
}

void expectBytesEqual(const std::vector<uint8_t>& actual,
                      const std::vector<uint8_t>& expected,
                      const char* label) {
  CHECK_EQ(actual.size(), expected.size(), label);
  if (actual.size() != expected.size()) return;
  for (size_t i = 0; i < actual.size(); ++i) {
    CHECK_EQ(actual[i], expected[i], label);
  }
}

void expectLocked(const std::vector<uint8_t>& bytes, const char* label) {
  Preferences::__reset();
  storeBundle(bytes);
  EntropyScript entropy = sequentialEntropy(0);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());
  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::CORRUPT_STATE), label);
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::LOCKED), label);
  CHECK_EQ(entropy.calls, 0U, label);
  CHECK_STR(security.secret(DeviceSecretKind::AP), "", label);
}

void testFreshGenerationAndGoldenLayout() {
  Preferences::__reset();
  EntropyScript entropy = sequentialEntropy(0);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());

  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "fresh state is generated and committed");
  CHECK_EQ(entropy.calls, 4U, "fresh state requests four independent blocks");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
           "legacy driver erase blocks readiness");
  CHECK_STR(security.secret(DeviceSecretKind::AP), "",
            "pending state discloses no secret");

  const std::vector<uint8_t> expected =
    validBundle(1, 0, 0, 0, 1, kApSecret, kBootstrapSecret,
                kAdminSecret, kStaffSecret);
  expectBytesEqual(storedBundle(), expected,
                   "fresh blob is an exact byte-defined golden vector");
  CHECK_EQ(DeviceSecurity::kBundleSize, kBundleSize,
           "public bundle size matches storage contract");
  CHECK_TRUE(Preferences::__longestKeyLength() <= 15,
             "security key respects ESP NVS key limit");

  CHECK_EQ(resultValue(security.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::OK),
           "successful legacy erase finalizes initial state");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::READY),
           "legacy erase makes state ready");
  CHECK_TRUE(security.legacySdkErased(), "legacy SDK erasure is durable");
  CHECK_EQ(security.revision(), 2U, "legacy erasure advances revision");
  CHECK_STR(security.secret(DeviceSecretKind::AP), kApSecret,
            "AP secret is canonical Base64URL");
  CHECK_STR(security.secret(DeviceSecretKind::BOOTSTRAP), kBootstrapSecret,
            "bootstrap secret is canonical Base64URL");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
            "unclaimed fresh state does not disclose admin secret");
  CHECK_STR(security.secret(DeviceSecretKind::STAFF), "",
            "unclaimed fresh state does not disclose staff secret");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String(kAdminSecret)),
             "unclaimed fresh state denies admin authentication");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::STAFF,
                                     String(kStaffSecret)),
             "unclaimed fresh state denies staff authentication");

  Preferences::__simulateReboot();
  EntropyScript unused = sequentialEntropy(200);
  Preferences rebootPreferences;
  DeviceSecurity rebooted(&rebootPreferences, unused.source());
  CHECK_EQ(resultValue(rebooted.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "valid bundle reloads after reboot");
  CHECK_EQ(unused.calls, 0U, "reload never regenerates existing credentials");
  CHECK_STR(rebooted.secret(DeviceSecretKind::AP), kApSecret,
            "unclaimed AP commissioning secret survives reboot");
  CHECK_STR(rebooted.secret(DeviceSecretKind::BOOTSTRAP), kBootstrapSecret,
            "unclaimed bootstrap commissioning secret survives reboot");
  CHECK_STR(rebooted.secret(DeviceSecretKind::ADMIN), "",
            "rebooted unclaimed state still hides admin secret");
  CHECK_STR(rebooted.secret(DeviceSecretKind::STAFF), "",
            "rebooted unclaimed state still hides staff secret");
}

void testEntropyFailuresFailClosed() {
  for (size_t failedCall = 1; failedCall <= 4; ++failedCall) {
    Preferences::__reset();
    EntropyScript entropy = sequentialEntropy(0);
    entropy.failCall = failedCall;
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
             "entropy callback failure rejects initialization");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(DeviceSecurityAvailability::LOCKED),
             "entropy callback failure locks state");
    CHECK_EQ(entropy.calls, failedCall,
             "generation stops at the failed entropy call");
    CHECK_TRUE(!Preferences::__hasKey("bp_sec", "sec_state"),
               "entropy failure writes no bundle");
  }

  Preferences::__reset();
  EntropyScript zeros = repeatedEntropy(0x00);
  Preferences zeroPreferences;
  DeviceSecurity zeroSecurity(&zeroPreferences, zeros.source());
  CHECK_EQ(resultValue(zeroSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "all-zero entropy is rejected");
  CHECK_EQ(zeros.calls, 1U, "zero block fails immediately");

  Preferences::__reset();
  EntropyScript ones = repeatedEntropy(0xff);
  Preferences onePreferences;
  DeviceSecurity oneSecurity(&onePreferences, ones.source());
  CHECK_EQ(resultValue(oneSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "stuck all-ones entropy is rejected");

  Preferences::__reset();
  EntropyScript duplicate = sequentialEntropy(0);
  duplicate.blocks[1] = duplicate.blocks[0];
  Preferences duplicatePreferences;
  DeviceSecurity duplicateSecurity(&duplicatePreferences, duplicate.source());
  CHECK_EQ(resultValue(duplicateSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "duplicate independent blocks are rejected");
  CHECK_EQ(duplicate.calls, 2U, "duplicate is detected at first collision");
}

void testFreshWriteCuts() {
  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    EntropyScript entropy = sequentialEntropy(0);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "fresh write cut reports storage failure");
    CHECK_STR(security.secret(DeviceSecretKind::AP), "",
              "failed creation never discloses a credential");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::LOCKED),
             "fresh cut exposes safe current-boot state");
    CHECK_EQ(Preferences::__hasKey("bp_sec", "sec_state"),
             isAfterApply(mode),
             "fresh cut stores either no image or the complete image");

    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(80);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK),
             "reboot resolves fresh cut deterministically");
    CHECK_EQ(rebootEntropy.calls, isAfterApply(mode) ? 0U : 4U,
             "only a before-cut regenerates on reboot");
    CHECK_EQ(availabilityValue(rebooted.availability()),
             availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
             "recovered fresh image still requires SDK erase");
  }
}

void testMalformedBundlesLockWithoutFallback() {
  const std::vector<uint8_t> valid = validBundle();
  for (size_t length = 0; length < kBundleSize; ++length) {
    expectLocked(std::vector<uint8_t>(valid.begin(), valid.begin() + length),
                 "every truncated security blob locks");
  }
  std::vector<uint8_t> overlong = valid;
  overlong.push_back(0);
  expectLocked(overlong, "overlong security blob locks");

  for (size_t offset = 0; offset < kCrcOffset; ++offset) {
    std::vector<uint8_t> corrupt = valid;
    corrupt[offset] ^= 0x01U;
    expectLocked(corrupt, "every protected-byte bit flip locks");
  }

  std::vector<std::vector<uint8_t>> invalid;
  auto addInvalid = [&](size_t offset, uint8_t value) {
    std::vector<uint8_t> bytes = valid;
    bytes[offset] = value;
    refreshCrc(bytes);
    invalid.push_back(bytes);
  };
  addInvalid(0, 'X');
  addInvalid(4, 2);
  addInvalid(5, 0);
  addInvalid(5, 3);
  addInvalid(6, 2);
  addInvalid(7, 3);
  addInvalid(8, 2);
  addInvalid(9, 1);
  addInvalid(10, 1);
  addInvalid(11, 1);
  addInvalid(kApOffset, '!');
  addInvalid(kApOffset + kSecretSize - 1, 'B');

  std::vector<uint8_t> zeroRevision = valid;
  writeLe64(zeroRevision, 12, 0);
  refreshCrc(zeroRevision);
  invalid.push_back(zeroRevision);

  std::vector<uint8_t> impossibleFlags = valid;
  impossibleFlags[6] = 1;
  refreshCrc(impossibleFlags);
  invalid.push_back(impossibleFlags);

  std::vector<uint8_t> duplicate = valid;
  memcpy(duplicate.data() + kAdminOffset,
         duplicate.data() + kApOffset, kSecretSize);
  refreshCrc(duplicate);
  invalid.push_back(duplicate);

  for (const auto& bytes : invalid) {
    expectLocked(bytes, "valid-CRC semantic corruption locks");
  }

  Preferences::__reset();
  Preferences typed;
  CHECK_TRUE(typed.begin("bp_sec", false), "wrong-type fixture opens NVS");
  CHECK_EQ(typed.putString("sec_state", String("legacy-default")), 14U,
           "wrong-type fixture stores string");
  typed.end();
  EntropyScript entropy = sequentialEntropy(0);
  Preferences preferences;
  DeviceSecurity wrongType(&preferences, entropy.source());
  CHECK_EQ(resultValue(wrongType.loadOrCreate()),
           resultValue(DeviceSecurityResult::CORRUPT_STATE),
           "present wrong-type state locks");
  CHECK_EQ(entropy.calls, 0U, "wrong type never regenerates defaults");

  Preferences::__reset();
  Preferences::__failNextBegin();
  EntropyScript beginEntropy = sequentialEntropy(0);
  Preferences beginPreferences;
  DeviceSecurity beginFailure(&beginPreferences, beginEntropy.source());
  CHECK_EQ(resultValue(beginFailure.loadOrCreate()),
           resultValue(DeviceSecurityResult::STORAGE_FAILURE),
           "NVS open failure is not mistaken for a fresh device");
  CHECK_EQ(beginEntropy.calls, 0U, "NVS open failure consumes no entropy");

  Preferences::__reset();
  storeBundle(validBundle(2, 0));
  EntropyScript armedEntropy = sequentialEntropy(0);
  Preferences armedPreferences;
  DeviceSecurity armed(&armedPreferences, armedEntropy.source());
  CHECK_EQ(resultValue(armed.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "claimed plus unconsumed bootstrap is valid recovery-armed state");
}

void testClaimAndConstantWorkCredentialChecks() {
  Preferences::__reset();
  const std::vector<uint8_t> original = validBundle();
  storeBundle(original);
  EntropyScript entropy = sequentialEntropy(100);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());
  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "claim fixture loads");

  CHECK_STR(security.secret(DeviceSecretKind::AP), kApSecret,
            "unclaimed state exposes AP secret for provisioning");
  CHECK_STR(security.secret(DeviceSecretKind::BOOTSTRAP), kBootstrapSecret,
            "unclaimed state exposes bootstrap for physical commissioning");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
            "unclaimed state hides admin secret");
  CHECK_STR(security.secret(DeviceSecretKind::STAFF), "",
            "unclaimed state hides staff secret");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String(kAdminSecret)),
             "stored admin secret cannot bypass first claim");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::STAFF,
                                     String(kStaffSecret)),
             "stored staff secret cannot bypass first claim");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String("XCEiIyQlJicoKSorLC0uLw")),
             "first-byte mismatch denies");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String("ICEiIyQlJicoKSorLC0uLX")),
             "last-byte mismatch denies");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String("ICEiIyQlJicoKSorLC0uL")),
             "short secret denies");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String("ICEiIyQlJicoKSorLC0uLwX")),
             "long secret denies");

  CHECK_EQ(resultValue(security.claimBootstrap(String("wrong"), true, false)),
           resultValue(DeviceSecurityResult::DENIED),
           "wrong bootstrap token denies claim");
  CHECK_EQ(resultValue(security.claimBootstrap(
             String(kBootstrapSecret), false, false)),
           resultValue(DeviceSecurityResult::DENIED),
           "claim requires AP or physical presence");
  expectBytesEqual(storedBundle(), original, "denied claim performs no write");

  CHECK_EQ(resultValue(security.claimBootstrap(
             String(kBootstrapSecret), true, false)),
           resultValue(DeviceSecurityResult::OK),
           "correct AP-bound bootstrap claims once");
  CHECK_EQ(claimValue(security.claimState()),
           claimValue(DeviceClaimState::CLAIMED), "claim state commits");
  CHECK_TRUE(security.tokenConsumed(), "claim consumes bootstrap atomically");
  CHECK_EQ(security.revision(), 8U, "claim advances revision");
  CHECK_EQ(entropy.calls, 0U, "claim uses stored independent credentials");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), kAdminSecret,
            "claimed state exposes admin secret to trusted caller");
  CHECK_STR(security.secret(DeviceSecretKind::STAFF), kStaffSecret,
            "claimed state exposes staff secret to trusted caller");
  CHECK_TRUE(security.matchesSecret(DeviceSecretKind::ADMIN,
                                    String(kAdminSecret)),
             "exact admin secret authenticates only after claim");
  CHECK_EQ(resultValue(security.claimBootstrap(
             String(kBootstrapSecret), true, false)),
           resultValue(DeviceSecurityResult::INVALID_STATE),
           "bootstrap cannot claim twice");

  Preferences::__reset();
  storeBundle(original);
  EntropyScript physicalEntropy = sequentialEntropy(100);
  Preferences physicalPreferences;
  DeviceSecurity physical(&physicalPreferences, physicalEntropy.source());
  CHECK_EQ(resultValue(physical.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "physical fixture loads");
  CHECK_EQ(resultValue(physical.claimBootstrap(
             String(kBootstrapSecret), false, true)),
           resultValue(DeviceSecurityResult::OK),
           "physical presence is a valid alternate claim boundary");
}

void testClaimWriteCuts() {
  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle());
    EntropyScript entropy = sequentialEntropy(100);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "cut claim fixture loads");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(security.claimBootstrap(
               String(kBootstrapSecret), true, false)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "claim write cut reports failure");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::READY),
             "claim cut reconciles current-boot availability");
    if (isAfterApply(mode)) {
      CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
                "applied-report-failed claim discloses nothing");
      CHECK_EQ(resultValue(security.loadOrCreate()),
               resultValue(DeviceSecurityResult::INVALID_STATE),
               "same object cannot bypass reboot-required by reloading");
      CHECK_EQ(availabilityValue(security.availability()),
               availabilityValue(DeviceSecurityAvailability::REBOOT_REQUIRED),
               "one-shot load preserves reboot-required state");
    }

    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(150);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK),
             "claim cut reload succeeds");
    CHECK_EQ(claimValue(rebooted.claimState()),
             claimValue(isAfterApply(mode)
               ? DeviceClaimState::CLAIMED
               : DeviceClaimState::UNCLAIMED),
             "claim cut reboots to exact old or post state");
    CHECK_EQ(rebooted.tokenConsumed(), isAfterApply(mode),
             "claim and token consumption never tear");
  }
}

void testRotationsAndRevisionExhaustion() {
  const std::array<DeviceSecretKind, 4> kinds = {
    DeviceSecretKind::AP,
    DeviceSecretKind::BOOTSTRAP,
    DeviceSecretKind::ADMIN,
    DeviceSecretKind::STAFF,
  };
  for (const auto kind : kinds) {
    Preferences::__reset();
    storeBundle(validBundle(2, 1, 0, 1, 10));
    EntropyScript entropy = sequentialEntropy(64);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "rotation fixture loads");
    const std::array<std::string, 4> oldSecrets = {
      kApSecret, kBootstrapSecret, kAdminSecret, kStaffSecret,
    };
    CHECK_EQ(resultValue(security.rotateSecret(kind)),
             resultValue(DeviceSecurityResult::OK),
             "admin-authorized independent rotation succeeds");
    CHECK_EQ(entropy.calls, 1U, "rotation requests one independent block");
    CHECK_EQ(security.revision(), 11U, "rotation advances revision");
    for (size_t i = 0; i < kinds.size(); ++i) {
      if (kinds[i] == kind) {
        CHECK_STR(security.secret(kinds[i]), kRotated64,
                  "rotation changes selected secret");
      } else {
        CHECK_STR(security.secret(kinds[i]), oldSecrets[i].c_str(),
                  "rotation preserves every other secret");
      }
    }
    CHECK_EQ(security.tokenConsumed(), kind != DeviceSecretKind::BOOTSTRAP,
             "bootstrap rotation arms one physical recovery token only");
    CHECK_TRUE(!security.matchesSecret(kind, String(oldSecrets[
                 static_cast<size_t>(kind)].c_str())),
               "old rotated secret is denied immediately");
    CHECK_TRUE(security.matchesSecret(kind, String(kRotated64)),
               "new rotated secret authenticates");
  }

  Preferences::__reset();
  storeBundle(validBundle());
  EntropyScript unclaimedEntropy = sequentialEntropy(64);
  Preferences unclaimedPreferences;
  DeviceSecurity unclaimed(&unclaimedPreferences, unclaimedEntropy.source());
  CHECK_EQ(resultValue(unclaimed.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "unclaimed fixture loads");
  CHECK_EQ(resultValue(unclaimed.rotateSecret(DeviceSecretKind::ADMIN)),
           resultValue(DeviceSecurityResult::INVALID_STATE),
           "unclaimed device cannot rotate access secrets");
  CHECK_EQ(unclaimedEntropy.calls, 0U, "invalid rotation consumes no entropy");

  Preferences::__reset();
  storeBundle(validBundle(2, 1));
  EntropyScript duplicate = sequentialEntropy(0);
  Preferences duplicatePreferences;
  DeviceSecurity duplicateSecurity(&duplicatePreferences, duplicate.source());
  CHECK_EQ(resultValue(duplicateSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "duplicate fixture loads");
  CHECK_EQ(resultValue(duplicateSecurity.rotateSecret(DeviceSecretKind::ADMIN)),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "rotation rejects a credential colliding with AP secret");
  CHECK_STR(duplicateSecurity.secret(DeviceSecretKind::ADMIN), kAdminSecret,
            "failed collision leaves old credential active");

  Preferences::__reset();
  storeBundle(validBundle(2, 1));
  EntropyScript failed = sequentialEntropy(64);
  failed.failCall = 1;
  Preferences failedPreferences;
  DeviceSecurity failedSecurity(&failedPreferences, failed.source());
  CHECK_EQ(resultValue(failedSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "entropy-fail fixture loads");
  CHECK_EQ(resultValue(failedSecurity.rotateSecret(DeviceSecretKind::STAFF)),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "rotation entropy failure is fail-closed");

  Preferences::__reset();
  storeBundle(validBundle(2, 1, 0, 1,
                          std::numeric_limits<uint64_t>::max()));
  EntropyScript exhaustedEntropy = sequentialEntropy(64);
  Preferences exhaustedPreferences;
  DeviceSecurity exhausted(&exhaustedPreferences, exhaustedEntropy.source());
  CHECK_EQ(resultValue(exhausted.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "exhaustion fixture loads");
  CHECK_EQ(resultValue(exhausted.rotateSecret(DeviceSecretKind::ADMIN)),
           resultValue(DeviceSecurityResult::REVISION_EXHAUSTED),
           "rotation refuses revision wrap");
  CHECK_EQ(exhaustedEntropy.calls, 0U,
           "revision exhaustion consumes no entropy");
  CHECK_EQ(resultValue(exhausted.claimBootstrap(
             String(kBootstrapSecret), true, false)),
           resultValue(DeviceSecurityResult::INVALID_STATE),
           "claimed state still rejects first claim at max revision");
  CHECK_EQ(resultValue(exhausted.requestWipe(DeviceWipeKind::NETWORK)),
           resultValue(DeviceSecurityResult::REVISION_EXHAUSTED),
           "wipe request refuses revision wrap");
}

void testRotationWriteCuts() {
  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle(2, 1));
    EntropyScript entropy = sequentialEntropy(64);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "rotation cut fixture loads");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(security.rotateSecret(DeviceSecretKind::ADMIN)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "rotation cut reports failure");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::READY),
             "rotation cut reconciles availability");
    if (isAfterApply(mode)) {
      CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
                "applied-report-failed rotation discloses no new secret");
    } else {
      CHECK_STR(security.secret(DeviceSecretKind::ADMIN), kAdminSecret,
                "before-cut rotation retains old credential");
    }

    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(150);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "rotation cut reloads");
    CHECK_STR(rebooted.secret(DeviceSecretKind::ADMIN),
              isAfterApply(mode) ? kRotated64 : kAdminSecret,
              "rotation cut reboots to exact old or post secret");
  }
}

void testPhysicalRecovery() {
  Preferences::__reset();
  storeBundle(validBundle(2, 0, 0, 1, 20));
  EntropyScript entropy = sequentialEntropy(64);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());
  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "recovery fixture loads");

  CHECK_EQ(resultValue(security.recoverWithBootstrap(
             String(kBootstrapSecret), false)),
           resultValue(DeviceSecurityResult::DENIED),
           "recovery requires physical presence");
  CHECK_EQ(resultValue(security.recoverWithBootstrap(
             String("wrong"), true)),
           resultValue(DeviceSecurityResult::DENIED),
           "recovery rejects wrong armed token");
  CHECK_EQ(entropy.calls, 0U, "denied recovery consumes no entropy");

  CHECK_EQ(resultValue(security.recoverWithBootstrap(
             String(kBootstrapSecret), true)),
           resultValue(DeviceSecurityResult::OK),
           "physical recovery consumes armed bootstrap");
  CHECK_EQ(entropy.calls, 2U, "recovery independently rotates admin and staff");
  CHECK_TRUE(security.tokenConsumed(), "recovery token becomes one-time consumed");
  CHECK_EQ(security.revision(), 21U, "recovery advances revision once");
  CHECK_STR(security.secret(DeviceSecretKind::AP), kApSecret,
            "recovery preserves AP secret");
  CHECK_STR(security.secret(DeviceSecretKind::BOOTSTRAP), kBootstrapSecret,
            "recovery preserves consumed bootstrap bytes");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), kRotated64,
            "recovery installs new admin secret");
  CHECK_STR(security.secret(DeviceSecretKind::STAFF), kRotated80,
            "recovery installs independent staff secret");
  CHECK_EQ(resultValue(security.recoverWithBootstrap(
             String(kBootstrapSecret), true)),
           resultValue(DeviceSecurityResult::INVALID_STATE),
           "consumed recovery token cannot be reused");

  Preferences::__reset();
  storeBundle(validBundle(2, 0));
  EntropyScript failed = sequentialEntropy(64);
  failed.failCall = 2;
  Preferences failedPreferences;
  DeviceSecurity failedSecurity(&failedPreferences, failed.source());
  CHECK_EQ(resultValue(failedSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "failed recovery fixture loads");
  CHECK_EQ(resultValue(failedSecurity.recoverWithBootstrap(
             String(kBootstrapSecret), true)),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "partial recovery entropy never mutates bundle");
  CHECK_TRUE(!failedSecurity.tokenConsumed(),
             "failed recovery leaves token armed for retry");
  CHECK_STR(failedSecurity.secret(DeviceSecretKind::ADMIN), kAdminSecret,
            "failed recovery retains admin credential");
}

void testRecoveryWriteCuts() {
  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle(2, 0));
    EntropyScript entropy = sequentialEntropy(64);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "recovery cut fixture loads");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(security.recoverWithBootstrap(
               String(kBootstrapSecret), true)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "recovery write cut reports failure");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::READY),
             "recovery cut reconciles availability");

    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(160);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "recovery cut reloads");
    CHECK_EQ(rebooted.tokenConsumed(), isAfterApply(mode),
             "recovery cut reboots to armed or consumed token");
    CHECK_STR(rebooted.secret(DeviceSecretKind::ADMIN),
              isAfterApply(mode) ? kRotated64 : kAdminSecret,
              "recovery cut reboots to exact old or post credentials");
  }
}

void testNetworkWipeLifecycleAndCuts() {
  Preferences::__reset();
  storeBundle(validBundle(2, 1, 0, 1, 30));
  EntropyScript entropy = sequentialEntropy(64);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());
  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "wipe fixture loads");
  CHECK_EQ(resultValue(security.requestWipe(DeviceWipeKind::NONE)),
           resultValue(DeviceSecurityResult::INVALID_STATE),
           "empty wipe request is rejected");
  CHECK_EQ(resultValue(security.requestWipe(DeviceWipeKind::NETWORK)),
           resultValue(DeviceSecurityResult::OK),
           "network wipe marker commits before external erasure");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
           "pending wipe denies normal access");
  CHECK_EQ(wipeValue(security.wipeKind()),
           wipeValue(DeviceWipeKind::NETWORK), "network wipe kind persists");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
            "pending wipe discloses no credential");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String(kAdminSecret)),
             "pending wipe denies authentication");
  CHECK_EQ(resultValue(security.finishExternalErase(false)),
           resultValue(DeviceSecurityResult::EXTERNAL_ERASE_FAILURE),
           "failed external erase retains pending marker");
  CHECK_EQ(security.revision(), 31U,
           "failed external erase performs no state write");
  CHECK_EQ(resultValue(security.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::OK),
           "successful external erase clears marker atomically");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::READY),
           "completed network wipe restores readiness");
  CHECK_EQ(wipeValue(security.wipeKind()),
           wipeValue(DeviceWipeKind::NONE), "completed wipe clears kind");
  CHECK_TRUE(security.legacySdkErased(), "completed wipe records SDK erasure");
  CHECK_EQ(security.revision(), 32U, "wipe completion advances revision");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), kAdminSecret,
            "network wipe retains security credentials");

  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle(2, 1));
    EntropyScript cutEntropy = sequentialEntropy(64);
    Preferences cutPreferences;
    DeviceSecurity cut(&cutPreferences, cutEntropy.source());
    CHECK_EQ(resultValue(cut.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "wipe-request cut loads");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(cut.requestWipe(DeviceWipeKind::NETWORK)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "wipe-request cut reports failure");
    CHECK_EQ(availabilityValue(cut.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::READY),
             "wipe-request cut reconciles old or post state");
    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(160);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "wipe-request cut reloads");
    CHECK_EQ(availabilityValue(rebooted.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::WIPE_PENDING
               : DeviceSecurityAvailability::READY),
             "wipe request is absent or completely pending after reboot");
  }

  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle(2, 1, 1, 1, 50));
    EntropyScript cutEntropy = sequentialEntropy(64);
    Preferences cutPreferences;
    DeviceSecurity cut(&cutPreferences, cutEntropy.source());
    CHECK_EQ(resultValue(cut.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "wipe-finish cut loads");
    CHECK_EQ(availabilityValue(cut.availability()),
             availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
             "stored marker resumes coordinator before networking");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(cut.finishExternalErase(true)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "wipe-finish cut reports failure");
    CHECK_EQ(availabilityValue(cut.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::WIPE_PENDING),
             "wipe-finish cut remains closed");
    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(160);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK), "wipe-finish cut reloads");
    CHECK_EQ(availabilityValue(rebooted.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::READY
               : DeviceSecurityAvailability::WIPE_PENDING),
             "wipe finish reboots to exact pending or complete state");
  }
}

void testDecommissionAndLegacyEraseFinalization() {
  Preferences::__reset();
  storeBundle(validBundle(2, 1, 2, 1, 60));
  EntropyScript entropy = sequentialEntropy(64);
  Preferences preferences;
  DeviceSecurity security(&preferences, entropy.source());
  CHECK_EQ(resultValue(security.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "decommission marker reloads");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
           "decommission blocks all access until external erasure");
  CHECK_EQ(resultValue(security.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::OK),
           "decommission finalizes with fresh credentials");
  CHECK_EQ(entropy.calls, 4U,
           "decommission generates four new independent credentials");
  CHECK_EQ(availabilityValue(security.availability()),
           availabilityValue(DeviceSecurityAvailability::READY),
           "decommission returns a ready fresh state");
  CHECK_EQ(claimValue(security.claimState()),
           claimValue(DeviceClaimState::UNCLAIMED),
           "decommission returns to unclaimed state");
  CHECK_TRUE(!security.tokenConsumed(),
             "decommission installs an unused bootstrap token");
  CHECK_EQ(security.revision(), 61U,
           "decommission advances rather than resets revision");
  CHECK_STR(security.secret(DeviceSecretKind::AP), kRotated64,
            "decommission rotates AP secret");
  CHECK_STR(security.secret(DeviceSecretKind::BOOTSTRAP), kRotated80,
            "decommission rotates bootstrap secret");
  CHECK_STR(security.secret(DeviceSecretKind::ADMIN), "",
            "decommissioned unclaimed state hides fresh admin secret");
  CHECK_STR(security.secret(DeviceSecretKind::STAFF), "",
            "decommissioned unclaimed state hides fresh staff secret");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::ADMIN,
                                     String(kAdminSecret)),
             "decommission invalidates old admin credential");
  CHECK_TRUE(!security.matchesSecret(DeviceSecretKind::STAFF,
                                     String(kStaffSecret)),
             "decommissioned unclaimed state denies staff authentication");

  Preferences::__reset();
  storeBundle(validBundle(1, 0, 0, 0, 70));
  EntropyScript legacyEntropy = sequentialEntropy(64);
  Preferences legacyPreferences;
  DeviceSecurity legacy(&legacyPreferences, legacyEntropy.source());
  CHECK_EQ(resultValue(legacy.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK), "legacy marker loads");
  CHECK_EQ(availabilityValue(legacy.availability()),
           availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
           "legacy SDK credential erase blocks network");
  CHECK_EQ(resultValue(legacy.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::OK),
           "external SDK erase commits completion");
  CHECK_EQ(legacyEntropy.calls, 0U,
           "legacy erase does not rotate application credentials");
  CHECK_TRUE(legacy.legacySdkErased(), "legacy erase bit becomes true");

  Preferences::__reset();
  storeBundle(validBundle(2, 1, 2, 1, 80));
  EntropyScript failed = sequentialEntropy(64);
  failed.failCall = 3;
  Preferences failedPreferences;
  DeviceSecurity failedSecurity(&failedPreferences, failed.source());
  CHECK_EQ(resultValue(failedSecurity.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "decommission entropy-failure fixture loads");
  CHECK_EQ(resultValue(failedSecurity.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::ENTROPY_FAILURE),
           "decommission entropy failure retains wipe marker");
  CHECK_EQ(availabilityValue(failedSecurity.availability()),
           availabilityValue(DeviceSecurityAvailability::WIPE_PENDING),
           "failed decommission stays closed for retry");
  expectBytesEqual(storedBundle(), validBundle(2, 1, 2, 1, 80),
                   "failed decommission leaves old pending image exact");

  Preferences::__reset();
  storeBundle(validBundle(2, 1, 2, 1,
                          std::numeric_limits<uint64_t>::max()));
  EntropyScript exhaustedEntropy = sequentialEntropy(64);
  Preferences exhaustedPreferences;
  DeviceSecurity exhausted(&exhaustedPreferences, exhaustedEntropy.source());
  CHECK_EQ(resultValue(exhausted.loadOrCreate()),
           resultValue(DeviceSecurityResult::OK),
           "pending exhaustion fixture loads");
  CHECK_EQ(resultValue(exhausted.finishExternalErase(true)),
           resultValue(DeviceSecurityResult::REVISION_EXHAUSTED),
           "wipe completion refuses revision wrap");
  CHECK_EQ(exhaustedEntropy.calls, 0U,
           "exhausted decommission consumes no entropy");
}

void testDecommissionWriteCuts() {
  const std::array<Preferences::FailureMode, 4> modes = {
    Preferences::FailureMode::BEFORE_APPLY,
    Preferences::FailureMode::AFTER_APPLY,
    Preferences::FailureMode::HARD_CUT_BEFORE_APPLY,
    Preferences::FailureMode::HARD_CUT_AFTER_APPLY,
  };
  for (const auto mode : modes) {
    Preferences::__reset();
    storeBundle(validBundle(2, 1, 2, 1, 90));
    EntropyScript entropy = sequentialEntropy(64);
    Preferences preferences;
    DeviceSecurity security(&preferences, entropy.source());
    CHECK_EQ(resultValue(security.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK),
             "decommission-cut fixture loads");
    Preferences::__failWrite(1, mode);
    CHECK_EQ(resultValue(security.finishExternalErase(true)),
             resultValue(DeviceSecurityResult::STORAGE_FAILURE),
             "decommission final write cut reports failure");
    CHECK_EQ(availabilityValue(security.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::REBOOT_REQUIRED
               : DeviceSecurityAvailability::WIPE_PENDING),
             "decommission cut remains closed");
    CHECK_STR(security.secret(DeviceSecretKind::AP), "",
              "decommission cut never discloses generated credentials");

    Preferences::__simulateReboot();
    EntropyScript rebootEntropy = sequentialEntropy(160);
    Preferences rebootPreferences;
    DeviceSecurity rebooted(&rebootPreferences, rebootEntropy.source());
    CHECK_EQ(resultValue(rebooted.loadOrCreate()),
             resultValue(DeviceSecurityResult::OK),
             "decommission cut reloads");
    CHECK_EQ(availabilityValue(rebooted.availability()),
             availabilityValue(isAfterApply(mode)
               ? DeviceSecurityAvailability::READY
               : DeviceSecurityAvailability::WIPE_PENDING),
             "decommission reboots to old pending or fresh complete state");
    if (isAfterApply(mode)) {
      CHECK_STR(rebooted.secret(DeviceSecretKind::AP), kRotated64,
                "applied decommission publishes fresh credential after reboot");
      CHECK_STR(rebooted.secret(DeviceSecretKind::ADMIN), "",
                "applied decommission reboot hides unclaimed admin secret");
      CHECK_STR(rebooted.secret(DeviceSecretKind::STAFF), "",
                "applied decommission reboot hides unclaimed staff secret");
      CHECK_EQ(claimValue(rebooted.claimState()),
               claimValue(DeviceClaimState::UNCLAIMED),
               "applied decommission is wholly unclaimed");
    }
  }
}

}  // namespace

int main() {
  testFreshGenerationAndGoldenLayout();
  testEntropyFailuresFailClosed();
  testFreshWriteCuts();
  testMalformedBundlesLockWithoutFallback();
  testClaimAndConstantWorkCredentialChecks();
  testClaimWriteCuts();
  testRotationsAndRevisionExhaustion();
  testRotationWriteCuts();
  testPhysicalRecovery();
  testRecoveryWriteCuts();
  testNetworkWipeLifecycleAndCuts();
  testDecommissionAndLegacyEraseFinalization();
  testDecommissionWriteCuts();
  return testReport();
}
