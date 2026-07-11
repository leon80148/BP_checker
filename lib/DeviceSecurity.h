#ifndef DEVICE_SECURITY_H
#define DEVICE_SECURITY_H

#include <Arduino.h>
#include <Preferences.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

struct DeviceEntropySource {
  void* context;
  bool (*fill)(void* context, uint8_t* output, size_t length);
};

enum class DeviceSecurityResult : uint8_t {
  OK = 0,
  DENIED,
  INVALID_STATE,
  ENTROPY_FAILURE,
  STORAGE_FAILURE,
  CORRUPT_STATE,
  REVISION_EXHAUSTED,
  EXTERNAL_ERASE_FAILURE,
};

enum class DeviceSecurityAvailability : uint8_t {
  READY = 0,
  LOCKED,
  REBOOT_REQUIRED,
  WIPE_PENDING,
};

enum class DeviceClaimState : uint8_t {
  UNCLAIMED = 1,
  CLAIMED = 2,
};

enum class DeviceWipeKind : uint8_t {
  NONE = 0,
  NETWORK = 1,
  DECOMMISSION = 2,
};

enum class DeviceSecretKind : uint8_t {
  AP = 0,
  BOOTSTRAP = 1,
  ADMIN = 2,
  STAFF = 3,
};

class DeviceSecurity {
public:
  static constexpr size_t kBundleSize = 112;

  DeviceSecurity(Preferences* preferences, DeviceEntropySource entropy)
    : _preferences(preferences), _entropy(entropy) {
    secureZero(&_bundle, sizeof(_bundle));
  }

  DeviceSecurity(const DeviceSecurity&) = delete;
  DeviceSecurity& operator=(const DeviceSecurity&) = delete;

  ~DeviceSecurity() {
    secureZero(&_bundle, sizeof(_bundle));
  }

  DeviceSecurityResult loadOrCreate() {
    if (_loadAttempted) return DeviceSecurityResult::INVALID_STATE;
    _loadAttempted = true;
    clearRuntimeState();

    uint8_t encoded[kBundleSize] = {};
    const ReadResult read = readStored(encoded);
    if (read == ReadResult::STORAGE_ERROR) {
      secureZero(encoded, sizeof(encoded));
      return DeviceSecurityResult::STORAGE_FAILURE;
    }
    if (read == ReadResult::INVALID) {
      secureZero(encoded, sizeof(encoded));
      return DeviceSecurityResult::CORRUPT_STATE;
    }
    if (read == ReadResult::PRESENT) {
      Bundle loaded{};
      const bool valid = decode(encoded, sizeof(encoded), loaded);
      secureZero(encoded, sizeof(encoded));
      if (!valid) {
        secureZero(&loaded, sizeof(loaded));
        return DeviceSecurityResult::CORRUPT_STATE;
      }
      apply(loaded);
      secureZero(&loaded, sizeof(loaded));
      return DeviceSecurityResult::OK;
    }

    Bundle fresh{};
    fresh.claimState = static_cast<uint8_t>(DeviceClaimState::UNCLAIMED);
    fresh.tokenConsumed = 0;
    fresh.wipeKind = static_cast<uint8_t>(DeviceWipeKind::NONE);
    fresh.legacySdkErased = 0;
    fresh.revision = 1;
    if (!generateAllSecrets(fresh)) {
      secureZero(&fresh, sizeof(fresh));
      return DeviceSecurityResult::ENTROPY_FAILURE;
    }
    const DeviceSecurityResult result = commitTransition(fresh, false);
    secureZero(&fresh, sizeof(fresh));
    return result;
  }

  DeviceSecurityAvailability availability() const {
    return _availability;
  }

  DeviceClaimState claimState() const {
    return static_cast<DeviceClaimState>(_bundle.claimState);
  }

  bool tokenConsumed() const {
    return _hasBundle && _bundle.tokenConsumed == 1;
  }

