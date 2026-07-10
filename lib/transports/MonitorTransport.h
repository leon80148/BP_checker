#ifndef MONITOR_TRANSPORT_H
#define MONITOR_TRANSPORT_H

#include <Arduino.h>
#include <stdint.h>

enum MonitorTransportState {
  TRANSPORT_STATE_STARTING = 0,
  TRANSPORT_STATE_WAITING_DEVICE = 1,
  TRANSPORT_STATE_READY = 2,
  TRANSPORT_STATE_RECEIVING = 3,
  TRANSPORT_STATE_UNSUPPORTED = 4,
  TRANSPORT_STATE_ERROR = 5,
};

enum class MonitorRxEventType : uint8_t {
  BYTE = 0,
  DISCONTINUITY,
};

// POD event boundary shared by transports and the main-loop frame owner.
// `epoch` orders a loss marker with the bytes it invalidates.
struct MonitorRxEvent {
  MonitorRxEventType type = MonitorRxEventType::BYTE;
  uint8_t byte = 0;
  uint32_t epoch = 0;
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

  // Legacy byte transports receive a safe adapter. Concurrent transports must
  // override this with their ordered POD queue so control events cannot be
  // overtaken by buffered data.
  virtual bool nextRxEvent(MonitorRxEvent& event) {
    if (available() <= 0) return false;
    int value = read();
    if (value < 0) return false;
    event.type = MonitorRxEventType::BYTE;
    event.byte = static_cast<uint8_t>(value);
    event.epoch = 0;
    return true;
  }

  virtual uint32_t dataLossCount() const { return 0; }
  virtual uint32_t reconnectCount() const { return 0; }
};

#endif
