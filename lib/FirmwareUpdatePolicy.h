#ifndef BP_FIRMWARE_UPDATE_POLICY_H
#define BP_FIRMWARE_UPDATE_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bp_update {

static constexpr size_t kManifestMaxBytes = 384;
static constexpr size_t kVersionCapacity = 33;
static constexpr size_t kTargetCapacity = 33;
static constexpr size_t kSourceShaCapacity = 41;
static constexpr size_t kSha256Capacity = 65;
static constexpr size_t kSequenceSlotBytes = 32;
static constexpr size_t kPendingSignatureMaxBytes = 80;
static constexpr size_t kPendingReceiptBytes = 480;

enum class Result : uint8_t {
  OK = 0,
  INVALID_ARGUMENT,
  MALFORMED_MANIFEST,
  WRONG_TARGET,
  INCOMPATIBLE_SEQUENCE,
  SIGNATURE_UNAVAILABLE,
  SIGNATURE_INVALID,
  STREAM_STATE,
  ARTIFACT_SIZE,
  ARTIFACT_HASH,
  ARTIFACT_WRITE,
  STORAGE_UNINITIALIZED,
  STORAGE_CORRUPT,
  STORAGE_FAILURE,
  SEQUENCE_EXHAUSTED,
  HEALTH_FAILED,
  BOOT_CONFIRM_FAILED,
};

struct Manifest {
  char version[kVersionCapacity] = {};
  char target[kTargetCapacity] = {};
  char sourceSha[kSourceShaCapacity] = {};
  uint64_t sequence = 0;
  uint64_t minimumSequence = 0;
  uint32_t artifactSize = 0;
  char artifactSha256[kSha256Capacity] = {};
};

struct SignatureVerifier;

class AuthorizedManifest {
public:
  AuthorizedManifest() = default;
  const Manifest& manifest() const { return _manifest; }
  bool valid() const { return _valid; }

private:
  Manifest _manifest{};
  uint64_t _authorizedAgainstSequence = 0;
  bool _valid = false;

  void clear() {
    memset(&_manifest, 0, sizeof(_manifest));
    _authorizedAgainstSequence = 0;
    _valid = false;
  }

  friend Result authorizeManifest(const char*, size_t, const uint8_t*, size_t,
                                  const char*, uint64_t,
                                  const SignatureVerifier&,
                                  AuthorizedManifest&);
  friend class ArtifactStreamPolicy;
  friend class PendingBootPolicy;
};

inline bool decimalComponent(const char* value, size_t length) {
  if (value == nullptr || length == 0) return false;
  if (length > 1 && value[0] == '0') return false;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

inline bool validSemanticVersion(const char* value) {
  if (value == nullptr) return false;
  const size_t length = strnlen(value, kVersionCapacity);
  if (length == 0 || length >= kVersionCapacity) return false;
  size_t offset = 0;
  for (int component = 0; component < 3; ++component) {
    const size_t start = offset;
    while (offset < length && value[offset] >= '0' && value[offset] <= '9') {
      ++offset;
    }
    if (!decimalComponent(value + start, offset - start)) return false;
    if (component < 2) {
      if (offset >= length || value[offset] != '.') return false;
      ++offset;
    }
  }
  if (offset == length) return true;
  if (value[offset++] != '-' || offset == length) return false;
  bool identifierHasByte = false;
  bool identifierNumeric = true;
  size_t identifierStart = offset;
  for (; offset <= length; ++offset) {
    const bool atEnd = offset == length;
    const char current = atEnd ? '.' : value[offset];
    if (current == '.') {
      if (!identifierHasByte) return false;
      if (identifierNumeric && offset - identifierStart > 1 &&
          value[identifierStart] == '0') {
        return false;
      }
      identifierHasByte = false;
      identifierNumeric = true;
      identifierStart = offset + 1;
      continue;
    }
    const bool allowed =
      (current >= 'A' && current <= 'Z') ||
      (current >= 'a' && current <= 'z') ||
      (current >= '0' && current <= '9') || current == '-';
    if (!allowed) return false;
    identifierHasByte = true;
    if (current < '0' || current > '9') identifierNumeric = false;
  }
  return true;
}

inline bool fixedLowerHex(const char* value, size_t length) {
  if (value == nullptr) return false;
  for (size_t i = 0; i < length; ++i) {
    const char byte = value[i];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
      return false;
    }
  }
  return value[length] == '\0';
}

