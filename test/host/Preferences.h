// Minimal Preferences (ESP32 NVS) shim for host-side unit tests.
// Values are byte-exact and survive across Preferences instances so tests can
// model reboots, torn workflows, wrong types, and checksum corruption.
#ifndef HOST_PREFERENCES_SHIM_H
#define HOST_PREFERENCES_SHIM_H

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"

typedef enum {
  PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64,
  PT_STR, PT_BLOB, PT_INVALID
} PreferenceType;

class Preferences {
public:
  enum class FailureMode {
    NONE,
    BEFORE_APPLY,
    AFTER_APPLY,
    HARD_CUT_BEFORE_APPLY,
    HARD_CUT_AFTER_APPLY,
  };

  bool begin(const char* name, bool readOnly = false) {
    if (_started || name == nullptr || *name == '\0') return false;
    if (beginFailures() > 0) {
      beginFailures()--;
      return false;
    }
    _ns = name ? name : "";
    _readOnly = readOnly;
    if (readOnly && store().find(_ns) == store().end()) {
      _ns.clear();
      _readOnly = false;
      return false;
    }
    _started = true;
    return true;
  }

  void end() {
    if (!_started) return;
    _ns.clear();
    _readOnly = false;
    _started = false;
  }

  bool isKey(const char* key) const {
    if (!_started || key == nullptr) return false;
    const auto ns = store().find(_ns);
    return ns != store().end() && ns->second.find(key ? key : "") != ns->second.end();
  }

  PreferenceType getType(const char* key) const {
    if (!_started || key == nullptr) return PT_INVALID;
    const Value* value = findValue(key);
    if (value == nullptr) return PT_INVALID;
    if (value->type == ValueType::STRING) return PT_STR;
    if (value->type == ValueType::INT32) return PT_I32;
    return PT_BLOB;
  }

  size_t putString(const char* key, const String& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(value.c_str());
    return putValue(key, bytes, value.length(), ValueType::STRING);
  }

  String getString(const char* key, const String& defaultValue = String()) const {
    const Value* value = findValue(key);
    if (value == nullptr || value->type != ValueType::STRING) return defaultValue;
    return String(std::string(value->bytes.begin(), value->bytes.end()));
  }

  size_t putInt(const char* key, int32_t value) {
    uint8_t encoded[sizeof(value)];
    for (size_t i = 0; i < sizeof(value); ++i) {
      encoded[i] = static_cast<uint8_t>(
        (static_cast<uint32_t>(value) >> (8U * i)) & 0xffU);
    }
    return putValue(key, encoded, sizeof(encoded), ValueType::INT32);
  }

  int32_t getInt(const char* key, int32_t defaultValue = 0) const {
    const Value* value = findValue(key);
    if (value == nullptr || value->type != ValueType::INT32 ||
        value->bytes.size() != sizeof(int32_t)) {
      return defaultValue;
    }
    uint32_t decoded = 0;
    for (size_t i = 0; i < sizeof(int32_t); ++i) {
      decoded |= static_cast<uint32_t>(value->bytes[i]) << (8U * i);
    }
    return static_cast<int32_t>(decoded);
  }

  size_t putBytes(const char* key, const void* value, size_t length) {
    if (value == nullptr && length != 0) return 0;
    return putValue(key, static_cast<const uint8_t*>(value), length,
                    ValueType::BYTES);
  }

  size_t getBytesLength(const char* key) const {
    const Value* value = findValue(key);
    if (value == nullptr || value->type != ValueType::BYTES) return 0;
    return value->bytes.size();
  }

  size_t getBytes(const char* key, void* buffer, size_t maxLength) const {
    const Value* value = findValue(key);
    if (value == nullptr || value->type != ValueType::BYTES ||
        buffer == nullptr || maxLength < value->bytes.size()) {
      return 0;
    }
    if (!value->bytes.empty()) {
      memcpy(buffer, value->bytes.data(), value->bytes.size());
    }
    return value->bytes.size();
  }

  bool remove(const char* key) {
    if (!_started || _readOnly || key == nullptr || !isKey(key)) return false;
    const FailureMode failure = startWrite();
    if (isBeforeFailure(failure)) return false;
    store()[_ns].erase(key ? key : "");
    return !isAfterFailure(failure);
  }

  bool clear() {
    if (!_started || _readOnly) return false;
    const FailureMode failure = startWrite();
    if (isBeforeFailure(failure)) return false;
    store()[_ns].clear();
    return !isAfterFailure(failure);
  }

  static void __reset() {
    store().clear();
    __clearFailure();
  }

  // Configure a one-shot failure at the one-based ordinal write after this
  // call. AFTER_APPLY mutates storage but reports failure to the caller.
  static void __failWrite(size_t ordinal, FailureMode mode) {
    faultOrdinal() = ordinal;
    faultMode() = mode;
    writeCount() = 0;
    hardCutLatched() = false;
  }

