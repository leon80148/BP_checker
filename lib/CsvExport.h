#ifndef CSV_EXPORT_H
#define CSV_EXPORT_H

#include <Arduino.h>
#include "BPRecordManager.h"

// 血壓歷史 CSV 匯出（純函式，host 可測）。
// 格式決策（與 codex 討論定案）：
//   - UTF-8 BOM + 中文標題，Excel 直開不亂碼；CRLF 行尾
//   - 所有欄位雙引號包裹，內含 '"' 以 "" 跳脫（防禦性，現有資料不含）
//   - 由舊到新（時間升冪），方便接續診所存檔試算表
//   - invalid 記錄（legacy -1 資料）不輸出：這是臨床報表不是診斷 dump

inline void __appendCsvField(String& out, const String& value) {
  out += '"';
  for (unsigned int i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
}

inline void appendHistoryCsv(String& out, const BP_RecordManager& mgr) {
  int count = mgr.getRecordCount();
  out.reserve(out.length() + 96 + count * 48);
  out += "\xEF\xBB\xBF";
  out += "\"測量時間\",\"收縮壓(mmHg)\",\"舒張壓(mmHg)\",\"脈搏(bpm)\"\r\n";

  // getRecord(0)=最新；倒序走訪 = 由舊到新
  for (int i = count - 1; i >= 0; i--) {
    const BPData& r = mgr.getRecord(i);
    if (!r.valid) continue;
    __appendCsvField(out, r.timestamp);
    out += ",\"";
    out += r.systolic;
    out += "\",\"";
    out += r.diastolic;
    out += "\",\"";
    out += r.pulse;
    out += "\"\r\n";
  }
}

#endif