  DeviceWipeKind wipeKind() const {
    return static_cast<DeviceWipeKind>(_bundle.wipeKind);
  }

  bool legacySdkErased() const {
    return _hasBundle && _bundle.legacySdkErased == 1;
  }

  uint64_t revision() const {
    return _hasBundle ? _bundle.revision : 0;
  }

  const char* secret(DeviceSecretKind kind) const {
    if (_availability != DeviceSecurityAvailability::READY || !_hasBundle ||
        !validSecretKind(kind)) {
      return "";
    }
    if ((kind == DeviceSecretKind::ADMIN ||
         kind == DeviceSecretKind::STAFF) &&
        _bundle.claimState !=
          static_cast<uint8_t>(DeviceClaimState::CLAIMED)) {
      return "";
    }
    return _bundle.secrets[secretIndex(kind)];
  }

  bool matchesSecret(DeviceSecretKind kind, const String& candidate) const {
    static const char kEmptySecret[kEncodedSecretSize + 1] = {};
    const char* expected = kEmptySecret;
    if (_hasBundle && validSecretKind(kind)) {
      expected = _bundle.secrets[secretIndex(kind)];
    }
    const bool equal = constantWorkEqual(candidate, expected);
    const bool claimedAccess =
      kind != DeviceSecretKind::ADMIN && kind != DeviceSecretKind::STAFF
        ? true
        : _bundle.claimState ==
            static_cast<uint8_t>(DeviceClaimState::CLAIMED);
    return _availability == DeviceSecurityAvailability::READY &&
           _hasBundle && validSecretKind(kind) && claimedAccess && equal;
  }

  DeviceSecurityResult claimBootstrap(const String& token,
                                      bool onProvisioningAp,
                                      bool physicalPresence) {
    const bool tokenMatches = compareBootstrap(token);
    if (_availability != DeviceSecurityAvailability::READY || !_hasBundle ||
        _bundle.claimState !=
          static_cast<uint8_t>(DeviceClaimState::UNCLAIMED) ||
        _bundle.tokenConsumed != 0) {
      return DeviceSecurityResult::INVALID_STATE;
    }
    if (!tokenMatches || (!onProvisioningAp && !physicalPresence)) {
      return DeviceSecurityResult::DENIED;
    }
    if (_bundle.revision == std::numeric_limits<uint64_t>::max()) {
      return DeviceSecurityResult::REVISION_EXHAUSTED;
    }

    Bundle post = _bundle;
    post.claimState = static_cast<uint8_t>(DeviceClaimState::CLAIMED);
    post.tokenConsumed = 1;
    post.revision++;
    const DeviceSecurityResult result = commitTransition(post, true);
    secureZero(&post, sizeof(post));
    return result;
  }

  DeviceSecurityResult rotateSecret(DeviceSecretKind kind) {
    if (_availability != DeviceSecurityAvailability::READY || !_hasBundle ||
        _bundle.claimState != static_cast<uint8_t>(DeviceClaimState::CLAIMED) ||
        !validSecretKind(kind)) {
      return DeviceSecurityResult::INVALID_STATE;
    }
    if (_bundle.revision == std::numeric_limits<uint64_t>::max()) {
      return DeviceSecurityResult::REVISION_EXHAUSTED;
    }

    Bundle post = _bundle;
    if (!generateOneSecret(post, secretIndex(kind))) {
      secureZero(&post, sizeof(post));
      return DeviceSecurityResult::ENTROPY_FAILURE;
    }
    if (kind == DeviceSecretKind::BOOTSTRAP) {
      post.tokenConsumed = 0;
    }
    post.revision++;
    const DeviceSecurityResult result = commitTransition(post, true);
    secureZero(&post, sizeof(post));
    return result;
  }

