#ifndef USB_CDC_TRANSPORT_H
#define USB_CDC_TRANSPORT_H

#include <Arduino.h>
#include "MonitorTransport.h"

class UsbCdcTransport : public MonitorTransport {
public:
  UsbCdcTransport();
  ~UsbCdcTransport() override;

  bool begin() override;
  void poll() override;
  int available() override;
  int read() override;
  bool nextRxEvent(MonitorRxEvent& event) override;
  const char* name() const override;
  MonitorTransportState state() const override;
  String detail() const override;
  uint32_t dataLossCount() const override;
  uint32_t reconnectCount() const override;
  uint32_t droppedByteCount() const;
  uint32_t overflowEpisodeCount() const;

  // Exposed so the OTG callbacks implemented in sketch/src can access state
  // without pulling ESP-IDF USB types into this header.
  struct Impl;
  Impl* impl;
};

#endif
