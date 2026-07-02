#ifndef WEB_SECURITY_H
#define WEB_SECURITY_H

#include <Arduino.h>
#include <cctype>

// 純函式（不依賴 WebServer），可在 host 上單元測試。
// 使用端：WebHandler 對破壞性 POST route 做 CSRF same-origin 檢查、
// 對記錄索引參數做嚴格解析。

// 從 "http://host[:port]/path" 取出小寫的 host[:port]；無 scheme 回空字串
inline String __hostPortOf(const String& url) {
  int scheme = url.indexOf("://");
  if (scheme < 0) return String();
  String out;
  for (unsigned int i = (unsigned int)scheme + 3; i < url.length(); i++) {
    char c = url.charAt(i);
    if (c == '/') break;
    out += (char)tolower((unsigned char)c);
  }
  return out;
}

// ":80" 是 http 預設 port，比對時視為省略
inline String __stripDefaultPort(const String& hostPort) {
  int colon = hostPort.indexOf(':');
  if (colon >= 0 && hostPort.substring((unsigned int)colon) == ":80") {
    return hostPort.substring(0, (unsigned int)colon);
  }
  return hostPort;
}

inline bool __urlMatchesHost(const String& url, const String& hostHeader) {
  if (hostHeader.length() == 0) return false; // 無從比對 → 拒絕
  String u = __stripDefaultPort(__hostPortOf(url));
  if (u.length() == 0) return false; // 格式錯誤 → 拒絕
  String h;
  for (unsigned int i = 0; i < hostHeader.length(); i++) {
    h += (char)tolower((unsigned char)hostHeader.charAt(i));
  }
  return u == __stripDefaultPort(h);
}

// CSRF same-origin 檢查：
//   - Origin 存在：必須與 Host 同 host:port；"null" 一律拒絕
//   - Origin 缺席但有 Referer：套同樣檢查
//   - 兩者皆缺（curl 等非瀏覽器工具）：放行 —— 這不是 access control，
//     只擋瀏覽器跨源偽造；LAN 上的直接 POST 需另以 auth 防護
inline bool csrfCheckPasses(const String& origin, const String& referer,
                            const String& hostHeader) {
  if (origin.length() > 0) {
    if (origin == "null") return false;
    return __urlMatchesHost(origin, hostHeader);
  }
  if (referer.length() > 0) {
    return __urlMatchesHost(referer, hostHeader);
  }
  return true;
}

// 管理密碼（PIN）檢查：stored 為空 = 功能未啟用，一律放行（向後相容）；
// 已設定則必須完全相符。plaintext + String 等值比對即可 —— 威脅模型是
// 診所 LAN 上的誤操作/惡作劇，flash 讀出與 timing attack 不在範圍內。
inline bool pinCheckPasses(const String& provided, const String& stored) {
  if (stored.length() == 0) return true;
  return provided == stored;
}

// PIN 格式：4-16 個可見 ASCII 字元（不含空白）
inline bool isValidPin(const String& s) {
  if (s.length() < 4 || s.length() > 16) return false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c < 33 || c > 126) return false;
  }
  return true;
}

// 嚴格解析非負整數索引：非純數字（空、負號、尾隨字母、前導空白）回 -1。
// String::toInt() 會把 "abc" 變 0、"12abc" 變 12，直接用會讓垃圾輸入
// 命中 record 0。長度 >6 視為異常（索引不可能那麼大）。
inline int parseIndexParam(const String& s) {
  if (s.length() == 0 || s.length() > 6) return -1;
  int value = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c < '0' || c > '9') return -1;
    value = value * 10 + (c - '0');
  }
  return value;
}

#endif
