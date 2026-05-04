#ifndef BP_CONFIG_H
#define BP_CONFIG_H

enum MonitorTransportMode {
  TRANSPORT_MODE_OTG_PRIMARY = 0,
  TRANSPORT_MODE_UART_FALLBACK = 1,
};

static constexpr MonitorTransportMode kTransportMode = TRANSPORT_MODE_OTG_PRIMARY;

static constexpr int kUartRxPin = 44;
static constexpr int kUartTxPin = 43;
static constexpr unsigned long kMonitorBaudRate = 9600;

// 多數 ESP32 開發板有 GPIO0 boot/reset 按鈕，長按 3 秒清空 WiFi 設定
static constexpr int kResetPin = 0;

#endif
