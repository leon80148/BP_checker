#ifndef USB_CDC_STATE_H
#define USB_CDC_STATE_H

#include <stddef.h>
#include <stdint.h>

enum class UsbCdcPhase : uint8_t {
  STARTING = 0,
  INSTALLING,
  WAITING_DEVICE,
  OPENING,
  CONFIGURING,
  READY,
  RETRY_WAIT,
  ERROR,
};

enum class UsbCdcRetryTarget : uint8_t {
  NONE = 0,
  INSTALL,
  OPEN,
};

enum class UsbCdcControlType : uint8_t {
  BEGIN = 0,
  HOST_INSTALL_OK,
  HOST_INSTALL_FAILED,
  DRIVER_INSTALL_OK,
  DRIVER_INSTALL_FAILED,
  DEVICE_ATTACHED,
  OPEN_STARTED,
  OPEN_SUCCEEDED,
  OPEN_FAILED,
  CONFIG_SUCCEEDED,
  CONFIG_FAILED,
  HANDLE_CLOSED,
  RX_ACTIVITY,
  RX_OVERFLOW,
  RX_CAPACITY_RECOVERED,
  TRANSFER_ERROR,
  DEVICE_DISCONNECTED,
  RETRY_TICK,
  CONTROL_QUEUE_OVERFLOW,
};

struct UsbCdcControlEvent {
  UsbCdcControlType type = UsbCdcControlType::BEGIN;
  int32_t code = 0;
  uint32_t count = 0;
  uint32_t session = 0;
};

enum class UsbCdcOrderedType : uint8_t {
  DISCONTINUITY = 0,
  STREAM_RESET,
};

struct UsbCdcOrderedEvent {
  UsbCdcOrderedType type = UsbCdcOrderedType::DISCONTINUITY;
  uint32_t session = 0;
  uint32_t epoch = 0;
  uint32_t byteBoundary = 0;
  uint32_t droppedBytes = 0;
};

// Main-owner cursor for a byte stream plus a separate FIFO of controls. A
// control becomes visible exactly after all bytes accepted before its boundary.
class UsbCdcOrderedCursor {
public:
  void beginSession(uint32_t session, uint32_t deliveredByteSequence,
                    uint32_t epoch) {
    _session = session;
    _deliveredByteSequence = deliveredByteSequence;
    _epoch = epoch;
  }

  bool controlStale(const UsbCdcOrderedEvent& event) const {
    if (event.session == _session) return false;
    return event.session - _session > UINT32_MAX / 2U;
  }

  bool controlDue(const UsbCdcOrderedEvent& event) const {
    return !controlStale(event) &&
           event.byteBoundary == _deliveredByteSequence;
  }

  void noteByteDelivered() { _deliveredByteSequence++; }

  void applyControl(const UsbCdcOrderedEvent& event) {
    if (!controlDue(event)) return;
    _session = event.session;
    _epoch = event.epoch;
    _droppedBytes = saturatingAdd(_droppedBytes, event.droppedBytes);
  }

  uint32_t session() const { return _session; }
  uint32_t deliveredByteSequence() const { return _deliveredByteSequence; }
  uint32_t epoch() const { return _epoch; }
  uint32_t droppedBytes() const { return _droppedBytes; }

private:
  uint32_t _session = 0;
  uint32_t _deliveredByteSequence = 0;
  uint32_t _epoch = 0;
  uint32_t _droppedBytes = 0;

  static uint32_t saturatingAdd(uint32_t left, uint32_t right) {
    if (UINT32_MAX - left < right) return UINT32_MAX;
    return left + right;
  }
};

static constexpr size_t USB_CDC_LIFECYCLE_CRITICAL_RESERVE = 4;

inline bool usbCdcMayEnqueueNormalControl(size_t queueSpaces) {
  return queueSpaces > USB_CDC_LIFECYCLE_CRITICAL_RESERVE;
}

inline bool usbCdcMayEnqueueCriticalControl(size_t queueSpaces) {
  return queueSpaces > 0;
}