  DeviceSecurityResult recoverWithBootstrap(const String& token,
                                            bool physicalPresence) {
    const bool tokenMatches = compareBootstrap(token);
    if (_availability != DeviceSecurityAvailability::READY || !_hasBundle ||
        _bundle.claimState != static_cast<uint8_t>(DeviceClaimState::CLAIMED) ||
        _bundle.tokenConsumed != 0) {
      return DeviceSecurityResult::INVALID_STATE;
    }
    if (!tokenMatches || !physicalPresence) {
      return DeviceSecurityResult::DENIED;
    }
    if (_bundle.revision == std::numeric_limits<uint64_t>::max()) {
      return DeviceSecurityResult::REVISION_EXHAUSTED;
    }

    Bundle post = _bundle;
    if (!generateOneSecret(post, secretIndex(DeviceSecretKind::ADMIN)) ||
        !generateOneSecret(post, secretIndex(DeviceSecretKind::STAFF))) {
      secureZero(&post, sizeof(post));
      return DeviceSecurityResult::ENTROPY_FAILURE;
    }
    post.tokenConsumed = 1;
    post.revision++;
    const DeviceSecurityResult result = commitTransition(post, true);
    secureZero(&post, sizeof(post));
    return result;
  }

  DeviceSecurityResult requestWipe(DeviceWipeKind kind) {
    if (_availability != DeviceSecurityAvailability::READY || !_hasBundle ||
        (kind != DeviceWipeKind::NETWORK &&
         kind != DeviceWipeKind::DECOMMISSION)) {
      return DeviceSecurityResult::INVALID_STATE;
    }
    if (_bundle.revision == std::numeric_limits<uint64_t>::max()) {
      return DeviceSecurityResult::REVISION_EXHAUSTED;
    }

    Bundle post = _bundle;
    post.wipeKind = static_cast<uint8_t>(kind);
    post.revision++;
    const DeviceSecurityResult result = commitTransition(post, true);
    secureZero(&post, sizeof(post));
    return result;
  }

  DeviceSecurityResult finishExternalErase(bool externalEraseSucceeded) {
    if (_availability != DeviceSecurityAvailability::WIPE_PENDING ||
        !_hasBundle) {
      return DeviceSecurityResult::INVALID_STATE;
    }
    if (!externalEraseSucceeded) {
      return DeviceSecurityResult::EXTERNAL_ERASE_FAILURE;
    }
    if (_bundle.revision == std::numeric_limits<uint64_t>::max()) {
      return DeviceSecurityResult::REVISION_EXHAUSTED;
    }

    Bundle post = _bundle;
    if (_bundle.wipeKind ==
        static_cast<uint8_t>(DeviceWipeKind::DECOMMISSION)) {
      post.claimState = static_cast<uint8_t>(DeviceClaimState::UNCLAIMED);
      post.tokenConsumed = 0;
      if (!generateAllSecrets(post)) {
        secureZero(&post, sizeof(post));
        return DeviceSecurityResult::ENTROPY_FAILURE;
      }
    }
    post.wipeKind = static_cast<uint8_t>(DeviceWipeKind::NONE);
    post.legacySdkErased = 1;
    post.revision++;
    const DeviceSecurityResult result = commitTransition(post, true);
    secureZero(&post, sizeof(post));
    return result;
  }

private:
  static constexpr size_t kRawSecretSize = 16;
  static constexpr size_t kEncodedSecretSize = 22;
  static constexpr size_t kSecretCount = 4;
  static constexpr size_t kCrcOffset = 108;
  static constexpr size_t kApOffset = 20;
  static constexpr size_t kBootstrapOffset = 42;
  static constexpr size_t kAdminOffset = 64;
  static constexpr size_t kStaffOffset = 86;

  struct Bundle {
    uint8_t claimState;
    uint8_t tokenConsumed;
    uint8_t wipeKind;
    uint8_t legacySdkErased;
    uint64_t revision;
    char secrets[kSecretCount][kEncodedSecretSize + 1];
  };

  enum class ReadResult : uint8_t {
    MISSING,
    PRESENT,
    INVALID,
    STORAGE_ERROR,
  };