inline bool parseCanonicalUnsigned(const char* value, size_t length,
                                   uint64_t& parsed) {
  if (value == nullptr || length == 0 || (length > 1 && value[0] == '0')) {
    return false;
  }
  uint64_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(value[i] - '0');
    if (result > (UINT64_MAX - digit) / 10U) return false;
    result = result * 10U + digit;
  }
  parsed = result;
  return true;
}

inline bool copyExact(char* target, size_t capacity,
                      const char* source, size_t length) {
  if (target == nullptr || source == nullptr || capacity == 0 ||
      length == 0 || length >= capacity) {
    return false;
  }
  memset(target, 0, capacity);
  memcpy(target, source, length);
  return true;
}

inline bool takeLine(const char* input, size_t length, size_t& offset,
                     const char*& line, size_t& lineLength) {
  if (input == nullptr || offset >= length) return false;
  const size_t start = offset;
  while (offset < length && input[offset] != '\n') {
    const uint8_t byte = static_cast<uint8_t>(input[offset]);
    if (byte < 0x20U || byte > 0x7eU || input[offset] == '\r') return false;
    ++offset;
  }
  if (offset >= length || offset == start) return false;
  line = input + start;
  lineLength = offset - start;
  ++offset;
  return true;
}

inline bool takeField(const char* line, size_t lineLength,
                      const char* expectedName,
                      const char*& value, size_t& valueLength) {
  const size_t nameLength = strlen(expectedName);
  if (line == nullptr || lineLength <= nameLength + 1U ||
      memcmp(line, expectedName, nameLength) != 0 ||
      line[nameLength] != '=') {
    return false;
  }
  value = line + nameLength + 1U;
  valueLength = lineLength - nameLength - 1U;
  return valueLength != 0;
}

inline Result parseManifest(const char* bytes, size_t length, Manifest& output) {
  memset(&output, 0, sizeof(output));
  if (bytes == nullptr || length == 0 || length > kManifestMaxBytes ||
      bytes[length - 1] != '\n') {
    return Result::MALFORMED_MANIFEST;
  }
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] == '\0') return Result::MALFORMED_MANIFEST;
  }

  static constexpr const char* kNames[] = {
    "schema", "version", "target", "source_sha", "sequence",
    "minimum_sequence", "size", "sha256"
  };
  const char* values[8] = {};
  size_t valueLengths[8] = {};
  size_t offset = 0;
  for (size_t i = 0; i < 8; ++i) {
    const char* line = nullptr;
    size_t lineLength = 0;
    if (!takeLine(bytes, length, offset, line, lineLength) ||
        !takeField(line, lineLength, kNames[i], values[i], valueLengths[i])) {
      return Result::MALFORMED_MANIFEST;
    }
  }
  if (offset != length || valueLengths[0] != strlen("bp-update-v1") ||
      memcmp(values[0], "bp-update-v1", valueLengths[0]) != 0 ||
      !copyExact(output.version, sizeof(output.version),
                 values[1], valueLengths[1]) ||
      !validSemanticVersion(output.version) ||
      !copyExact(output.target, sizeof(output.target),
                 values[2], valueLengths[2]) ||
      !copyExact(output.sourceSha, sizeof(output.sourceSha),
                 values[3], valueLengths[3]) ||
      !fixedLowerHex(output.sourceSha, 40) ||
      !copyExact(output.artifactSha256, sizeof(output.artifactSha256),
                 values[7], valueLengths[7]) ||
      !fixedLowerHex(output.artifactSha256, 64)) {
    memset(&output, 0, sizeof(output));
    return Result::MALFORMED_MANIFEST;
  }
  uint64_t size = 0;
  if (!parseCanonicalUnsigned(values[4], valueLengths[4], output.sequence) ||
      !parseCanonicalUnsigned(values[5], valueLengths[5],
                              output.minimumSequence) ||
      !parseCanonicalUnsigned(values[6], valueLengths[6], size) ||
      output.sequence == 0 || size == 0 || size > UINT32_MAX) {
    memset(&output, 0, sizeof(output));
    return Result::MALFORMED_MANIFEST;
  }
  output.artifactSize = static_cast<uint32_t>(size);
  return Result::OK;
}

using SignatureVerifyFn = bool (*)(void* context,
                                   const uint8_t* canonicalManifest,
                                   size_t manifestLength,
                                   const uint8_t* signature,
                                   size_t signatureLength);

