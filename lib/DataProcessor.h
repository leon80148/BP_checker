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
  bool* transportActive;
  unsigned long* lastTransportActivity;
  String* transportName;
  String* transportStatus;
  MonitorTransport* transport;

  String stateLabel(MonitorTransportState state) {
    switch (state) {
      case TRANSPORT_STATE_STARTING:
        return "啟動中";
      case TRANSPORT_STATE_WAITING_DEVICE:
        return "等待裝置";
      case TRANSPORT_STATE_READY:
        return "就緒";
      case TRANSPORT_STATE_RECEIVING:
        return "接收中";
      case TRANSPORT_STATE_UNSUPPORTED:
        return "未就緒";
      case TRANSPORT_STATE_ERROR:
        return "錯誤";
      default:
        return "未知";
    }
  }

  void syncTransportStatus() {
    *transportName = String(transport->name());
    *transportStatus = stateLabel(transport->state()) + " - " + transport->detail();
  }

public:
  DataProcessor(BP_Parser* bpParser, BP_RecordManager* recordManager,
                String* lastData, bool* transportActive, unsigned long* lastTransportActivity,
                String* transportName, String* transportStatus, MonitorTransport* transport) {
    this->bpParser = bpParser;
    this->recordManager = recordManager;
    this->lastData = lastData;
    this->transportActive = transportActive;
    this->lastTransportActivity = lastTransportActivity;
    this->transportName = transportName;
    this->transportStatus = transportStatus;
    this->transport = transport;
  }

  void setup() {
    bool ok = transport->begin();
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

    *lastTransportActivity = millis();
    *transportActive = true;

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
      return false;
    }

    // 解析血壓數據（在組 HTML 之前先做，避免無資料時還浪費字串組裝）
    BPData parsedData = bpParser->parse(buffer, byteCount);

    // 一次組好 HTML，預先 reserve 容量以減少 String 重新配置
    String dataStr;
    String asciiStr;
    dataStr.reserve(byteCount * 4 + 64);
    asciiStr.reserve(byteCount * 2 + 64);
    dataStr = "<div class='data-section'><h3>原始數據 (十六進制):</h3><pre>";
    asciiStr = "<div class='data-section'><h3>原始數據 (ASCII):</h3><pre>";
    for (int i = 0; i < byteCount; i++) {
      uint8_t b = buffer[i];
      if (b < 0x10) dataStr += '0';
      dataStr += String(b, HEX);
      dataStr += ' ';
      asciiStr += (b >= 32 && b <= 126) ? (char)b : '.';
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

    recordManager->addRecord(parsedData);

    // Serial log（除錯用，組單行字串只 print 一次）
    String hexLine;
    String asciiLine;
    hexLine.reserve(byteCount * 3);
    asciiLine.reserve(byteCount);
    for (int i = 0; i < byteCount; i++) {
      uint8_t b = buffer[i];
      if (b < 0x10) hexLine += '0';
      hexLine += String(b, HEX);
      hexLine += ' ';
      asciiLine += (b >= 32 && b <= 126) ? (char)b : '.';
    }
    Serial.println("接收數據: " + hexLine);
    Serial.println("ASCII數據: " + asciiLine);
    if (parsedData.valid) {
      Serial.println("解析結果: SYS=" + String(parsedData.systolic) +
                     " DIA=" + String(parsedData.diastolic) +
                     " PUL=" + String(parsedData.pulse));
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
    if (*transportActive && (millis() - *lastTransportActivity > 5000)) {
      *transportActive = false;
      Serial.println("資料通道已超過 5 秒沒有新資料: " + *transportStatus);
    }
  }
};

#endif 