class UsbCdcLifecycle {
public:
  void apply(const UsbCdcControlEvent& event, uint64_t nowMs) {
    switch (event.type) {
      case UsbCdcControlType::BEGIN:
        _phase = UsbCdcPhase::INSTALLING;
        _retryTarget = UsbCdcRetryTarget::NONE;
        break;

      case UsbCdcControlType::HOST_INSTALL_OK:
        _hostReady = true;
        _lastError = 0;
        break;

      case UsbCdcControlType::HOST_INSTALL_FAILED:
        scheduleInstallRetry(event.code, nowMs);
        break;

      case UsbCdcControlType::DRIVER_INSTALL_OK:
        if (_hostReady) {
          _driverReady = true;
          _phase = UsbCdcPhase::WAITING_DEVICE;
          _retryTarget = UsbCdcRetryTarget::NONE;
          _installRetryAttempt = 0;
          _lastError = 0;
        }
        break;

      case UsbCdcControlType::DRIVER_INSTALL_FAILED:
        scheduleInstallRetry(event.code, nowMs);
        break;

      case UsbCdcControlType::DEVICE_ATTACHED:
        _deviceAttached = true;
        if (_hostReady && _driverReady && !_connected && !_handleOwned) {
          _phase = UsbCdcPhase::WAITING_DEVICE;
        }
        break;

      case UsbCdcControlType::OPEN_STARTED:
        if (shouldAttemptOpen(nowMs)) {
          // Main owner begins a new handle/session generation only after every
          // prior handle has closed and the device is attached again.
          _terminalHandled = false;
          _overflowActive = false;
          _openInProgress = true;
          _phase = UsbCdcPhase::OPENING;
        }
        break;

      case UsbCdcControlType::OPEN_SUCCEEDED:
        _openInProgress = false;
        _handleOwned = true;  // The returned candidate must always gain an owner.
        _closeIssued = false;
        if (_phase == UsbCdcPhase::OPENING && _deviceAttached && _hostReady &&
            _driverReady && !_terminalHandled) {
          _phase = UsbCdcPhase::CONFIGURING;
          _lastError = 0;
        } else {
          requestClose();
          if (!_deviceAttached) _phase = UsbCdcPhase::WAITING_DEVICE;
        }
        break;

      case UsbCdcControlType::OPEN_FAILED:
        if (_phase == UsbCdcPhase::OPENING && !_terminalHandled) {
          _openInProgress = false;
          _handleOwned = false;
          scheduleOpenRetry(event.code, nowMs);
        }
        break;

      case UsbCdcControlType::CONFIG_SUCCEEDED:
        if (_phase == UsbCdcPhase::CONFIGURING && _handleOwned &&
            _deviceAttached && _hostReady && _driverReady &&
            !_terminalHandled) {
          _connected = true;
          _phase = UsbCdcPhase::READY;
          _retryTarget = UsbCdcRetryTarget::NONE;
          _openRetryAttempt = 0;
          _lastError = 0;
          _overflowActive = false;
          _terminalHandled = false;
          if (_everConnected) saturatingIncrement(_reconnectCount);
          _everConnected = true;
        }
        break;

      case UsbCdcControlType::CONFIG_FAILED:
        if (_phase == UsbCdcPhase::CONFIGURING && _handleOwned &&
            !_terminalHandled) {
          _connected = false;
          beginTerminalLoss();
          requestClose();
          scheduleOpenRetry(event.code, nowMs);
        }
        break;

      case UsbCdcControlType::HANDLE_CLOSED:
        _handleOwned = false;
        _closePending = false;
        _closeIssued = false;
        if (!_deviceAttached) {
          _phase = UsbCdcPhase::WAITING_DEVICE;
          _retryTarget = UsbCdcRetryTarget::NONE;
        } else if (_retryTarget == UsbCdcRetryTarget::OPEN &&
                   nowMs >= _retryAtMs) {
          _phase = UsbCdcPhase::WAITING_DEVICE;
          _retryTarget = UsbCdcRetryTarget::NONE;
        }
        break;

      case UsbCdcControlType::RX_ACTIVITY:
        if (_connected) _phase = UsbCdcPhase::READY;
        break;

      case UsbCdcControlType::RX_OVERFLOW:
        if (!_overflowActive) {
          _overflowActive = true;
          saturatingIncrement(_overflowEpisodes);
          beginOverflowLoss();
        }
        _droppedBytes = saturatingAdd(_droppedBytes, event.count);
        break;

      case UsbCdcControlType::RX_CAPACITY_RECOVERED:
        _overflowActive = false;
        break;

      case UsbCdcControlType::TRANSFER_ERROR:
        if (!_terminalHandled) {
          _connected = false;
          _openInProgress = false;
          beginTerminalLoss();
          requestClose();
          scheduleOpenRetry(event.code, nowMs);
        }
        break;

      case UsbCdcControlType::DEVICE_DISCONNECTED:
        _connected = false;
        _openInProgress = false;
        _deviceAttached = false;
        beginTerminalLoss();
        requestClose();
        _phase = UsbCdcPhase::WAITING_DEVICE;
        _retryTarget = UsbCdcRetryTarget::NONE;
        break;

      case UsbCdcControlType::RETRY_TICK:
        if (_phase == UsbCdcPhase::RETRY_WAIT && nowMs >= _retryAtMs) {
          bool transitioned = false;
          if (_retryTarget == UsbCdcRetryTarget::INSTALL) {
            _phase = UsbCdcPhase::INSTALLING;
            transitioned = true;
          } else if (_retryTarget == UsbCdcRetryTarget::OPEN &&
                     _hostReady && _driverReady && _deviceAttached &&
                     !_handleOwned && !_closePending) {
            _phase = UsbCdcPhase::WAITING_DEVICE;
            transitioned = true;
          }
          if (transitioned) _retryTarget = UsbCdcRetryTarget::NONE;
        }
        break;

      case UsbCdcControlType::CONTROL_QUEUE_OVERFLOW:
        if (!_terminalHandled) {
          _connected = false;
          beginTerminalLoss();
          requestClose();
          if (_hostReady && _driverReady && _deviceAttached) {
            scheduleOpenRetry(event.code, nowMs);
          } else {
            scheduleInstallRetry(event.code, nowMs);
          }
        }
        break;
    }
  }