  Preferences* _preferences = nullptr;
  DeviceEntropySource _entropy{};
  Bundle _bundle{};
  bool _hasBundle = false;
  bool _loadAttempted = false;
  DeviceSecurityAvailability _availability =
    DeviceSecurityAvailability::LOCKED;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- > 0) {
      *bytes++ = 0;
    }
  }

  void clearRuntimeState() {
    secureZero(&_bundle, sizeof(_bundle));
    _hasBundle = false;
    _availability = DeviceSecurityAvailability::LOCKED;
  }

  static bool validSecretKind(DeviceSecretKind kind) {
    return static_cast<uint8_t>(kind) < kSecretCount;
  }

  static size_t secretIndex(DeviceSecretKind kind) {
    return static_cast<size_t>(kind);
  }

  static uint32_t crc32(const uint8_t* bytes, size_t length) {
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

  static void writeLe32(uint8_t* target, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
      target[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
    }
  }

  static void writeLe64(uint8_t* target, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
      target[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
    }
  }

  static uint32_t readLe32(const uint8_t* source) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
      value |= static_cast<uint32_t>(source[i]) << (8U * i);
    }
    return value;
  }

  static uint64_t readLe64(const uint8_t* source) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
      value |= static_cast<uint64_t>(source[i]) << (8U * i);
    }
    return value;
  }

  static int base64UrlIndex(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
  }

  static bool validEncodedSecret(const char* secret) {
    for (size_t i = 0; i < kEncodedSecretSize; ++i) {
      if (base64UrlIndex(secret[i]) < 0) return false;
    }
    const int finalIndex = base64UrlIndex(secret[kEncodedSecretSize - 1]);
    return finalIndex >= 0 && (finalIndex & 0x0f) == 0;
  }

  static bool equalSecretBytes(const char* left, const char* right) {
    uint8_t difference = 0;
    for (size_t i = 0; i < kEncodedSecretSize; ++i) {
      difference |= static_cast<uint8_t>(left[i]) ^
                    static_cast<uint8_t>(right[i]);
    }
    return difference == 0;
  }

  static bool constantWorkEqual(const String& candidate,
                                const char* expected) {
    uint32_t difference =
      static_cast<uint32_t>(candidate.length() ^ kEncodedSecretSize);
    for (size_t i = 0; i < kEncodedSecretSize; ++i) {
      difference |= static_cast<uint8_t>(candidate.charAt(
                      static_cast<unsigned int>(i))) ^
                    static_cast<uint8_t>(expected[i]);
    }
    return difference == 0;
  }

  bool compareBootstrap(const String& candidate) const {
    static const char kEmptySecret[kEncodedSecretSize + 1] = {};
    return constantWorkEqual(candidate,
      _hasBundle
        ? _bundle.secrets[secretIndex(DeviceSecretKind::BOOTSTRAP)]
        : kEmptySecret);
  }

  static void encodeBase64Url(const uint8_t raw[kRawSecretSize],
                              char encoded[kEncodedSecretSize + 1]) {
    static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t input = 0;
    size_t output = 0;
    while (input + 3 <= kRawSecretSize) {
      const uint32_t value =
        (static_cast<uint32_t>(raw[input]) << 16U) |
        (static_cast<uint32_t>(raw[input + 1]) << 8U) |
        static_cast<uint32_t>(raw[input + 2]);
      encoded[output++] = kAlphabet[(value >> 18U) & 0x3fU];
      encoded[output++] = kAlphabet[(value >> 12U) & 0x3fU];
      encoded[output++] = kAlphabet[(value >> 6U) & 0x3fU];
      encoded[output++] = kAlphabet[value & 0x3fU];
      input += 3;
    }
    const uint8_t finalByte = raw[kRawSecretSize - 1];
    encoded[output++] = kAlphabet[(finalByte >> 2U) & 0x3fU];
    encoded[output++] = kAlphabet[(finalByte & 0x03U) << 4U];
    encoded[output] = '\0';
  }

  bool fillRawSecret(uint8_t raw[kRawSecretSize]) const {
    if (_entropy.fill == nullptr ||
        !_entropy.fill(_entropy.context, raw, kRawSecretSize)) {
      return false;
    }
    uint8_t anySet = 0;
    uint8_t anyClear = 0;
    for (size_t i = 0; i < kRawSecretSize; ++i) {
      anySet |= raw[i];
      anyClear |= static_cast<uint8_t>(~raw[i]);
    }
    return anySet != 0 && anyClear != 0;
  }

  static bool rawSecretsEqual(const uint8_t* left, const uint8_t* right) {
    uint8_t difference = 0;
    for (size_t i = 0; i < kRawSecretSize; ++i) {
      difference |= left[i] ^ right[i];
    }
    return difference == 0;
  }

  bool generateAllSecrets(Bundle& target) const {
    uint8_t raw[kSecretCount][kRawSecretSize] = {};
    bool valid = true;
    for (size_t secret = 0; secret < kSecretCount && valid; ++secret) {
      valid = fillRawSecret(raw[secret]);
      for (size_t previous = 0; previous < secret && valid; ++previous) {
        if (rawSecretsEqual(raw[secret], raw[previous])) valid = false;
      }
      if (valid) encodeBase64Url(raw[secret], target.secrets[secret]);
    }
    secureZero(raw, sizeof(raw));
    return valid;
  }

  bool generateOneSecret(Bundle& target, size_t targetIndex) const {
    uint8_t raw[kRawSecretSize] = {};
    char encoded[kEncodedSecretSize + 1] = {};
    bool valid = fillRawSecret(raw);
    if (valid) {
      encodeBase64Url(raw, encoded);
      for (size_t existing = 0; existing < kSecretCount; ++existing) {
        if (equalSecretBytes(encoded, target.secrets[existing])) {
          valid = false;
        }
      }
    }
    if (valid) {
      memcpy(target.secrets[targetIndex], encoded,
             kEncodedSecretSize + 1);
    }
    secureZero(raw, sizeof(raw));
    secureZero(encoded, sizeof(encoded));
    return valid;
  }

  static bool validBundleSemantics(const Bundle& bundle) {
    if (bundle.claimState !=
          static_cast<uint8_t>(DeviceClaimState::UNCLAIMED) &&
        bundle.claimState !=
          static_cast<uint8_t>(DeviceClaimState::CLAIMED)) {
      return false;
    }
    if (bundle.tokenConsumed > 1 || bundle.wipeKind > 2 ||
        bundle.legacySdkErased > 1 || bundle.revision == 0) {
      return false;
    }
    if (bundle.claimState ==
          static_cast<uint8_t>(DeviceClaimState::UNCLAIMED) &&
        bundle.tokenConsumed != 0) {
      return false;
    }
    for (size_t i = 0; i < kSecretCount; ++i) {
      if (!validEncodedSecret(bundle.secrets[i]) ||
          bundle.secrets[i][kEncodedSecretSize] != '\0') {
        return false;
      }
      for (size_t previous = 0; previous < i; ++previous) {
        if (equalSecretBytes(bundle.secrets[i], bundle.secrets[previous])) {
          return false;
        }
      }
    }
    return true;
  }

  static void encode(const Bundle& bundle, uint8_t encoded[kBundleSize]) {
    secureZero(encoded, kBundleSize);
    encoded[0] = 'B';
    encoded[1] = 'P';
    encoded[2] = 'S';
    encoded[3] = 'C';
    encoded[4] = 1;
    encoded[5] = bundle.claimState;
    encoded[6] = bundle.tokenConsumed;
    encoded[7] = bundle.wipeKind;
    encoded[8] = bundle.legacySdkErased;
    writeLe64(encoded + 12, bundle.revision);
    memcpy(encoded + kApOffset,
           bundle.secrets[secretIndex(DeviceSecretKind::AP)],
           kEncodedSecretSize);
    memcpy(encoded + kBootstrapOffset,
           bundle.secrets[secretIndex(DeviceSecretKind::BOOTSTRAP)],
           kEncodedSecretSize);
    memcpy(encoded + kAdminOffset,
           bundle.secrets[secretIndex(DeviceSecretKind::ADMIN)],
           kEncodedSecretSize);
    memcpy(encoded + kStaffOffset,
           bundle.secrets[secretIndex(DeviceSecretKind::STAFF)],
           kEncodedSecretSize);
    writeLe32(encoded + kCrcOffset, crc32(encoded, kCrcOffset));
  }

  static bool decode(const uint8_t* encoded, size_t length, Bundle& bundle) {
    if (encoded == nullptr || length != kBundleSize ||
        encoded[0] != 'B' || encoded[1] != 'P' ||
        encoded[2] != 'S' || encoded[3] != 'C' ||
        encoded[4] != 1 || encoded[9] != 0 || encoded[10] != 0 ||
        encoded[11] != 0 ||
        readLe32(encoded + kCrcOffset) != crc32(encoded, kCrcOffset)) {
      return false;
    }

    secureZero(&bundle, sizeof(bundle));
    bundle.claimState = encoded[5];
    bundle.tokenConsumed = encoded[6];
    bundle.wipeKind = encoded[7];
    bundle.legacySdkErased = encoded[8];
    bundle.revision = readLe64(encoded + 12);
    memcpy(bundle.secrets[secretIndex(DeviceSecretKind::AP)],
           encoded + kApOffset, kEncodedSecretSize);
    memcpy(bundle.secrets[secretIndex(DeviceSecretKind::BOOTSTRAP)],
           encoded + kBootstrapOffset, kEncodedSecretSize);
    memcpy(bundle.secrets[secretIndex(DeviceSecretKind::ADMIN)],
           encoded + kAdminOffset, kEncodedSecretSize);
    memcpy(bundle.secrets[secretIndex(DeviceSecretKind::STAFF)],
           encoded + kStaffOffset, kEncodedSecretSize);
    for (size_t i = 0; i < kSecretCount; ++i) {
      bundle.secrets[i][kEncodedSecretSize] = '\0';
    }
    return validBundleSemantics(bundle);
  }

  ReadResult readStored(uint8_t encoded[kBundleSize]) {
    if (_preferences == nullptr ||
        !_preferences->begin("bp_sec", false)) {
      return ReadResult::STORAGE_ERROR;
    }
    if (!_preferences->isKey("sec_state")) {
      _preferences->end();
      return ReadResult::MISSING;
    }
    if (_preferences->getType("sec_state") != PT_BLOB ||
        _preferences->getBytesLength("sec_state") != kBundleSize) {
      _preferences->end();
      return ReadResult::INVALID;
    }
    const size_t read = _preferences->getBytes(
      "sec_state", encoded, kBundleSize);
    _preferences->end();
    return read == kBundleSize ? ReadResult::PRESENT : ReadResult::INVALID;
  }

  bool writeStored(const uint8_t encoded[kBundleSize]) {
    if (_preferences == nullptr ||
        !_preferences->begin("bp_sec", false)) {
      return false;
    }
    const size_t written = _preferences->putBytes(
      "sec_state", encoded, kBundleSize);
    _preferences->end();
    return written == kBundleSize;
  }

  static bool encodedEqual(const uint8_t* left, const uint8_t* right) {
    uint8_t difference = 0;
    for (size_t i = 0; i < kBundleSize; ++i) {
      difference |= left[i] ^ right[i];
    }
    return difference == 0;
  }

  void apply(const Bundle& bundle) {
    secureZero(&_bundle, sizeof(_bundle));
    memcpy(&_bundle, &bundle, sizeof(bundle));
    _hasBundle = true;
    _availability =
      bundle.wipeKind != static_cast<uint8_t>(DeviceWipeKind::NONE) ||
      bundle.legacySdkErased == 0
        ? DeviceSecurityAvailability::WIPE_PENDING
        : DeviceSecurityAvailability::READY;
  }

  void lock() {
    clearRuntimeState();
  }

  DeviceSecurityResult commitTransition(const Bundle& post,
                                        bool oldExists) {
    uint8_t oldEncoded[kBundleSize] = {};
    uint8_t postEncoded[kBundleSize] = {};
    uint8_t observed[kBundleSize] = {};
    if (oldExists) encode(_bundle, oldEncoded);
    encode(post, postEncoded);

    const bool writeReportedSuccess = writeStored(postEncoded);
    const ReadResult read = readStored(observed);

    if (writeReportedSuccess) {
      if (read == ReadResult::PRESENT &&
          encodedEqual(observed, postEncoded)) {
        Bundle verified{};
        const bool decoded = decode(observed, sizeof(observed), verified);
        if (decoded) apply(verified);
        else lock();
        secureZero(&verified, sizeof(verified));
        secureZero(oldEncoded, sizeof(oldEncoded));
        secureZero(postEncoded, sizeof(postEncoded));
        secureZero(observed, sizeof(observed));
        return decoded
          ? DeviceSecurityResult::OK
          : DeviceSecurityResult::CORRUPT_STATE;
      }
      lock();
      secureZero(oldEncoded, sizeof(oldEncoded));
      secureZero(postEncoded, sizeof(postEncoded));
      secureZero(observed, sizeof(observed));
      return read == ReadResult::INVALID
        ? DeviceSecurityResult::CORRUPT_STATE
        : DeviceSecurityResult::STORAGE_FAILURE;
    }

    if (oldExists && read == ReadResult::PRESENT &&
        encodedEqual(observed, oldEncoded)) {
      Bundle old{};
      const bool decoded = decode(observed, sizeof(observed), old);
      if (decoded) apply(old);
      else lock();
      secureZero(&old, sizeof(old));
      secureZero(oldEncoded, sizeof(oldEncoded));
      secureZero(postEncoded, sizeof(postEncoded));
      secureZero(observed, sizeof(observed));
      return decoded
        ? DeviceSecurityResult::STORAGE_FAILURE
        : DeviceSecurityResult::CORRUPT_STATE;
    }

    if (!oldExists && read == ReadResult::MISSING) {
      lock();
      secureZero(oldEncoded, sizeof(oldEncoded));
      secureZero(postEncoded, sizeof(postEncoded));
      secureZero(observed, sizeof(observed));
      return DeviceSecurityResult::STORAGE_FAILURE;
    }

    if (read == ReadResult::PRESENT &&
        encodedEqual(observed, postEncoded)) {
      Bundle appliedPost{};
      const bool decoded = decode(observed, sizeof(observed), appliedPost);
      if (decoded) {
        apply(appliedPost);
        _availability = DeviceSecurityAvailability::REBOOT_REQUIRED;
      } else {
        lock();
      }
      secureZero(&appliedPost, sizeof(appliedPost));
      secureZero(oldEncoded, sizeof(oldEncoded));
      secureZero(postEncoded, sizeof(postEncoded));
      secureZero(observed, sizeof(observed));
      return decoded
        ? DeviceSecurityResult::STORAGE_FAILURE
        : DeviceSecurityResult::CORRUPT_STATE;
    }

    lock();
    secureZero(oldEncoded, sizeof(oldEncoded));
    secureZero(postEncoded, sizeof(postEncoded));
    secureZero(observed, sizeof(observed));
    return read == ReadResult::INVALID
      ? DeviceSecurityResult::CORRUPT_STATE
      : DeviceSecurityResult::STORAGE_FAILURE;
  }
};

#endif
