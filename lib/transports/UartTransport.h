#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include <Arduino.h>
#include "MonitorTransport.h"

class UartTransport : public MonitorTransport {
private:
  HardwareSerial* serial;
  int rxPin;
  int txPin;
  unsigned long baudRate;
  MonitorTransportState currentState;
  String currentDetail;

public:
  UartTransport(HardwareSerial* serial, int rxPin, int txPin, unsigned long baudRate)
    : serial(serial), rxPin(rxPin), txPin(txPin), baudRate(baudRate), currentState(TRANSPORT_STATE_STARTING) {}

  bool begin() override {
    serial->begin(baudRate, SERIAL_8N1, rxPin, txPin);
    currentState = TRANSPORT_STATE_READY;
    currentDetail = "UART fallback active (RX:" + String(rxPin) + ", TX:" + String(txPin) + ", " + String(baudRate) + "bps)";
    return true;
  }

  void poll() override {
    if (serial->available() > 0) {
      currentState = TRANSPORT_STATE_RECEIVING;
      return;
    }
    if (currentState == TRANSPORT_STATE_RECEIVING) {
      currentState = TRANSPORT_STATE_READY;
    }
  }

  int available() override {
    return serial->available();
  }

  int read() override {
    int value = serial->read();
    if (value >= 0) {
      currentState = TRANSPORT_STATE_RECEIVING;
    }
    return value;
  }

  const char* name() const override {
    return "UART fallback";
  }

  MonitorTransportState state() const override {
    return currentState;
  }

  String detail() const override {
    return currentDetail;
  }

  bool isFallback() const override {
    return true;
  }
};

#endif
