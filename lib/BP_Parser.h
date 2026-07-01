#ifndef BP_Parser_h
#define BP_Parser_h

#include <Arduino.h>

// 血壓數據結構
struct BPData {
  String timestamp;
  int systolic = -1;   // 收縮壓
  int diastolic = -1;  // 舒張壓
  int pulse = -1;      // 脈搏
  String rawData;      // 原始數據
  bool valid = false;  // 數據是否有效
};

class BP_Parser {
private:
  String _model;

public:
  // const ref 收參數避免 caller→param 多一次 String 複製
  explicit BP_Parser(const String& model) : _model(model) {}

  void setModel(const String& model) {
    _model = model;
  }

  const String& getModel() const {
    return _model;
  }

  // line-delimited framing 只適用 ASCII/CSV 型號；
  // binary 型號的 frame 內容可能剛好含 0x0A，不能用換行當 frame 邊界
  bool isLineDelimited() const {
    return !(_model == "OMRON-HBP1300" ||
             _model == "OMRON-HEM7121" ||
             _model == "TERUMO-ES-P2020");
  }

  BPData parse(const uint8_t* buffer, int length) const {
    BPData result;
    if (_model == "OMRON-HBP9030") {
      result = parseOmronHBP9030(buffer, length);
    } else if (_model == "OMRON-HBP1300") {
      result = parseOmronHBP1300(buffer, length);
    } else if (_model == "OMRON-HEM7121") {
      result = parseOmronHEM7121(buffer, length);
    } else if (_model == "TERUMO-ES-P2020") {
      result = parseTerumoESP2020(buffer, length);
    } else {
      result = parseGeneric(buffer, length);
    }
    // rawData 由呼叫端負責填入（DataProcessor 已產生 HTML 包裝的 hex/ascii，
    // 在 parse() 重建一次再被覆寫是浪費）。
    result.valid = (result.systolic > 0 && result.diastolic > 0 && result.pulse > 0);
    return result;
  }

private:
  // OMRON HBP-9030 解析邏輯：CSV 格式，第 7/8/9 欄為 SYS/DIA/PUL（0-indexed）
  BPData parseOmronHBP9030(const uint8_t* buffer, int length) const {
    BPData result;

    // 一次 memcpy 取代逐 byte append（避免 log(n) 次 String 重新配置）
    String dataStr;
    dataStr.reserve(length + 1);
    dataStr.concat(reinterpret_cast<const char*>(buffer), length);

    int values[10] = {0};
    int valueIndex = 0;
    int startPos = 0;
    int len = dataStr.length();

    for (int i = 0; i < len; i++) {
      bool isLast = (i == len - 1);
      bool isComma = (dataStr.charAt(i) == ',');
      if (isComma || isLast) {
        int endPos = (isLast && !isComma) ? i + 1 : i;
        String value = dataStr.substring(startPos, endPos);
        value.trim();
        if (valueIndex < 10) {
          values[valueIndex] = value.toInt();
        }
        valueIndex++;
        startPos = i + 1;
      }
    }

    if (valueIndex >= 10) {
      result.systolic = values[7];
      result.diastolic = values[8];
      result.pulse = values[9];
    }
    return result;
  }

  // OMRON HBP-1300 解析邏輯
  BPData parseOmronHBP1300(const uint8_t* buffer, int length) const {
    BPData result;
    if (length >= 10 && buffer[0] == 0x01) {
      result.systolic = buffer[2] * 256 + buffer[3];
      result.diastolic = buffer[4] * 256 + buffer[5];
      result.pulse = buffer[6] * 256 + buffer[7];
    }
    return result;
  }

  // OMRON HEM-7121 解析邏輯（示意實作）
  BPData parseOmronHEM7121(const uint8_t* buffer, int length) const {
    BPData result;
    if (length >= 10) {
      result.systolic = buffer[3];
      result.diastolic = buffer[5];
      result.pulse = buffer[7];
    }
    return result;
  }