struct SignatureVerifier {
  void* context = nullptr;
  SignatureVerifyFn verify = nullptr;
  bool trustAnchorConfigured = false;
};

inline Result authorizeManifest(const char* bytes, size_t length,
                                const uint8_t* signature,
                                size_t signatureLength,
                                const char* expectedTarget,
                                uint64_t highestAcceptedSequence,
                                const SignatureVerifier& verifier,
                                AuthorizedManifest& authorized) {
  authorized.clear();
  Manifest manifest{};
  Result result = parseManifest(bytes, length, manifest);
  if (result != Result::OK) return result;
  if (expectedTarget == nullptr || strcmp(manifest.target, expectedTarget) != 0) {
    return Result::WRONG_TARGET;
  }
  if (manifest.sequence <= highestAcceptedSequence ||
      manifest.minimumSequence > highestAcceptedSequence) {
    return Result::INCOMPATIBLE_SEQUENCE;
  }
  if (!verifier.trustAnchorConfigured || verifier.verify == nullptr) {
    return Result::SIGNATURE_UNAVAILABLE;
  }
  if (signature == nullptr || signatureLength == 0 ||
      !verifier.verify(verifier.context,
                       reinterpret_cast<const uint8_t*>(bytes), length,
                       signature, signatureLength)) {
    return Result::SIGNATURE_INVALID;
  }
  authorized._manifest = manifest;
  authorized._authorizedAgainstSequence = highestAcceptedSequence;
  authorized._valid = true;
  return Result::OK;
}

using ArtifactBeginFn = bool (*)(void* context, uint32_t expectedSize);
using ArtifactWriteFn = bool (*)(void* context, const uint8_t* bytes,
                                 size_t length);
using ArtifactFinishFn = bool (*)(void* context, uint8_t digest[32]);
using ArtifactAbortFn = void (*)(void* context);

struct ArtifactCallbacks {
  void* context = nullptr;
  ArtifactBeginFn begin = nullptr;
  ArtifactWriteFn write = nullptr;
  ArtifactFinishFn finish = nullptr;
  ArtifactAbortFn abort = nullptr;
};

inline int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

inline bool decodeSha256(const char* encoded, uint8_t digest[32]) {
  if (!fixedLowerHex(encoded, 64)) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexNibble(encoded[i * 2]);
    const int low = hexNibble(encoded[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    digest[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

inline bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                              size_t length) {
  if (left == nullptr || right == nullptr) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

class ArtifactStreamPolicy {
public:
  ArtifactStreamPolicy() = default;
  ArtifactStreamPolicy(const ArtifactStreamPolicy&) = delete;
  ArtifactStreamPolicy& operator=(const ArtifactStreamPolicy&) = delete;
  ~ArtifactStreamPolicy() { abort(); }

  Result begin(const AuthorizedManifest& authorized,
               const ArtifactCallbacks& callbacks) {
    if (!authorized._valid) return Result::SIGNATURE_INVALID;
    const Manifest& manifest = authorized._manifest;
    if (_active || callbacks.begin == nullptr || callbacks.write == nullptr ||
        callbacks.finish == nullptr || callbacks.abort == nullptr ||
        manifest.artifactSize == 0 ||
        !decodeSha256(manifest.artifactSha256, _expectedDigest)) {
      return Result::INVALID_ARGUMENT;
    }
    _callbacks = callbacks;
    _expectedSize = manifest.artifactSize;
    _received = 0;
    if (!_callbacks.begin(_callbacks.context, _expectedSize)) {
      _callbacks.abort(_callbacks.context);
      clear();
      return Result::ARTIFACT_WRITE;
    }
    _active = true;
    return Result::OK;
  }

  Result write(const uint8_t* bytes, size_t length) {
    if (!_active || bytes == nullptr || length == 0) return Result::STREAM_STATE;
    if (length > _expectedSize - _received) {
      abort();
      return Result::ARTIFACT_SIZE;
    }
    if (!_callbacks.write(_callbacks.context, bytes, length)) {
      abort();
      return Result::ARTIFACT_WRITE;
    }
    _received += static_cast<uint32_t>(length);
    return Result::OK;
  }

  Result finish() {
    if (!_active) return Result::STREAM_STATE;
    if (_received != _expectedSize) {
      abort();
      return Result::ARTIFACT_SIZE;
    }
    uint8_t actual[32] = {};
    if (!_callbacks.finish(_callbacks.context, actual)) {
      abort();
      return Result::ARTIFACT_WRITE;
    }
    const bool matches = constantTimeEqual(actual, _expectedDigest, 32);
    memset(actual, 0, sizeof(actual));
    if (!matches) {
      abort();
      return Result::ARTIFACT_HASH;
    }
    clear();
    return Result::OK;
  }

  void abort() {
    if (_active && _callbacks.abort != nullptr) {
      _callbacks.abort(_callbacks.context);
    }
    clear();
  }

  bool active() const { return _active; }
  uint32_t received() const { return _received; }

private:
  ArtifactCallbacks _callbacks{};
  uint8_t _expectedDigest[32] = {};
  uint32_t _expectedSize = 0;
  uint32_t _received = 0;
  bool _active = false;

  void clear() {
    memset(_expectedDigest, 0, sizeof(_expectedDigest));
    _callbacks = {};
    _expectedSize = 0;
    _received = 0;
    _active = false;
  }
};

using SlotReadFn = bool (*)(void* context, uint8_t slot,
                            uint8_t* bytes, size_t length, bool& present);
using SlotWriteFn = bool (*)(void* context, uint8_t slot,
                             const uint8_t* bytes, size_t length);

struct SequenceStorage {
  void* context = nullptr;
  SlotReadFn read = nullptr;
  SlotWriteFn write = nullptr;
};

inline uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
    }
  }
  return ~crc;
}

inline void writeLe64(uint8_t* output, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    output[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
  }
}

inline uint64_t readLe64(const uint8_t* input) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(input[i]) << (8U * i);
  }
  return value;
}

