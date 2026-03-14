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
    
    // 讀取數據
    uint8_t buffer[100];
    int byteCount = 0;
    String dataStr = "<div class='data-section'><h3>原始數據 (十六進制):</h3><pre>";
    String asciiStr = "<div class='data-section'><h3>原始數據 (ASCII):</h3><pre>";
    
    while (transport->available() > 0 && byteCount < 100) {
      int incoming = transport->read();
      if (incoming < 0) {
        break;
      }
      buffer[byteCount] = static_cast<uint8_t>(incoming);
      
      // 組格式化的十六進制字串
      if (buffer[byteCount] < 0x10) dataStr += "0";
      dataStr += String(buffer[byteCount], HEX) + " ";
      
      // 添加ASCII顯示（可顯示字符）
      if (buffer[byteCount] >= 32 && buffer[byteCount] <= 126) {
        asciiStr += (char)buffer[byteCount];
      } else {
        asciiStr += ".";
      }
      
      byteCount++;
      if (byteCount % 16 == 0) {
        dataStr += "<br>";
        asciiStr += "<br>";
      }
      delay(2);
    }
    dataStr += "</pre></div>";
    asciiStr += "</pre></div>";
    
    // 組合完整的原始數據顯示
    *lastData = dataStr + asciiStr;
    
    // 解析血壓數據
    BPData parsedData = bpParser->parse(buffer, byteCount);
    
    // 添加原始數據到解析結果
    parsedData.rawData = *lastData;
    
    // 獲取當前時間並格式化為台北時間
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)) {
      char timeStr[64];
      // 格式化為 2025-04-05 15:30:45 格式
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      parsedData.timestamp = String(timeStr);
    } else {
      // 如果無法獲取時間，使用簡單時間戳
      parsedData.timestamp = String("時間未同步");
    }
    
    // 無論數據是否有效，都添加到歷史記錄
    recordManager->addRecord(parsedData);
    
    // 顯示數據在串列監視器
    Serial.print("接收數據: ");
    for(int i=0; i<byteCount; i++) {
      if(buffer[i] < 0x10) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    // 嘗試顯示ASCII數據
    Serial.print("ASCII數據: ");
    for(int i=0; i<byteCount; i++) {
      if(buffer[i] >= 32 && buffer[i] <= 126) {
        Serial.print((char)buffer[i]);
      } else {
        Serial.print(".");
      }
    }
    Serial.println();
    
    // 顯示解析結果
    if (parsedData.valid) {
      Serial.println("解析結果: SYS=" + String(parsedData.systolic) + " DIA=" + String(parsedData.diastolic) + " PUL=" + String(parsedData.pulse));
    } else {
      Serial.println("無法解析為有效的血壓數據，但已儲存原始數據");
    }
    
    Serial.println("數據已準備，可通過網頁查看");
    Serial.println("----------------------------------");
    syncTransportStatus();
    
    return true;
  }

  void checkActivity() {
    transport->poll();
    syncTransportStatus();

    if (*transportActive && (millis() - *lastTransportActivity > 5000)) {
      *transportActive = false;
      Serial.println("資料通道已超過 5 秒沒有新資料: " + *transportStatus);
    }
  }
};

#endif 
