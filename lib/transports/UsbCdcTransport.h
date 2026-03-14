#ifndef USB_CDC_TRANSPORT_H
#define USB_CDC_TRANSPORT_H

#include <Arduino.h>
#include "MonitorTransport.h"

class UsbCdcTransport : public MonitorTransport {
private:
  MonitorTransportState currentState;
  String currentDetail;

public:
  UsbCdcTransport() : currentState(TRANSPORT_STATE_STARTING) {}

  bool begin() override {
#if __has_include("usb/usb_host.h")
    currentState = TRANSPORT_STATE_UNSUPPORTED;
    currentDetail = "OTG selected. USB Host headers exist in the toolchain, but CDC host runtime is not implemented in this Arduino sketch yet.";
#else
    currentState = TRANSPORT_STATE_UNSUPPORTED;
    currentDetail = "OTG selected, but the installed toolchain does not expose USB Host headers to the sketch build.";
#endif
    return false;
  }

  void poll() override {}

  int available() override {
    return 0;
  }

  int read() override {
    return -1;
  }

  const char* name() const override {
    return "USB OTG Host";
  }

  MonitorTransportState state() const override {
    return currentState;
  }

  String detail() const override {
    return currentDetail;
  }

  bool isFallback() const override {
    return false;
  }
};

#endif
