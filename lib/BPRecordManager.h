#ifndef BP_RecordManager_h
#define BP_RecordManager_h

#include <Arduino.h>
#include <Preferences.h>
#include "BP_Parser.h"

class BP_RecordManager {
private:
  const int _maxRecords;
  BPData* _records;
  int _historyIndex;
  int _recordCount;
  Preferences _preferences;
  
  // 非易失性存儲的命名空間
  String _namespace = "bp_records";
  
public:
  BP_RecordManager(int maxRecords = 10) : _maxRecords(maxRecords) {
    _records = new BPData[_maxRecords];
    _historyIndex = 0;
    _recordCount = 0;
  }
  
  ~BP_RecordManager() {
    delete[] _records;
  }
  
  // 添加新的血壓記錄
  void addRecord(BPData record) {
    // 添加到環形緩衝區
    _records[_historyIndex] = record;
    _historyIndex = (_historyIndex + 1) % _maxRecords;
    if (_recordCount < _maxRecords) _recordCount++;
    
    // 保存到非易失性存儲
    saveToStorage();
  }
  
  // 獲取某個指定位置的記錄
  BPData getRecord(int index) {
    if (index < 0 || index >= _recordCount) {
      // 返回無效數據
      BPData emptyData;
      emptyData.systolic = -1;
      emptyData.diastolic = -1;
      emptyData.pulse = -1;
      emptyData.valid = false;
      return emptyData;
    }
    
    int actualIndex = (_historyIndex - index - 1 + _maxRecords) % _maxRecords;
    return _records[actualIndex];
  }
  
  // 獲取最新的記錄
  BPData getLatestRecord() {
    if (_recordCount == 0) {
      // 返回無效數據
      BPData emptyData;
      emptyData.systolic = -1;
      emptyData.diastolic = -1;
      emptyData.pulse = -1;
      emptyData.valid = false;
      return emptyData;
    }
    
    return _records[(_historyIndex - 1 + _maxRecords) % _maxRecords];
  }
  
  // 獲取記錄數量
  int getRecordCount() {
    return _recordCount;
  }
  
  // 獲取最大記錄數量
  int getMaxRecords() {
    return _maxRecords;
  }
  
  // 清除所有記錄
  void clearRecords() {
    _historyIndex = 0;
    _recordCount = 0;
    
    // 清除存儲
    _preferences.begin(_namespace.c_str(), false);
    _preferences.clear();
    _preferences.end();
  }
  
  // 從非易失性存儲加載記錄
  // 儲存格式（newest-first）：rec_0 = 最新，rec_(count-1) = 最舊
  // 載入時將 rec_i 放到 _records[count-1-i]，配合 _historyIndex = count % max，
  // 使 circular buffer 中 (_historyIndex - 1 + max) % max 永遠指向最新筆。
  void loadFromStorage() {
    _preferences.begin(_namespace.c_str(), true); // 只讀模式

    _recordCount = _preferences.getInt("count", 0);
    if (_recordCount > _maxRecords) _recordCount = _maxRecords;
    if (_recordCount < 0) _recordCount = 0;

    for (int i = 0; i < _recordCount; i++) {
      String key = "rec_" + String(i);
      String recData = _preferences.getString(key.c_str(), "");

      if (recData.length() == 0) continue;

      // 解析記錄格式: timestamp|systolic|diastolic|pulse[|valid]
      int sep1 = recData.indexOf('|');
      int sep2 = (sep1 > 0) ? recData.indexOf('|', sep1 + 1) : -1;
      int sep3 = (sep2 > 0) ? recData.indexOf('|', sep2 + 1) : -1;
      if (sep1 <= 0 || sep2 <= 0 || sep3 <= 0) continue;

      int sep4 = recData.indexOf('|', sep3 + 1);
      String timestamp = recData.substring(0, sep1);
      int systolic = recData.substring(sep1 + 1, sep2).toInt();
      int diastolic = recData.substring(sep2 + 1, sep3).toInt();
      int pulse;
      bool valid;
      if (sep4 > 0) {
        pulse = recData.substring(sep3 + 1, sep4).toInt();
        valid = (recData.substring(sep4 + 1).toInt() != 0);
      } else {
        pulse = recData.substring(sep3 + 1).toInt();
        valid = (systolic > 0 && diastolic > 0 && pulse > 0);
      }

      int slot = _recordCount - 1 - i; // 把最新放在最後一格
      _records[slot].timestamp = timestamp;
      _records[slot].systolic = systolic;
      _records[slot].diastolic = diastolic;
      _records[slot].pulse = pulse;
      _records[slot].valid = valid;
    }

    _historyIndex = _recordCount % _maxRecords;
    _preferences.end();
  }

  // 保存記錄到非易失性存儲（newest-first 格式）
  // ESP-IDF 的 NVS 對相同字串會跳過實體寫入，因此每次 addRecord 只會真正
  // 寫入有變動的欄位（通常是 rec_0、count、index），對 flash 損耗影響可控。
  void saveToStorage() {
    _preferences.begin(_namespace.c_str(), false);

    _preferences.putInt("count", _recordCount);
    _preferences.putInt("index", _historyIndex);

    for (int i = 0; i < _recordCount; i++) {
      int recordIndex = (_historyIndex - i - 1 + _maxRecords) % _maxRecords;
      const BPData& record = _records[recordIndex];

      String recData = record.timestamp + "|" +
                       String(record.systolic) + "|" +
                       String(record.diastolic) + "|" +
                       String(record.pulse) + "|" +
                       String(record.valid ? 1 : 0);

      String key = "rec_" + String(i);
      _preferences.putString(key.c_str(), recData);
    }

    _preferences.end();
  }
};

#endif 