  static void __clearFailure() {
    faultOrdinal() = 0;
    faultMode() = FailureMode::NONE;
    writeCount() = 0;
    hardCutLatched() = false;
    beginFailures() = 0;
  }

  static void __failNextBegin() { beginFailures()++; }

  // End a simulated power cut without changing durable bytes.
  static void __simulateReboot() { __clearFailure(); }

  static void __startWriteTrace() {
    faultOrdinal() = 0;
    faultMode() = FailureMode::NONE;
    writeCount() = 0;
    hardCutLatched() = false;
  }

  static size_t __writeCount() { return writeCount(); }

  static void __putRawBytes(const char* nameSpace, const char* key,
                            const std::vector<uint8_t>& bytes) {
    store()[nameSpace ? nameSpace : ""][key ? key : ""] =
      Value{ValueType::BYTES, bytes};
  }

  static std::vector<uint8_t> __getRawBytes(const char* nameSpace,
                                            const char* key) {
    const auto ns = store().find(nameSpace ? nameSpace : "");
    if (ns == store().end()) return {};
    const auto it = ns->second.find(key ? key : "");
    if (it == ns->second.end()) return {};
    return it->second.bytes;
  }

  static void __eraseRaw(const char* nameSpace, const char* key) {
    const auto ns = store().find(nameSpace ? nameSpace : "");
    if (ns != store().end()) ns->second.erase(key ? key : "");
  }

  static bool __hasKey(const char* nameSpace, const char* key) {
    const auto ns = store().find(nameSpace ? nameSpace : "");
    return ns != store().end() &&
           ns->second.find(key ? key : "") != ns->second.end();
  }

  static size_t __longestKeyLength() {
    size_t longest = 0;
    for (const auto& nameSpace : store()) {
      for (const auto& item : nameSpace.second) {
        if (item.first.size() > longest) longest = item.first.size();
      }
    }
    return longest;
  }

  static bool __containsSubstring(const char* needle) {
    if (needle == nullptr) return false;
    const std::string target(needle);
    if (target.empty()) return false;
    for (const auto& nameSpace : store()) {
      for (const auto& item : nameSpace.second) {
        const std::string bytes(item.second.bytes.begin(), item.second.bytes.end());
        if (bytes.find(target) != std::string::npos) return true;
      }
    }
    return false;
  }

private:
  enum class ValueType { STRING, INT32, BYTES };
  struct Value {
    ValueType type;
    std::vector<uint8_t> bytes;
  };
  using KV = std::map<std::string, Value>;

  size_t putValue(const char* key, const uint8_t* bytes, size_t length,
                  ValueType type) {
    if (!_started || _readOnly || key == nullptr || *key == '\0' ||
        strlen(key) > 15 || (bytes == nullptr && length != 0)) return 0;
    const FailureMode failure = startWrite();
    if (isBeforeFailure(failure)) return 0;
    std::vector<uint8_t> copy;
    if (length != 0) copy.assign(bytes, bytes + length);
    store()[_ns][key ? key : ""] = Value{type, std::move(copy)};
    return isAfterFailure(failure) ? 0 : length;
  }

  const Value* findValue(const char* key) const {
    if (!_started || key == nullptr) return nullptr;
    const auto ns = store().find(_ns);
    if (ns == store().end()) return nullptr;
    const auto it = ns->second.find(key ? key : "");
    return it == ns->second.end() ? nullptr : &it->second;
  }

  static FailureMode startWrite() {
    writeCount()++;
    if (hardCutLatched()) return FailureMode::HARD_CUT_BEFORE_APPLY;
    if (faultOrdinal() != 0 && writeCount() == faultOrdinal()) {
      const FailureMode result = faultMode();
      faultOrdinal() = 0;
      faultMode() = FailureMode::NONE;
      if (result == FailureMode::HARD_CUT_BEFORE_APPLY ||
          result == FailureMode::HARD_CUT_AFTER_APPLY) {
        hardCutLatched() = true;
      }
      return result;
    }
    return FailureMode::NONE;
  }

  static bool isBeforeFailure(FailureMode mode) {
    return mode == FailureMode::BEFORE_APPLY ||
           mode == FailureMode::HARD_CUT_BEFORE_APPLY;
  }

  static bool isAfterFailure(FailureMode mode) {
    return mode == FailureMode::AFTER_APPLY ||
           mode == FailureMode::HARD_CUT_AFTER_APPLY;
  }

  static std::map<std::string, KV>& store() {
    static std::map<std::string, KV> s;
    return s;
  }
  static size_t& writeCount() {
    static size_t value = 0;
    return value;
  }
  static size_t& faultOrdinal() {
    static size_t value = 0;
    return value;
  }
  static FailureMode& faultMode() {
    static FailureMode value = FailureMode::NONE;
    return value;
  }
  static bool& hardCutLatched() {
    static bool value = false;
    return value;
  }
  static size_t& beginFailures() {
    static size_t value = 0;
    return value;
  }

  std::string _ns;
  bool _readOnly = false;
  bool _started = false;
};

#endif
