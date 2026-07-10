// Minimal Arduino shim for host-side unit tests.
// 只模擬被測 lib 實際用到的 String 介面；語意對齊 ESP32 Arduino core：
//   - toInt(): atol 語意（跳過前導空白、遇非數字停止、無效輸入回 0）
//   - substring(a, b): a > b 時互換，邊界 clamp 到字串長度
//   - charAt(): 越界回 '\0'
//   - indexOf(): 找不到回 -1
// 刻意不模擬記憶體配置失敗與 move 內部行為——host 測試不涵蓋這些。
#ifndef HOST_ARDUINO_SHIM_H
#define HOST_ARDUINO_SHIM_H

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// ---- 可注入的 String allocation failure ----
// -1 表示正常；0 表示下一個 reserve/concat 失敗；正數會在每次配置操作遞減。
// 只模擬 production 會檢查回傳值的操作，讓低記憶體 fail-closed 路徑可測。
inline int& __stringAllocationFailureCountdown() {
  static int v = -1;
  return v;
}
inline bool __stringAllocationAllowed() {
  int& countdown = __stringAllocationFailureCountdown();
  if (countdown < 0) return true;
  if (countdown == 0) {
    countdown = -1;
    return false;
  }
  countdown--;
  return true;
}

// ---- fake clock：millis()/delay() 不真的睡，測試可用 delay() 快轉 ----
inline unsigned long& __millisCounter() {
  static unsigned long v = 0;
  return v;
}
inline unsigned long& __delayCallCount() {
  static unsigned long v = 0;
  return v;
}
inline unsigned long millis() { return __millisCounter(); }
inline void delay(unsigned long ms) {
  __delayCallCount()++;
  __millisCounter() += ms;
}

// ---- 可控 getLocalTime（ESP32 core 函式）----
inline bool& __fakeTimeValid() {
  static bool v = false;
  return v;
}
inline struct tm& __fakeTm() {
  static struct tm t = {};
  return t;
}
inline unsigned long& __getLocalTimeCallCount() {
  static unsigned long v = 0;
  return v;
}
inline bool getLocalTime(struct tm* info) {
  __getLocalTimeCallCount()++;
  if (!__fakeTimeValid()) return false;
  *info = __fakeTm();
  return true;
}

// ---- Serial stub：保留輸出供隱私測試檢查 ----
inline std::string& __serialOutput() {
  static std::string value;
  return value;
}
class HostSerial {
public:
  void begin(unsigned long) {}
  void print(char value) { __serialOutput().push_back(value); }
  void print(const char* value) {
    if (value) __serialOutput().append(value);
  }
  void print(int value) { __serialOutput().append(std::to_string(value)); }
  void print(unsigned int value) { __serialOutput().append(std::to_string(value)); }
  void print(long value) { __serialOutput().append(std::to_string(value)); }
  void print(unsigned long value) { __serialOutput().append(std::to_string(value)); }
  template <typename T> void print(const T&) {}
  template <typename T> void println(const T& value) {
    print(value);
    __serialOutput().push_back('\n');
  }
  void println() { __serialOutput().push_back('\n'); }
};
inline HostSerial Serial;

class String {
public:
  String() = default;
  String(const char* s) : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}

  unsigned int length() const { return (unsigned int)_s.size(); }
  bool isEmpty() const { return _s.empty(); }
  const char* c_str() const { return _s.c_str(); }

  bool reserve(unsigned int n) {
    if (!__stringAllocationAllowed()) return false;
    _s.reserve(n);
    return _s.capacity() >= n;
  }

  bool concat(const char* p, unsigned int len) {
    if (!__stringAllocationAllowed()) return false;
    _s.append(p, len);
    return true;
  }

  char charAt(unsigned int i) const { return i < _s.size() ? _s[i] : '\0'; }

  String substring(unsigned int from) const {
    return substring(from, (unsigned int)_s.size());
  }

  String substring(unsigned int from, unsigned int to) const {
    if (from > to) { unsigned int t = from; from = to; to = t; }
    if (from >= _s.size()) return String();
    if (to > _s.size()) to = (unsigned int)_s.size();
    return String(_s.substr(from, to - from));
  }

  void trim() {
    size_t b = 0, e = _s.size();
    while (b < e && isspace((unsigned char)_s[b])) b++;
    while (e > b && isspace((unsigned char)_s[e - 1])) e--;
    _s = _s.substr(b, e - b);
  }

  long toInt() const { return atol(_s.c_str()); }

  int indexOf(char c) const { return indexOf(c, 0); }
  int indexOf(char c, unsigned int from) const {
    size_t p = _s.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const char* sub) const {
    size_t p = _s.find(sub);
    return p == std::string::npos ? -1 : (int)p;
  }

  String& operator+=(char c) { _s.push_back(c); return *this; }
  String& operator+=(const char* p) { _s.append(p); return *this; }
  String& operator+=(const String& o) { _s.append(o._s); return *this; }
  String& operator+=(int v) { _s.append(std::to_string(v)); return *this; }

  bool operator==(const char* p) const { return _s == p; }
  bool operator==(const String& o) const { return _s == o._s; }
  bool operator!=(const char* p) const { return _s != p; }

private:
  std::string _s;
};

#endif
