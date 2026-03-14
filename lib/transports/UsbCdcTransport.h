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
  const char* name() const override;
  MonitorTransportState state() const override;
  String detail() const override;
  bool isFallback() const override;

  // Exposed so the OTG callbacks implemented in sketch/src can access state
  // without pulling ESP-IDF USB types into this header.
  struct Impl;
  Impl* impl;
};

#endif