inline void writeLe32(uint8_t* output, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    output[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xffU);
  }
}

inline uint32_t readLe32(const uint8_t* input) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(input[i]) << (8U * i);
  }
  return value;
}

inline void writeLe16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value & 0xffU);
  output[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
}

inline uint16_t readLe16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

struct PendingUpdateReceipt {
  uint16_t manifestLength = 0;
  uint16_t signatureLength = 0;
  uint8_t manifest[kManifestMaxBytes] = {};
  uint8_t signature[kPendingSignatureMaxBytes] = {};
};

inline Result makePendingUpdateReceipt(
    const uint8_t* manifest, size_t manifestLength,
    const uint8_t* signature, size_t signatureLength,
    PendingUpdateReceipt& output) {
  memset(&output, 0, sizeof(output));
  if (manifest == nullptr || manifestLength == 0 ||
      manifestLength > sizeof(output.manifest) || signature == nullptr ||
      signatureLength == 0 || signatureLength > sizeof(output.signature)) {
    return Result::INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < manifestLength; ++i) {
    if (manifest[i] == 0) return Result::INVALID_ARGUMENT;
  }
  output.manifestLength = static_cast<uint16_t>(manifestLength);
  output.signatureLength = static_cast<uint16_t>(signatureLength);
  memcpy(output.manifest, manifest, manifestLength);
  memcpy(output.signature, signature, signatureLength);
  return Result::OK;
}

inline bool encodePendingUpdateReceipt(const PendingUpdateReceipt& receipt,
                                       uint8_t* output, size_t length) {
  if (output == nullptr || length != kPendingReceiptBytes ||
      receipt.manifestLength == 0 ||
      receipt.manifestLength > sizeof(receipt.manifest) ||
      receipt.signatureLength == 0 ||
      receipt.signatureLength > sizeof(receipt.signature)) {
    return false;
  }
  memset(output, 0, length);
  output[0] = 'B'; output[1] = 'P'; output[2] = 'U'; output[3] = 'R';
  output[4] = 1;
  writeLe16(output + 8, receipt.manifestLength);
  writeLe16(output + 10, receipt.signatureLength);
  memcpy(output + 12, receipt.manifest, receipt.manifestLength);
  memcpy(output + 12 + kManifestMaxBytes, receipt.signature,
         receipt.signatureLength);
  writeLe32(output + length - 4, crc32(output, length - 4));
  return true;
}

