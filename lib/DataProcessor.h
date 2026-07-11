#ifndef DATA_PROCESSOR_H
#define DATA_PROCESSOR_H

#include <Arduino.h>
#include <utility>

#include "BP_Parser.h"
#include "BPRecordManager.h"
#include "ProtocolFramer.h"
#include "transports/MonitorTransport.h"

class DataProcessor {
private:
  BP_Parser* bpParser;
  BP_RecordManager* recordManager;
  String* lastData;
  String* transportName;
  String* transportStatus;
  MonitorTransport* transport;

  bool transportActive = false;
  unsigned long lastTransportActivity = 0;

  ProtocolFramer framer;
  ProtocolFrameContract frameContract;
  String framedModel;
  uint32_t rxEpoch = 0;
  bool rxEpochKnown = false;

  MonitorTransportState lastSyncedState = TRANSPORT_STATE_STARTING;
  String lastSyncedDetail;
  bool statusEverSynced = false;

  const char* stateLabel(MonitorTransportState state) const {
    switch (state) {
      case TRANSPORT_STATE_STARTING:       return "啟動中";
      case TRANSPORT_STATE_WAITING_DEVICE: return "等待裝置";
      case TRANSPORT_STATE_READY:          return "就緒";
      case TRANSPORT_STATE_RECEIVING:      return "接收中";
      case TRANSPORT_STATE_UNSUPPORTED:    return "未就緒";
      case TRANSPORT_STATE_ERROR:          return "錯誤";
      default:                             return "未知";
    }
  }

  void syncTransportStatus() {
    MonitorTransportState state = transport->state();
    String detail = transport->detail();
    if (statusEverSynced && state == lastSyncedState &&
        detail == lastSyncedDetail) {
      return;
    }
    lastSyncedState = state;
    lastSyncedDetail = detail;
    statusEverSynced = true;
    String& target = *transportStatus;
    target = stateLabel(state);
    target += " - ";
    target += detail;
  }

  void syncFramingContract() {
    if (framedModel == bpParser->getModel()) return;
    framer.reset();
    framedModel = bpParser->getModel();
    frameContract = bpParser->framingContract();
  }

  static const char* operatorAction(BPParseError error) {
    switch (error) {
      case BPParseError::INVALID_TIMESTAMP:
        return "請校正血壓計日期與時間後重新量測。";
      case BPParseError::DEVICE_ERROR:
        return "請依血壓計錯誤碼處理後重新量測。";
      case BPParseError::OUT_OF_RANGE:
        return "請確認量測流程與設備狀態後重新量測。";
      case BPParseError::UNSUPPORTED_FORMAT:
        return "請將 HBP-9030 USB 輸出設定為格式 5。";
      case BPParseError::UNSUPPORTED_MODEL:
        return "請選用已驗證的 HBP-9030 設定。";
      default:
        return "請確認 USB 輸出格式與連線後重新量測。";
    }
  }

  void renderDiagnostic(const char* status, const char* action,
                        const BPData* measurement = nullptr) {
    String& target = *lastData;
    target = "";
    target.reserve(480);
    target += "<div class='diagnostic-data' data-status='";
    target += status;
    target += "'><h3>接收診斷</h3><p><strong>狀態：</strong>";
    target += status;
    target += "</p>";
    if (measurement != nullptr && measurement->valid) {
      target += "<p><strong>量測：</strong>SYS ";
      target += measurement->systolic;
      target += " / DIA ";
      target += measurement->diastolic;
      target += " / PULSE ";
      target += measurement->pulse;
      target += "</p><p><strong>設備時間：</strong>";
      target += measurement->timestamp;
      target += "</p>";
    }
    target += "<p class='helper-text'>";
    target += action;
    target += "</p></div>";
  }

  void renderFrameEvent(ProtocolFrameEvent event) {
    if (event == ProtocolFrameEvent::FRAME_OVERFLOW) {
      renderDiagnostic("overflow", "資料過長已丟棄；請確認輸出格式後重新量測。");
    } else if (event == ProtocolFrameEvent::DISCONTINUITY) {
      renderDiagnostic("discontinuity", "資料傳輸中斷；請確認連線後重新量測。");
    } else {
      renderDiagnostic("malformed", operatorAction(BPParseError::MALFORMED));
    }
  }

