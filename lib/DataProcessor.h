#ifndef DATA_PROCESSOR_H
#define DATA_PROCESSOR_H

#include <Arduino.h>
#include <time.h>
#include "BP_Parser.h"
#include "BPRecordManager.h"
#include "transports/MonitorTransport.h"

class DataProcessor {
private:
  BP_Parser* bpParser;
  BP_RecordManager* recordManager;
  String* lastData;
  String* transportName;
  String* transportStatus;
  MonitorTransport* transport;

  // 內部 inactivity 追蹤（先前是 bp_checker.ino 全域，但只有此類用到）
  bool transportActive = false;
  unsigned long lastTransportActivity = 0;

  // frame assembly：跨 loop() 累積 bytes，line-based 型號以 '\n' 為邊界，
  // 其餘靠 idle timeout flush。取代舊的「單次 poll 內 blocking 讀完一個 burst」
  // 模式 —— 不再有 delay(2) 等待迴圈阻塞 webserver，分段到達的 frame 也不會
  // 被切成多筆垃圾記錄。
  static constexpr size_t kFrameBufferSize = 256;
  static constexpr unsigned long kFrameFlushTimeoutMs = 30;
  uint8_t frameBuf[kFrameBufferSize];
  size_t frameLen = 0;
  bool frameOverflowed = false;
  unsigned long lastByteMs = 0;

  // 上次同步的狀態快取，避免每 loop iteration 都重建 transportStatus String
  MonitorTransportState lastSyncedState = TRANSPORT_STATE_STARTING;
  String lastSyncedDetail;
  bool statusEverSynced = false;

  const char* stateLabel(MonitorTransportState state) {
    switch (state) {
      case TRANSPORT_STATE_STARTING:      return "啟動中";
      case TRANSPORT_STATE_WAITING_DEVICE:return "等待裝置";
      case TRANSPORT_STATE_READY:         return "就緒";
      case TRANSPORT_STATE_RECEIVING:     return "接收中";
      case TRANSPORT_STATE_UNSUPPORTED:   return "未就緒";
      case TRANSPORT_STATE_ERROR:         return "錯誤";
      default:                            return "未知";
    }
  }

  void syncTransportStatus() {
    MonitorTransportState s = transport->state();
    String d = transport->detail();
    if (statusEverSynced && s == lastSyncedState && d == lastSyncedDetail) {
      return; // 沒變化就跳過全部 String 重組
    }
    lastSyncedState = s;
    lastSyncedDetail = d;
    statusEverSynced = true;
    // transport->name() 是 const literal，setup() 已寫入 *transportName 一次，不再重複配置
    // 直接 += 串接避免 `String(label) + " - " + d` 產生的 2-3 個暫物件
    String& target = *transportStatus;
    target = stateLabel(s);
    target += " - ";
    target += d;
  }

public:
  DataProcessor(BP_Parser* bpParser, BP_RecordManager* recordManager,
                String* lastData,
                String* transportName, String* transportStatus, MonitorTransport* transport)
    : bpParser(bpParser),
      recordManager(recordManager),
      lastData(lastData),
      transportName(transportName),
      transportStatus(transportStatus),
      transport(transport) {}

  void setup() {
    bool ok = transport->begin();
    *transportName = transport->name(); // const literal，僅在 setup 寫一次
    syncTransportStatus();
    Serial.println("與電腦通訊: 115200 bps");
    Serial.print("血壓機資料通道: ");
    Serial.println(*transportName);
    Serial.print("資料通道狀態: ");
    Serial.println(*transportStatus);
    if (!ok) {
      Serial.println("注意: 目前資料通道尚未可用，系統仍會保持 WiFi 與網頁服務可用。");
    }
  }