inline Result decodePendingUpdateReceipt(const uint8_t* encoded, size_t length,
                                         PendingUpdateReceipt& output) {
  memset(&output, 0, sizeof(output));
  if (encoded == nullptr || length != kPendingReceiptBytes ||
      encoded[0] != 'B' || encoded[1] != 'P' || encoded[2] != 'U' ||
      encoded[3] != 'R' || encoded[4] != 1 || encoded[5] != 0 ||
      encoded[6] != 0 || encoded[7] != 0 ||
      readLe32(encoded + length - 4) != crc32(encoded, length - 4)) {
    return Result::STORAGE_CORRUPT;
  }
  const uint16_t manifestLength = readLe16(encoded + 8);
  const uint16_t signatureLength = readLe16(encoded + 10);
  if (manifestLength == 0 || manifestLength > sizeof(output.manifest) ||
      signatureLength == 0 || signatureLength > sizeof(output.signature)) {
    return Result::STORAGE_CORRUPT;
  }
  for (size_t i = manifestLength; i < kManifestMaxBytes; ++i) {
    if (encoded[12 + i] != 0) return Result::STORAGE_CORRUPT;
  }
  const size_t signatureOffset = 12 + kManifestMaxBytes;
  for (size_t i = signatureLength; i < kPendingSignatureMaxBytes; ++i) {
    if (encoded[signatureOffset + i] != 0) return Result::STORAGE_CORRUPT;
  }
  memcpy(output.manifest, encoded + 12, manifestLength);
  memcpy(output.signature, encoded + signatureOffset, signatureLength);
  output.manifestLength = manifestLength;
  output.signatureLength = signatureLength;
  return Result::OK;
}

struct SequenceSlot {
  uint64_t generation = 0;
  uint64_t sequence = 0;
};

inline void encodeSequenceSlot(const SequenceSlot& slot,
                               uint8_t output[kSequenceSlotBytes]) {
  memset(output, 0, kSequenceSlotBytes);
  output[0] = 'B'; output[1] = 'P'; output[2] = 'U'; output[3] = 'S';
  output[4] = 1;
  writeLe64(output + 8, slot.generation);
  writeLe64(output + 16, slot.sequence);
  writeLe32(output + 28, crc32(output, 28));
}

inline bool decodeSequenceSlot(const uint8_t input[kSequenceSlotBytes],
                               SequenceSlot& slot) {
  if (input[0] != 'B' || input[1] != 'P' || input[2] != 'U' ||
      input[3] != 'S' || input[4] != 1 || input[5] != 0 ||
      input[6] != 0 || input[7] != 0 ||
      readLe32(input + 28) != crc32(input, 28)) {
    return false;
  }
  slot.generation = readLe64(input + 8);
  slot.sequence = readLe64(input + 16);
  return slot.generation != 0;
}

class MonotonicSequenceStore {
public:
  explicit MonotonicSequenceStore(SequenceStorage storage) : _storage(storage) {}

  Result load() {
    _ready = false;
    if (_storage.read == nullptr || _storage.write == nullptr) {
      return Result::STORAGE_FAILURE;
    }
    SequenceSlot slots[2] = {};
    bool valid[2] = {};
    bool present[2] = {};
    for (uint8_t i = 0; i < 2; ++i) {
      uint8_t encoded[kSequenceSlotBytes] = {};
      if (!_storage.read(_storage.context, i, encoded, sizeof(encoded), present[i])) {
        return Result::STORAGE_FAILURE;
      }
      valid[i] = present[i] && decodeSequenceSlot(encoded, slots[i]);
    }
    if (!present[0] && !present[1]) return Result::STORAGE_UNINITIALIZED;
    if ((present[0] && !valid[0]) || (present[1] && !valid[1])) {
      return Result::STORAGE_CORRUPT;
    }
    if (!valid[0] && !valid[1]) return Result::STORAGE_CORRUPT;
    if (valid[0] && valid[1] && slots[0].generation == slots[1].generation &&
        slots[0].sequence != slots[1].sequence) {
      return Result::STORAGE_CORRUPT;
    }
    if (valid[0] && valid[1] && slots[0].generation != slots[1].generation) {
      const SequenceSlot& newer =
        slots[0].generation > slots[1].generation ? slots[0] : slots[1];
      const SequenceSlot& older =
        slots[0].generation > slots[1].generation ? slots[1] : slots[0];
      if (newer.sequence <= older.sequence) return Result::STORAGE_CORRUPT;
    }
    _activeSlot = valid[1] && (!valid[0] || slots[1].generation > slots[0].generation)
      ? 1U : 0U;
    const SequenceSlot& selected = slots[_activeSlot];
    _generation = selected.generation;
    _sequence = selected.sequence;
    _ready = true;
    return Result::OK;
  }