  bool finishFrame(const uint8_t* data, size_t length) {
    BPParseResult result = bpParser->parseResult(data, static_cast<int>(length));
    if (!result.ok() ||
        result.measurement.timestampSource != BPTimestampSource::DEVICE) {
      BPParseError error = result.error;
      if (result.ok()) error = BPParseError::INVALID_TIMESTAMP;
      renderDiagnostic(bpParseErrorCode(error), operatorAction(error));
      Serial.print("measurement_rejected reason=");
      Serial.println(bpParseErrorCode(error));
      return true;
    }

    BPData measurement = std::move(result.measurement);
    const int systolic = measurement.systolic;
    const int diastolic = measurement.diastolic;
    const int pulse = measurement.pulse;
    if (!recordManager->addRecord(std::move(measurement))) {
      renderDiagnostic(
        "storage_error",
        "儲存系統未能確認本次量測；請先查看歷史記錄確認是否已保存，再依診所流程重新量測。");
      Serial.println("measurement_storage_failed");
      return true;
    }
    renderDiagnostic("valid", "量測已接收；如需複測請依診所流程進行。",
                     &recordManager->getLatestRecord());

    Serial.print("measurement_accepted SYS=");
    Serial.print(systolic);
    Serial.print(" DIA=");
    Serial.print(diastolic);
    Serial.print(" PULSE=");
    Serial.println(pulse);
    return true;
  }

public:
  DataProcessor(BP_Parser* parser, BP_RecordManager* manager, String* diagnostics,
                String* name, String* status, MonitorTransport* monitor)
    : bpParser(parser),
      recordManager(manager),
      lastData(diagnostics),
      transportName(name),
      transportStatus(status),
      transport(monitor) {}

  void setup() {
    bool ok = transport->begin();
    *transportName = transport->name();
    syncTransportStatus();
    syncFramingContract();
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
    syncFramingContract();

    bool produced = false;
    bool unsupportedBytes = false;
    MonitorRxEvent rxEvent;
    while (transport->nextRxEvent(rxEvent)) {
      lastTransportActivity = millis();
      transportActive = true;

      if (rxEvent.type == MonitorRxEventType::DISCONTINUITY ||
          rxEvent.type == MonitorRxEventType::STREAM_RESET) {
        rxEpoch = rxEvent.epoch;
        rxEpochKnown = true;
        if (rxEvent.type == MonitorRxEventType::STREAM_RESET) {
          framer.reset();
        } else {
          framer.discardUntilBoundary();
        }
        continue;
      }
      if (rxEpochKnown && rxEvent.epoch != rxEpoch) {
        framer.discardUntilBoundary();
      }
      rxEpoch = rxEvent.epoch;
      rxEpochKnown = true;

      if (frameContract.mode == ProtocolFrameMode::UNSUPPORTED) {
        unsupportedBytes = true;
        continue;
      }

      ProtocolFrameEvent event =
        framer.feed(rxEvent.byte, frameContract);
      if (event == ProtocolFrameEvent::FRAME) {
        produced = finishFrame(framer.frameData(), framer.frameLength()) || produced;
        framer.clearCompletedFrame();
      } else if (event != ProtocolFrameEvent::NONE) {
        renderFrameEvent(event);
        produced = true;
      }
    }

    if (unsupportedBytes) {
      renderDiagnostic("unsupported_model",
                       operatorAction(BPParseError::UNSUPPORTED_MODEL));
      produced = true;
    }
    if (produced) syncTransportStatus();
    return produced;
  }

  void checkActivity() {
    if (transportActive && millis() - lastTransportActivity > 5000) {
      transportActive = false;
      Serial.print("資料通道已超過 5 秒沒有新資料: ");
      Serial.println(*transportStatus);
    }
  }
};

#endif
