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
                String* transportName, String* transportStatus, MonitorTransport* transport) {
    this->bpParser = bpParser;
    this->recordManager = recordManager;
    this->lastData = lastData;
    this->transportName = transportName;
    this->transportStatus = transportStatus;
    this->transport = transport;
  }

  void setup() {
    bool ok = transport->begin();
    *transportName = String(transport->name()); // const literal，僅在 setup 寫一次
    syncTransportStatus();
    Serial.println("與電腦通訊: 115200 bps");
    Serial.println("血壓機資料通道: " + *transportName);
    Serial.println("資料通道狀態: " + *transportStatus);
    if (!ok) {
      Serial.println("注意: 目前資料通道尚未可用，系統仍會保持 WiFi 與網頁服務可用。");
    }
  }

  bool processIncomingData() {
    transport->poll();
    syncTransportStatus();

    if (transport->available() <= 0) {
      return false;
    }

    // 用 inter-byte timeout 模式讀取，兼顧 UART（bytes 1ms 間隔）與 USB CDC（爆量到達）。
    // 比起原本每 byte 都 delay(2)，最多只在 byte 間 idle 等候 30ms，避免單次接收阻塞 webserver 200ms。
    static constexpr int kBufferSize = 100;
    static constexpr unsigned long kInterByteTimeoutMs = 30;
    uint8_t buffer[kBufferSize];
    int byteCount = 0;
    unsigned long lastByteMs = millis();
    while (byteCount < kBufferSize) {
      if (transport->available() > 0) {
        int incoming = transport->read();
        if (incoming < 0) break;
        buffer[byteCount++] = static_cast<uint8_t>(incoming);
        lastByteMs = millis();
        continue;
      }
      if (millis() - lastByteMs > kInterByteTimeoutMs) break;
      delay(2);
    }

    if (byteCount == 0) {
      return false; // 不更新 transportActive：實際沒讀到資料
    }

    // 確認真的讀到 byte 後才標記活動
    lastTransportActivity = millis();
    transportActive = true;

    // 解析血壓數據（在組 HTML 之前先做，避免無資料時還浪費字串組裝）
    BPData parsedData = bpParser->parse(buffer, byteCount);

    // 用 hex lookup table 避免每 byte 建一次 String(uint8_t, HEX) 臨時物件
    static const char kHex[] = "0123456789abcdef";
    String dataStr;
    String asciiStr;
    // ASCII 區塊保留一點額外空間給 HTML escape（< → &lt; 等放大 4 倍）
    dataStr.reserve(byteCount * 4 + 64);
    asciiStr.reserve(byteCount * 4 + 64);
    dataStr = "<div class='data-section'><h3>原始數據 (十六進制):</h3><pre>";
    asciiStr = "<div class='data-section'><h3>原始數據 (ASCII):</h3><pre>";
    for (int i = 0; i < byteCount; i++) {
      uint8_t b = buffer[i];
      dataStr += kHex[b >> 4];
      dataStr += kHex[b & 0x0F];
      dataStr += ' ';
      // BP 機若回傳 '<' 等字元會被瀏覽器當 HTML 解析；做最小 escape
      if (b < 32 || b > 126) {
        asciiStr += '.';
      } else if (b == '<') {
        asciiStr += "&lt;";
      } else if (b == '>') {
        asciiStr += "&gt;";
      } else if (b == '&') {
        asciiStr += "&amp;";
      } else {
        asciiStr += (char)b;
      }
      if ((i + 1) % 16 == 0) {
        dataStr += "<br>";
        asciiStr += "<br>";
      }
    }
    dataStr += "</pre></div>";
    asciiStr += "</pre></div>";
    *lastData = dataStr + asciiStr;
    parsedData.rawData = *lastData;

    // 取得台北時區時間
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      parsedData.timestamp = String(timeStr);
    } else {
      parsedData.timestamp = String("時間未同步");
    }

    // 移轉 parsedData 進 ring buffer 省一次 ~700B rawData copy；
    // 我們之後只讀 systolic/diastolic/pulse/valid 這些 trivial 欄位（move 後仍有效）
    int sys = parsedData.systolic;
    int dia = parsedData.diastolic;
    int pul = parsedData.pulse;
    bool ok = parsedData.valid;
    recordManager->addRecord(std::move(parsedData));

    // Serial log 直接 print（內建 buffered IO），避免再配置兩個 String
    Serial.print("接收數據: ");
    for (int i = 0; i < byteCount; i++) {
      uint8_t b = buffer[i];
      Serial.print(kHex[b >> 4]);
      Serial.print(kHex[b & 0x0F]);
      Serial.print(' ');
    }
    Serial.println();
    Serial.print("ASCII數據: ");
    for (int i = 0; i < byteCount; i++) {
      uint8_t b = buffer[i];
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
    } else {
      Serial.println("無法解析為有效的血壓數據，但已儲存原始數據");
    }
    Serial.println("數據已準備，可通過網頁查看");
    Serial.println("----------------------------------");
    syncTransportStatus();

    return true;
  }

  // processIncomingData 會在 loop 開頭就呼叫 poll/syncTransportStatus，
  // 所以這裡只負責偵測 idle 狀態，不再重複 poll。
  void checkActivity() {
    if (transportActive && (millis() - lastTransportActivity > 5000)) {
      transportActive = false;
      Serial.println("資料通道已超過 5 秒沒有新資料: " + *transportStatus);
    }
  }
};

#endif 
