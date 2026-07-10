#ifndef BP_RecordManager_h
#define BP_RecordManager_h

#include <Arduino.h>
#include <Preferences.h>
#include "BP_Parser.h"

// 儲存策略：slot-based
//
//   slot_<i>  : 第 i 個物理 slot 的記錄（i 對應 _records[i]）
//   count     : 目前有效記錄數
//   index     : _historyIndex（下一個要寫的 slot）
//   schema    : "v2" 標記新格式；舊格式（rec_*）會在第一次 load 時被遷移
//
// 對比舊格式（rec_0=最新，rec_(count-1)=最舊）：每筆 addRecord 必須 rewrite
// 全部 count 個 rec_* keys；slot-based 只需寫入剛改動的單一 slot + 3 個 metadata。
// 對於 20 筆 ring buffer，NVS 寫入量從 22 降到 4（包含 schema），長期 flash
// 損耗約降低 5x。
class BP_RecordManager {
private:
  const int _maxRecords;
  BPData* _records;
  int _historyIndex;
  int _recordCount;
  Preferences _preferences;

  static constexpr const char* kNamespace = "bp_records";

  String serializeRecord(const BPData& r) const {
    String s;
    s.reserve(r.timestamp.length() + 24); // ts + 4 ints + 4 separators
    s += r.timestamp;
    s += '|';
    s += r.systolic;
    s += '|';
    s += r.diastolic;
    s += '|';
    s += r.pulse;
    s += '|';
    s += (r.valid ? '1' : '0');
    return s;
  }

  bool parseRecord(const String& recData, BPData& out) const {
    if (recData.length() == 0) return false;
    int sep1 = recData.indexOf('|');
    if (sep1 <= 0) return false;
    int sep2 = recData.indexOf('|', sep1 + 1);
    if (sep2 <= 0) return false;
    int sep3 = recData.indexOf('|', sep2 + 1);
    if (sep3 <= 0) return false;
    int sep4 = recData.indexOf('|', sep3 + 1);

    out.timestamp = recData.substring(0, sep1);
    out.systolic = recData.substring(sep1 + 1, sep2).toInt();
    out.diastolic = recData.substring(sep2 + 1, sep3).toInt();
    if (sep4 > 0) {
      out.pulse = recData.substring(sep3 + 1, sep4).toInt();
      out.valid = (recData.substring(sep4 + 1).toInt() != 0);
    } else {
      // 舊 4 欄格式相容：以數值合理性推導 valid
      out.pulse = recData.substring(sep3 + 1).toInt();
      out.valid = (out.systolic > 0 && out.diastolic > 0 && out.pulse > 0);
    }
    return true;
  }

  // 寫入單一 slot + metadata；addRecord 用此快取路徑（4 NVS writes）
  void saveSlot(int slot) {
    _preferences.begin(kNamespace, false);
    String recData = serializeRecord(_records[slot]);
    char key[24];
    snprintf(key, sizeof(key), "slot_%d", slot);
    _preferences.putString(key, recData);
    _preferences.putInt("count", _recordCount);
    _preferences.putInt("index", _historyIndex);
    _preferences.putString("schema", "v2");
    _preferences.end();
  }

  // 一次性寫入所有 in-memory slots（migration 用）
  void saveAllSlots() {
    if (_recordCount <= 0) return;
    _preferences.begin(kNamespace, false);
    char key[24];
    for (int i = 0; i < _recordCount; i++) {
      String recData = serializeRecord(_records[i]);
      snprintf(key, sizeof(key), "slot_%d", i);
      _preferences.putString(key, recData);
    }
    _preferences.putInt("count", _recordCount);
    _preferences.putInt("index", _historyIndex);
    _preferences.putString("schema", "v2");
    _preferences.end();
  }

public:
  explicit BP_RecordManager(int maxRecords = 10)
    : _maxRecords(maxRecords),
      _records(new BPData[maxRecords]),
      _historyIndex(0),
      _recordCount(0) {}

  ~BP_RecordManager() {
    delete[] _records;
  }

  // by-value 收參數讓 caller 可決定 copy（lvalue）或 move（std::move(rvalue)）。
  // 呼叫端用 std::move 時可省下一次 ~700B 的 rawData String 複製。
  void addRecord(BPData record) {
    int slot = _historyIndex;
    _records[slot] = std::move(record);
    _historyIndex = (_historyIndex + 1) % _maxRecords;
    if (_recordCount < _maxRecords) _recordCount++;
    saveSlot(slot);
  }

  // 獲取某個指定位置的記錄（0 = 最新）
  // 回傳 const ref：BPData 含 String 欄位（rawData 可達 ~700B），by-value 多次呼叫
  // 累積複製成本可觀。out-of-range 回傳一個靜態 default 實例。
  const BPData& getRecord(int index) const {
    if (index < 0 || index >= _recordCount) {
      static const BPData kEmpty;
      return kEmpty;
    }
    int actualIndex = (_historyIndex - index - 1 + _maxRecords) % _maxRecords;
    return _records[actualIndex];
  }

  const BPData& getLatestRecord() const {
    return getRecord(0);
  }

  int getRecordCount() const { return _recordCount; }
  int getMaxRecords() const { return _maxRecords; }

  void clearRecords() {
    _historyIndex = 0;
    _recordCount = 0;
    for (int i = 0; i < _maxRecords; i++) {
      _records[i] = BPData{};
    }
    _preferences.begin(kNamespace, false);
    _preferences.clear(); // 同時清掉舊 rec_* 與新 slot_* keys
    _preferences.end();
  }

  void loadFromStorage() {
    _preferences.begin(kNamespace, true);
    String schema = _preferences.getString("schema", "");
    int storedCount = _preferences.getInt("count", 0);
    int storedIndex = _preferences.getInt("index", 0);

    if (storedCount < 0) storedCount = 0;
    if (storedCount > _maxRecords) storedCount = _maxRecords;
    if (storedIndex < 0 || storedIndex >= _maxRecords) storedIndex = 0;

    if (schema == "v2") {
      // 新格式：直接以物理 slot 對應 _records[i]
      _recordCount = storedCount;
      _historyIndex = storedIndex;
      // count < max 表示 ring buffer 還沒環繞，只有 slot[0..count-1] 有資料；
      // count == max 已環繞，需讀全部 slot。
      int slotsToRead = (storedCount < _maxRecords) ? storedCount : _maxRecords;
      char key[24];
      for (int i = 0; i < slotsToRead; i++) {
        snprintf(key, sizeof(key), "slot_%d", i);
        String recData = _preferences.getString(key, "");
        parseRecord(recData, _records[i]); // 失敗則維持 default
      }
      _preferences.end();
    } else {
      // 舊格式 rec_0=newest..rec_(count-1)=oldest；放到 _records[count-1-i]
      _recordCount = storedCount;
      char key[24];
      for (int i = 0; i < _recordCount; i++) {
        snprintf(key, sizeof(key), "rec_%d", i);
        String recData = _preferences.getString(key, "");
        if (recData.length() == 0) continue;
        int slot = _recordCount - 1 - i;
        parseRecord(recData, _records[slot]);
      }
      _historyIndex = _recordCount % _maxRecords;
      _preferences.end();

      // 一次性 migration：用 v2 格式回寫；舊 rec_* keys 留下不主動清除
      // （NVS 空間影響微小，clearRecords 才會整體清掉）
      saveAllSlots();
    }
  }
};

#endif
