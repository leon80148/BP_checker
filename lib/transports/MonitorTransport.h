#ifndef MONITOR_TRANSPORT_H
#define MONITOR_TRANSPORT_H

#include <Arduino.h>

enum MonitorTransportState {
  TRANSPORT_STATE_STARTING = 0,
  TRANSPORT_STATE_WAITING_DEVICE = 1,
  TRANSPORT_STATE_READY = 2,
  TRANSPORT_STATE_RECEIVING = 3,
  TRANSPORT_STATE_UNSUPPORTED = 4,
  TRANSPORT_STATE_ERROR = 5,
};

class MonitorTransport {
public:
  virtual ~MonitorTransport() {}
  virtual bool begin() = 0;
  virtual void poll() = 0;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual const char* name() const = 0;
  virtual MonitorTransportState state() const = 0;
  virtual String detail() const = 0;
  virtual bool isFallback() const = 0;
};

#endif