  Result initialize(uint64_t initialSequence = 0) {
    const Result loaded = load();
    if (loaded == Result::OK) return Result::OK;
    if (loaded != Result::STORAGE_UNINITIALIZED) return loaded;
    return commit(0, SequenceSlot{1, initialSequence});
  }

  Result advance(uint64_t nextSequence) {
    if (!_ready) return Result::STORAGE_UNINITIALIZED;
    if (nextSequence <= _sequence) return Result::INCOMPATIBLE_SEQUENCE;
    if (_generation == UINT64_MAX) return Result::SEQUENCE_EXHAUSTED;
    return commit(static_cast<uint8_t>(_activeSlot ^ 1U),
                  SequenceSlot{_generation + 1U, nextSequence});
  }

  bool ready() const { return _ready; }
  uint64_t sequence() const { return _ready ? _sequence : 0; }
  uint64_t generation() const { return _ready ? _generation : 0; }

private:
  SequenceStorage _storage{};
  uint64_t _sequence = 0;
  uint64_t _generation = 0;
  uint8_t _activeSlot = 0;
  bool _ready = false;

  void lock() {
    _sequence = 0;
    _generation = 0;
    _activeSlot = 0;
    _ready = false;
  }

  Result commit(uint8_t slotIndex, const SequenceSlot& candidate) {
    uint8_t encoded[kSequenceSlotBytes] = {};
    encodeSequenceSlot(candidate, encoded);
    const bool reported = _storage.write(
      _storage.context, slotIndex, encoded, sizeof(encoded));
    uint8_t reconciled[kSequenceSlotBytes] = {};
    bool present = false;
    if (!_storage.read(_storage.context, slotIndex, reconciled,
                       sizeof(reconciled), present)) {
      lock();
      return Result::STORAGE_FAILURE;
    }
    SequenceSlot decoded{};
    if (!present || !decodeSequenceSlot(reconciled, decoded) ||
        decoded.generation != candidate.generation ||
        decoded.sequence != candidate.sequence) {
      (void)reported;
      lock();
      return Result::STORAGE_FAILURE;
    }
    _activeSlot = slotIndex;
    _generation = decoded.generation;
    _sequence = decoded.sequence;
    _ready = true;
    return Result::OK;
  }
};

enum class BootState : uint8_t {
  IDLE = 0,
  PENDING_HEALTH,
  CONFIRMED,
  ROLLBACK_REQUIRED,
};

struct HealthSnapshot {
  bool storageReady = false;
  bool transportReady = false;
  bool webSecurityReady = false;
};

using ConfirmBootFn = bool (*)(void* context);

class PendingBootPolicy {
public:
  explicit PendingBootPolicy(MonotonicSequenceStore* sequenceStore)
    : _sequenceStore(sequenceStore) {}

  Result beginPending(const AuthorizedManifest& authorized) {
    const uint64_t candidateSequence = authorized._manifest.sequence;
    if (_state != BootState::IDLE || _sequenceStore == nullptr ||
        !authorized._valid || !_sequenceStore->ready() ||
        authorized._authorizedAgainstSequence != _sequenceStore->sequence() ||
        candidateSequence <= _sequenceStore->sequence()) {
      return Result::INCOMPATIBLE_SEQUENCE;
    }
    _candidateSequence = candidateSequence;
    _state = BootState::PENDING_HEALTH;
    return Result::OK;
  }

  Result evaluate(const HealthSnapshot& health, void* confirmContext,
                  ConfirmBootFn confirmBoot) {
    if (_state != BootState::PENDING_HEALTH) return Result::STREAM_STATE;
    if (!health.storageReady || !health.transportReady ||
        !health.webSecurityReady) {
      _state = BootState::ROLLBACK_REQUIRED;
      return Result::HEALTH_FAILED;
    }
    const Result stored = _sequenceStore->advance(_candidateSequence);
    if (stored != Result::OK) {
      _state = BootState::ROLLBACK_REQUIRED;
      return stored;
    }
    if (confirmBoot == nullptr || !confirmBoot(confirmContext)) {
      _state = BootState::ROLLBACK_REQUIRED;
      return Result::BOOT_CONFIRM_FAILED;
    }
    _state = BootState::CONFIRMED;
    return Result::OK;
  }

  BootState state() const { return _state; }
  uint64_t candidateSequence() const { return _candidateSequence; }

private:
  MonotonicSequenceStore* _sequenceStore = nullptr;
  uint64_t _candidateSequence = 0;
  BootState _state = BootState::IDLE;
};

}  // namespace bp_update

#endif
