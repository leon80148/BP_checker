#ifndef DATA_PROCESSOR_H
#define DATA_PROCESSOR_H

#include <Arduino.h>
#include <time.h>
#include "BP_Parser.h"
#include "BPRecordManager.h"

class DataProcessor {
private:
  BP_Parser* bpParser;
  BP_RecordManager* recordManager;
  String* lastData;
  bool* serial_active;
  unsigned long* lastSerialActivity;
  
  // TTL串口的引腳定義
  int rx_pin;
  int tx_pin;

public:
  DataProcessor(BP_Parser* bpParser, BP_RecordManager* recordManager,
                String* lastData, bool* serial_active, unsigned long* lastSerialActivity,
                int rx_pin, int tx_pin) {
    this->bpParser = bpParser;
    this->recordManager = recordManager;
    this->lastData = lastData;
    this->serial_active = serial_active;
    this->lastSerialActivity = lastSerialActivity;
    this->rx_pin = rx_pin;
    this->tx_pin = tx_pin;
  }

  void setup() {
    // 初始化 TTL 串口用於與血壓機通訊
    Serial1.begin(9600, SERIAL_8N1, rx_pin, tx_pin);
    
    Serial.println("與電腦通訊: 115200 bps");
    Serial.println("與血壓機通訊: 9600 bps (RX:" + String(rx_pin) + ", TX:" + String(tx_pin) + ")");
  }

  bool processIncomingData() {
    if (!Serial1.available()) {
      return false;
    }
    
    *lastSerialActivity = millis();
    *serial_active = true;
    
    // 讀取數據
    uint8_t buffer[100];
    int byteCount = 0;
    String dataStr = "<div class='data-section'><h3>原始數據 (十六進制):</h3><pre>";
    String asciiStr = "<div class='data-section'><h3>原始數據 (ASCII):</h3><pre>";
    
    while (Serial1.available() && byteCount < 100) {
      buffer[byteCount] = Serial1.read();
      
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
    
    return true;
  }

  void checkActivity() {
    // 如果TTL串口通訊長時間無活動，設為非活動狀態
    if (*serial_active && (millis() - *lastSerialActivity > 5000)) {
      *serial_active = false;
      Serial.println("TTL串口未檢測到活動");
    }
  }
};

#endif 