  bool shouldAttemptOpen(uint64_t nowMs) const {
    if (!_hostReady || !_driverReady || !_deviceAttached || _connected ||
        _openInProgress || _handleOwned || _closePending) {
      return false;
    }
    if (_phase == UsbCdcPhase::WAITING_DEVICE) return true;
    return _phase == UsbCdcPhase::RETRY_WAIT &&
           _retryTarget == UsbCdcRetryTarget::OPEN && nowMs >= _retryAtMs;
  }

  bool takeCloseRequest() {
    if (!_closePending || _closeIssued) return false;
    _closeIssued = true;
    return true;
  }

  UsbCdcPhase phase() const { return _phase; }
  UsbCdcRetryTarget retryTarget() const { return _retryTarget; }
  bool hostReady() const { return _hostReady; }
  bool driverReady() const { return _driverReady; }
  bool deviceAttached() const { return _deviceAttached; }
  bool openInProgress() const { return _openInProgress; }
  bool connected() const { return _connected; }
  bool handleOwned() const { return _handleOwned; }
  bool closePending() const { return _closePending; }
  uint64_t retryAtMs() const { return _retryAtMs; }
  int32_t lastError() const { return _lastError; }
  uint32_t rxEpoch() const { return _rxEpoch; }
  uint32_t dataLossEpisodes() const { return _dataLossEpisodes; }
  uint32_t overflowEpisodes() const { return _overflowEpisodes; }
  uint32_t droppedBytes() const { return _droppedBytes; }
  uint32_t reconnectCount() const { return _reconnectCount; }

private:
  UsbCdcPhase _phase = UsbCdcPhase::STARTING;
  UsbCdcRetryTarget _retryTarget = UsbCdcRetryTarget::NONE;
  bool _hostReady = false;
  bool _driverReady = false;
  bool _deviceAttached = false;
  bool _openInProgress = false;
  bool _connected = false;
  bool _handleOwned = false;
  bool _closePending = false;
  bool _closeIssued = false;
  bool _everConnected = false;
  bool _overflowActive = false;
  bool _terminalHandled = false;
  uint8_t _installRetryAttempt = 0;
  uint8_t _openRetryAttempt = 0;
  uint64_t _retryAtMs = 0;
  int32_t _lastError = 0;
  uint32_t _rxEpoch = 0;
  uint32_t _dataLossEpisodes = 0;
  uint32_t _overflowEpisodes = 0;
  uint32_t _droppedBytes = 0;
  uint32_t _reconnectCount = 0;

  static uint64_t retryDelayMs(uint8_t attempt) {
    uint8_t shift = attempt > 5 ? 4 : static_cast<uint8_t>(attempt - 1);
    return static_cast<uint64_t>(1000) << shift;
  }

  static uint32_t saturatingAdd(uint32_t left, uint32_t right) {
    if (UINT32_MAX - left < right) return UINT32_MAX;
    return left + right;
  }

  void scheduleInstallRetry(int32_t error, uint64_t nowMs) {
    _hostReady = false;
    _driverReady = false;
    _connected = false;
    _openInProgress = false;
    if (_installRetryAttempt < UINT8_MAX) _installRetryAttempt++;
    _retryAtMs = nowMs + retryDelayMs(_installRetryAttempt);
    _retryTarget = UsbCdcRetryTarget::INSTALL;
    _lastError = error;
    _phase = UsbCdcPhase::RETRY_WAIT;
  }

  void scheduleOpenRetry(int32_t error, uint64_t nowMs) {
    if (_openRetryAttempt < UINT8_MAX) _openRetryAttempt++;
    _retryAtMs = nowMs + retryDelayMs(_openRetryAttempt);
    _retryTarget = UsbCdcRetryTarget::OPEN;
    _lastError = error;
    _phase = UsbCdcPhase::RETRY_WAIT;
  }

  void requestClose() {
    if (_handleOwned && !_closeIssued) _closePending = true;
  }

  static void saturatingIncrement(uint32_t& value) {
    if (value < UINT32_MAX) value++;
  }

  void countLoss() {
    saturatingIncrement(_dataLossEpisodes);
    saturatingIncrement(_rxEpoch);
  }

  void beginOverflowLoss() { countLoss(); }

  void beginTerminalLoss() {
    if (_terminalHandled) return;
    _terminalHandled = true;
    countLoss();
  }
};

#endif
