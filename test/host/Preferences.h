// Minimal Preferences (ESP32 NVS) shim for host-side unit tests.
// 以 process 內的 static map 模擬 NVS：跨實例、跨 begin/end 存活，
// 讓「重啟後 loadFromStorage」可以用新物件實例模擬。
// __reset() 是 test-only：清空整個 fake NVS，做測試隔離。
#ifndef HOST_PREFERENCES_SHIM_H
#define HOST_PREFERENCES_SHIM_H

#include <map>
#include <string>

#include "Arduino.h"

class Preferences {
public:
  bool begin(const char* name, bool /*readOnly*/ = false) {
    _ns = name;
    return true;
  }

  void end() { _ns.clear(); }

  size_t putString(const char* key, const String& value) {
    store()[_ns][key] = value.c_str();
    return value.length();
  }

  String getString(const char* key, const String& defaultValue = String()) {
    auto ns = store().find(_ns);
    if (ns == store().end()) return defaultValue;
    auto it = ns->second.find(key);
    if (it == ns->second.end()) return defaultValue;
    return String(it->second.c_str());
  }

  size_t putInt(const char* key, int32_t value) {
    store()[_ns][key] = std::to_string(value);
    return sizeof(value);
  }

  int32_t getInt(const char* key, int32_t defaultValue = 0) {
    auto ns = store().find(_ns);
    if (ns == store().end()) return defaultValue;
    auto it = ns->second.find(key);
    if (it == ns->second.end()) return defaultValue;
    return (int32_t)atol(it->second.c_str());
  }

  bool remove(const char* key) {
    auto ns = store().find(_ns);
    if (ns == store().end()) return false;
    return ns->second.erase(key) > 0;
  }

  bool clear() {
    store()[_ns].clear();
    return true;
  }

  static void __reset() { store().clear(); }

  static bool __containsSubstring(const char* needle) {
    if (needle == nullptr) return false;
    for (const auto& nameSpace : store()) {
      for (const auto& item : nameSpace.second) {
        if (item.second.find(needle) != std::string::npos) return true;
      }
    }
    return false;
  }

private:
  using KV = std::map<std::string, std::string>;
  static std::map<std::string, KV>& store() {
    static std::map<std::string, KV> s;
    return s;
  }
  std::string _ns;
};

#endif