  // TERUMO ES-P2020 解析邏輯（示意實作）
  BPData parseTerumoESP2020(const uint8_t* buffer, int length) const {
    BPData result;
    if (length >= 8) {
      result.systolic = buffer[2] * 10 + buffer[3];
      result.diastolic = buffer[4] * 10 + buffer[5];
      result.pulse = buffer[6] * 10 + buffer[7];
    }
    return result;
  }

  // 通用解析邏輯 - 嘗試尋找ASCII格式的數據或其他常見格式
  BPData parseGeneric(const uint8_t* buffer, int length) const {
    BPData result;

    // 過濾出可列印 ASCII；先 reserve length 避免逐字 append 重新配置
    String dataStr;
    dataStr.reserve(length);
    for (int i = 0; i < length; i++) {
      if (buffer[i] >= 32 && buffer[i] <= 126) {
        dataStr += (char)buffer[i];
      }
    }
    
    // 嘗試使用常見格式解析
    // 格式1: "SYS:120,DIA:80,PUL:75"
    int sysPos = dataStr.indexOf("SYS:");
    int diaPos = dataStr.indexOf("DIA:");
    int pulPos = dataStr.indexOf("PUL:");
    
    if (sysPos >= 0 && diaPos >= 0 && pulPos >= 0) {
      String sysStr = dataStr.substring(sysPos + 4, diaPos);
      String diaStr = dataStr.substring(diaPos + 4, pulPos);
      String pulStr = dataStr.substring(pulPos + 4);
      
      // 清理字符串
      sysStr.trim();
      diaStr.trim();
      pulStr.trim();
      
      // 移除非數字字符
      sysStr = sysStr.substring(0, sysStr.indexOf(',') > 0 ? sysStr.indexOf(',') : sysStr.length());
      diaStr = diaStr.substring(0, diaStr.indexOf(',') > 0 ? diaStr.indexOf(',') : diaStr.length());
      pulStr = pulStr.substring(0, pulStr.indexOf(',') > 0 ? pulStr.indexOf(',') : pulStr.length());
      
      result.systolic = sysStr.toInt();
      result.diastolic = diaStr.toInt();
      result.pulse = pulStr.toInt();
      
      return result;
    }
    
    // 格式2: "BP: 120/80, PR: 75"
    int bpPos = dataStr.indexOf("BP:");
    pulPos = dataStr.indexOf("PR:");
    
    if (bpPos >= 0 && pulPos >= 0) {
      String bpStr = dataStr.substring(bpPos + 3, pulPos);
      String pulStr = dataStr.substring(pulPos + 3);
      
      // 清理字符串
      bpStr.trim();
      pulStr.trim();
      
      // 解析血壓 (SYS/DIA)
      int slashPos = bpStr.indexOf('/');
      if (slashPos > 0) {
        String sysStr = bpStr.substring(0, slashPos);
        String diaStr = bpStr.substring(slashPos + 1);
        
        sysStr.trim();
        diaStr.trim();
        
        // 移除非數字字符
        for (unsigned int i = 0; i < sysStr.length(); i++) {
          if (!isdigit(sysStr.charAt(i))) {
            sysStr = sysStr.substring(0, i);
            break;
          }
        }
        
        for (unsigned int i = 0; i < diaStr.length(); i++) {
          if (!isdigit(diaStr.charAt(i))) {
            diaStr = diaStr.substring(0, i);
            break;
          }
        }
        
        for (unsigned int i = 0; i < pulStr.length(); i++) {
          if (!isdigit(pulStr.charAt(i))) {
            pulStr = pulStr.substring(0, i);
            break;
          }
        }
        
        result.systolic = sysStr.toInt();
        result.diastolic = diaStr.toInt();
        result.pulse = pulStr.toInt();
      }
    }
    
    return result;
  }
};

#endif 