  bool processIncomingData() {
    transport->poll();
    syncTransportStatus();

    bool produced = false;

    // 非阻塞：把目前可讀的 bytes 全部收進 frame buffer，line-based 型號
    // 遇 '\n' 立即完成一個 frame；其餘 bytes 留在 buffer 等下輪 loop()。
    while (transport->available() > 0) {
      int incoming = transport->read();
      if (incoming < 0) break;
      lastByteMs = millis();
      lastTransportActivity = millis();
      transportActive = true;

      uint8_t b = static_cast<uint8_t>(incoming);
      if (b == '\n' && bpParser->isLineDelimited()) {
        if (finishFrame()) produced = true;
        continue;
      }
      if (frameLen >= kFrameBufferSize) {
        frameOverflowed = true; // 丟棄超出的 bytes，flush 時標記診斷
        continue;
      }
      frameBuf[frameLen++] = b;
    }

    // idle timeout flush：binary 型號的唯一 frame 邊界；
    // line-based 型號漏送換行時的保底
    if (frameLen > 0 && millis() - lastByteMs > kFrameFlushTimeoutMs) {
      if (finishFrame()) produced = true;
    }

    if (produced) syncTransportStatus();
    return produced;
  }

private:
  // 完成一個 frame：解析、更新 lastData 診斷、只在 parse 成功時持久化。
  // invalid frame（雜訊/分段殘渣）只留 RAM 診斷，不寫 ring buffer/NVS ——
  // 避免 flash 損耗與歷史記錄被 "—" 列污染。
  bool finishFrame() {
    size_t len = frameLen;
    bool overflowed = frameOverflowed;
    frameLen = 0;
    frameOverflowed = false;

    if (len > 0 && frameBuf[len - 1] == '\r') len--; // CRLF 結尾剝除
    if (len == 0 && !overflowed) return false;       // 空行（bare CRLF）直接略過

    // 解析血壓數據（在組 HTML 之前先做，避免無資料時還浪費字串組裝）
    BPData parsedData = bpParser->parse(frameBuf, static_cast<int>(len));

    // 用 hex lookup table 避免每 byte 建一次 String(uint8_t, HEX) 臨時物件
    static const char kHex[] = "0123456789abcdef";
    // 直接寫入 *lastData（取代 dataStr + asciiStr 兩個中介 String + 一次 concat）
    String& target = *lastData;
    target = "";
    target.reserve(len * 8 + 192);
    target += "<div class='data-section'><h3>原始數據 (十六進制):</h3><pre>";
    for (size_t i = 0; i < len; i++) {
      uint8_t b = frameBuf[i];
      target += kHex[b >> 4];
      target += kHex[b & 0x0F];
      target += ' ';
      if ((i + 1) % 16 == 0) target += "<br>";
    }
    target += "</pre></div>";
    target += "<div class='data-section'><h3>原始數據 (ASCII):</h3><pre>";
    for (size_t i = 0; i < len; i++) {
      uint8_t b = frameBuf[i];
      // BP 機若回傳 '<' 等字元會被瀏覽器當 HTML 解析；做最小 escape
      if (b < 32 || b > 126)      target += '.';
      else if (b == '<')          target += "&lt;";
      else if (b == '>')          target += "&gt;";
      else if (b == '&')          target += "&amp;";
      else                        target += (char)b;
      if ((i + 1) % 16 == 0) target += "<br>";
    }
    target += "</pre></div>";
    if (overflowed) {
      target += "<p class='helper-text'>（frame 超過 ";
      target += static_cast<int>(kFrameBufferSize);
      target += " bytes 已截斷，此筆不儲存）</p>";
    }

    // 取得台北時區時間
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      parsedData.timestamp = timeStr; // const char* assign，免 String temp
    } else {
      parsedData.timestamp = "時間未同步";
    }

    int sys = parsedData.systolic;
    int dia = parsedData.diastolic;
    int pul = parsedData.pulse;
    bool ok = parsedData.valid && !overflowed;
    if (ok) {
      // 移轉 parsedData 進 ring buffer 省一次 ~700B rawData copy；
      // 之後只讀 trivial 欄位（move 後仍有效）
      parsedData.rawData = target;
      recordManager->addRecord(std::move(parsedData));
    }

    // Serial log 直接 print（內建 buffered IO），避免再配置兩個 String
    Serial.print("接收數據: ");
    for (size_t i = 0; i < len; i++) {
      uint8_t b = frameBuf[i];
      Serial.print(kHex[b >> 4]);
      Serial.print(kHex[b & 0x0F]);
      Serial.print(' ');
    }
    Serial.println();
    Serial.print("ASCII數據: ");
    for (size_t i = 0; i < len; i++) {
      uint8_t b = frameBuf[i];
      Serial.print((b >= 32 && b <= 126) ? (char)b : '.');
    }
    Serial.println();
    if (ok) {
      // 直接 print 避免建 5 個 String 暫物件
      Serial.print("解析結果: SYS=");
      Serial.print(sys);
      Serial.print(" DIA=");
      Serial.print(dia);
      Serial.print(" PUL=");
      Serial.println(pul);
    } else if (overflowed) {
      Serial.println("frame 超過緩衝上限已截斷，不儲存此筆");
    } else {
      Serial.println("無法解析為有效的血壓數據，僅保留原始診斷（不寫入歷史）");
    }
    Serial.println("----------------------------------");
    return true;
  }

public:

  // processIncomingData 會在 loop 開頭就呼叫 poll/syncTransportStatus，
  // 所以這裡只負責偵測 idle 狀態，不再重複 poll。
  void checkActivity() {
    if (transportActive && (millis() - lastTransportActivity > 5000)) {
      transportActive = false;
      Serial.print("資料通道已超過 5 秒沒有新資料: ");
      Serial.println(*transportStatus);
    }
  }
};

#endif 
