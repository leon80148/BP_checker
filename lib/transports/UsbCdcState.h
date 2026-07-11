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

inline bool usbCdcConfigurationMayContinue(UsbCdcPhase phase,
                                           bool closePending,
                                           bool handleMatches,
                                           bool contextValid,
                                           bool tokenValid) {
  return phase == UsbCdcPhase::CONFIGURING && !closePending &&
         handleMatches && contextValid && tokenValid;
}

enum class UsbCdcRetryTarget : uint8_t {
  NONE = 0,
  INSTALL,
  OPEN,
};

enum class UsbCdcDaemonPhase : uint8_t {
  STOPPED = 0,
  STARTING,
  RUNNING,
  STOPPING,
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
  HANDLE_CLOSE_FAILED,
  RX_ACTIVITY,
  RX_OVERFLOW,
  RX_CAPACITY_RECOVERED,
  TRANSFER_ERROR,
  DEVICE_DISCONNECTED,
  RETRY_TICK,
  CONTROL_QUEUE_OVERFLOW,
};

inline uint32_t usbCdcEffectiveControlSession(UsbCdcControlType type,
                                              uint32_t eventSession,
                                              uint32_t activeSession) {
  if (type == UsbCdcControlType::CONTROL_QUEUE_OVERFLOW &&
      eventSession == 0) {
    return activeSession;
  }
  return eventSession;
}

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

enum class UsbCdcTerminalFact : uint8_t {
  NONE = 0,
  ERROR,
  DISCONNECTED,
};

enum class UsbCdcTerminalUpdate : uint8_t {
  IGNORED = 0,
  FIRST,
  UPGRADED,
};

class UsbCdcTerminalBoundaryTracker {
public:
  bool needsPublish(UsbCdcOrderedType requested) const {
    return !_published ||
           (_type == UsbCdcOrderedType::DISCONTINUITY &&
            requested == UsbCdcOrderedType::STREAM_RESET);
  }

  void notePublished(UsbCdcOrderedType type) {
    if (!_published || type == UsbCdcOrderedType::STREAM_RESET) {
      _type = type;
      _published = true;
    }
  }

  void reset() {
    _published = false;
    _type = UsbCdcOrderedType::DISCONTINUITY;
  }

private:
  UsbCdcOrderedType _type = UsbCdcOrderedType::DISCONTINUITY;
  bool _published = false;
};

struct UsbCdcOrderedEvent {
  UsbCdcOrderedType type = UsbCdcOrderedType::DISCONTINUITY;
  uint32_t session = 0;
  uint32_t epoch = 0;
  uint32_t byteBoundary = 0;
  uint32_t droppedBytes = 0;
};

inline uint32_t usbCdcNextSession(uint32_t current) {
  current++;
  return current == 0 ? 1 : current;
}

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

enum class UsbCdcOrderedPublishResult : uint8_t {
  QUEUED = 0,
  FALLBACK_CREATED,
  FALLBACK_MERGED,
};

enum class UsbCdcOrderedClaimResult : uint8_t {
  NONE = 0,
  BLOCKED,
  CLAIMED,
  STALE_QUEUE_DISCARDED,
  STALE_FALLBACK_DISCARDED,
};

struct UsbCdcOrderedDelivery {
  UsbCdcOrderedEvent event;
  bool resumeProducer = false;
  bool fromFallback = false;
  bool quarantineBarrier = false;
  bool producerResumed = false;
};

// Fixed-capacity ordered control channel. The caller supplies synchronization:
// production wraps every method with one portMUX, while host stress uses one
// std::mutex. Once the ring fills, every later control merges into the single
// fallback so no newer queue item can overtake it.
template <size_t Capacity>
class UsbCdcOrderedChannel {
  static_assert(Capacity > 0, "ordered channel capacity must be nonzero");

public:
  UsbCdcOrderedPublishResult publish(const UsbCdcOrderedEvent& event,
                                     bool resumeProducer,
                                     bool quarantineBarrier = false) {
    if (_fallbackPending) {
      mergeFallback(event, resumeProducer, quarantineBarrier);
      return UsbCdcOrderedPublishResult::FALLBACK_MERGED;
    }
    if (_count < Capacity) {
      _items[_tail].event = event;
      _items[_tail].resumeProducer = resumeProducer;
      _items[_tail].fromFallback = false;
      _items[_tail].quarantineBarrier = quarantineBarrier;
      _items[_tail].producerResumed = false;
      _tail = (_tail + 1) % Capacity;
      _count++;
      return UsbCdcOrderedPublishResult::QUEUED;
    }
    _fallback.event = event;
    _fallback.resumeProducer = resumeProducer;
    _fallback.fromFallback = true;
    _fallback.quarantineBarrier = quarantineBarrier;
    _fallback.producerResumed = false;
    _fallbackPending = true;
    return UsbCdcOrderedPublishResult::FALLBACK_CREATED;
  }

  UsbCdcOrderedClaimResult claim(const UsbCdcOrderedCursor& cursor,
                                 UsbCdcOrderedDelivery& delivery) {
    if (_count > 0) {
      const UsbCdcOrderedDelivery& head = _items[_head];
      if (cursor.controlStale(head.event)) {
        popQueue();
        return UsbCdcOrderedClaimResult::STALE_QUEUE_DISCARDED;
      }
      if (!cursor.controlDue(head.event)) {
        return UsbCdcOrderedClaimResult::BLOCKED;
      }
      delivery = head;
      popQueue();
      return UsbCdcOrderedClaimResult::CLAIMED;
    }
    if (!_fallbackPending) return UsbCdcOrderedClaimResult::NONE;
    if (cursor.controlStale(_fallback.event)) {
      clearFallback();
      return UsbCdcOrderedClaimResult::STALE_FALLBACK_DISCARDED;
    }
    if (!cursor.controlDue(_fallback.event)) {
      return UsbCdcOrderedClaimResult::BLOCKED;
    }
    delivery = _fallback;
    clearFallback();
    return UsbCdcOrderedClaimResult::CLAIMED;
  }

  size_t pendingCount() const { return _count + (_fallbackPending ? 1 : 0); }
  bool fallbackPending() const { return _fallbackPending; }

  bool hasPendingQuarantineBarrier() const {
    if (_fallbackPending && _fallback.quarantineBarrier) return true;
    for (size_t offset = 0; offset < _count; ++offset) {
      size_t index = (_head + offset) % Capacity;
      if (_items[index].quarantineBarrier) return true;
    }
    return false;
  }

  bool appendDroppedBytesToResume(uint32_t session, uint32_t epoch,
                                  uint32_t droppedBytes) {
    if (_fallbackPending && _fallback.quarantineBarrier &&
        _fallback.event.type == UsbCdcOrderedType::DISCONTINUITY &&
        _fallback.event.session == session &&
        _fallback.event.epoch == epoch) {
      _fallback.event.droppedBytes = saturatingAdd(
        _fallback.event.droppedBytes, droppedBytes);
      return true;
    }
    for (size_t offset = 0; offset < _count; ++offset) {
      size_t index = (_tail + Capacity - 1 - offset) % Capacity;
      UsbCdcOrderedDelivery& delivery = _items[index];
      if (delivery.quarantineBarrier &&
          delivery.event.type == UsbCdcOrderedType::DISCONTINUITY &&
          delivery.event.session == session &&
          delivery.event.epoch == epoch) {
        delivery.event.droppedBytes = saturatingAdd(
          delivery.event.droppedBytes, droppedBytes);
        return true;
      }
    }
    return false;
  }

  void clear() {
    for (size_t i = 0; i < Capacity; ++i) _items[i] = UsbCdcOrderedDelivery{};
    _head = 0;
    _tail = 0;
    _count = 0;
    clearFallback();
  }

private:
  UsbCdcOrderedDelivery _items[Capacity] = {};
  size_t _head = 0;
  size_t _tail = 0;
  size_t _count = 0;
  UsbCdcOrderedDelivery _fallback;
  bool _fallbackPending = false;

  static uint32_t saturatingAdd(uint32_t left, uint32_t right) {
    if (UINT32_MAX - left < right) return UINT32_MAX;
    return left + right;
  }

  void popQueue() {
    _items[_head] = UsbCdcOrderedDelivery{};
    _head = (_head + 1) % Capacity;
    _count--;
  }

  void clearFallback() {
    _fallback = UsbCdcOrderedDelivery{};
    _fallbackPending = false;
  }

  void mergeFallback(const UsbCdcOrderedEvent& event, bool resumeProducer,
                     bool quarantineBarrier) {
    _fallback.event.droppedBytes = saturatingAdd(
      _fallback.event.droppedBytes, event.droppedBytes);

    if (event.type == UsbCdcOrderedType::STREAM_RESET) {
      // A later reset dominates an overflow marker. No bytes can be accepted
      // while fallback is pending, so the original boundary stays exact.
      _fallback.event.type = UsbCdcOrderedType::STREAM_RESET;
      _fallback.event.session = event.session;
      _fallback.event.epoch = event.epoch;
      _fallback.resumeProducer = resumeProducer;
      _fallback.quarantineBarrier = quarantineBarrier;
    } else if (_fallback.event.type != UsbCdcOrderedType::STREAM_RESET) {
      _fallback.event.session = event.session;
      _fallback.event.epoch = event.epoch;
      _fallback.resumeProducer =
        _fallback.resumeProducer || resumeProducer;
      _fallback.quarantineBarrier =
        _fallback.quarantineBarrier || quarantineBarrier;
    }
  }
};

struct UsbCdcConfigToken {
  uint32_t session = 0;
  uint32_t terminalGeneration = 0;
};

// Session/configuration gate. The caller holds the same short lock around
// callbacks and main-loop commits, making terminal-vs-config ordering linear.
class UsbCdcSessionGate {
public:
  void startSession(uint32_t session) {
    _session = session;
    _producerEnabled = false;
    _terminalSeen = false;
  }

  UsbCdcConfigToken configurationToken() const {
    UsbCdcConfigToken token;
    token.session = _session;
    token.terminalGeneration = _terminalGeneration;
    return token;
  }

  bool commitConfiguration(const UsbCdcConfigToken& token,
                           bool enableProducer) {
    if (!configurationTokenValid(token)) return false;
    _producerEnabled = enableProducer;
    return true;
  }

  bool configurationTokenValid(const UsbCdcConfigToken& token) const {
    return token.session != 0 && token.session == _session && !_terminalSeen &&
           token.terminalGeneration == _terminalGeneration;
  }

  bool noteTerminal(uint32_t session) {
    if (session == 0 || session != _session || _terminalSeen) return false;
    _terminalSeen = true;
    _producerEnabled = false;
    _terminalGeneration++;
    return true;
  }

  bool stopProducer(uint32_t session) {
    if (session == 0 || session != _session) return false;
    _producerEnabled = false;
    return true;
  }

  bool resumeProducer(uint32_t session) {
    if (session == 0 || session != _session || _terminalSeen) return false;
    _producerEnabled = true;
    return true;
  }

  bool callbackEnabled(uint32_t session) const {
    return session != 0 && session == _session && _producerEnabled &&
           !_terminalSeen;
  }

  bool sessionCurrent(uint32_t session) const {
    return session != 0 && session == _session;
  }

  bool terminalSeen() const { return _terminalSeen; }
  uint32_t session() const { return _session; }

private:
  uint32_t _session = 0;
  uint32_t _terminalGeneration = 0;
  bool _producerEnabled = false;
  bool _terminalSeen = false;
};

// Main-owner fixed slot pool. A slot's session is never mutated while active;
// production releases it only after the CDC task has synchronously closed the
// associated handle.
template <size_t Capacity>
class UsbCdcSessionSlots {
  static_assert(Capacity > 0, "callback slot capacity must be nonzero");

public:
  int acquire(uint32_t session) {
    if (session == 0) return -1;
    for (size_t i = 0; i < Capacity; ++i) {
      if (!_slots[i].active) {
        _slots[i].session = session;
        _slots[i].active = true;
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  bool release(size_t index, uint32_t session) {
    if (!matches(index, session)) return false;
    _slots[index] = Slot{};
    return true;
  }

  bool matches(size_t index, uint32_t session) const {
    return index < Capacity && _slots[index].active && session != 0 &&
           _slots[index].session == session;
  }

private:
  struct Slot {
    uint32_t session = 0;
    bool active = false;
  };
  Slot _slots[Capacity] = {};
};

class UsbCdcTeardownTracker {
public:
  explicit UsbCdcTeardownTracker(bool driverInstalled)
    : _cdcUninstalled(!driverInstalled), _noClients(!driverInstalled) {}

  void noteCdcUninstalled() { _cdcUninstalled = true; }
  void noteNoClients() { _noClients = true; }
  void noteFreeAllRequested(bool alreadyFree) {
    _freeRequested = true;
    if (alreadyFree) _allFree = true;
  }
  void noteAllFree() { _allFree = true; }
  void noteHostUninstalled() { _hostUninstalled = true; }

  bool mayFreeDevices() const { return _cdcUninstalled && _noClients; }
  bool mayUninstallHost() const {
    return mayFreeDevices() && _freeRequested && _allFree;
  }
  bool complete() const { return mayUninstallHost() && _hostUninstalled; }

private:
  bool _cdcUninstalled = false;
  bool _noClients = false;
  bool _freeRequested = false;
  bool _allFree = false;
  bool _hostUninstalled = false;
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

      case UsbCdcControlType::HANDLE_CLOSE_FAILED:
        if (_handleOwned) {
          _closePending = true;
          _closeIssued = false;
          _lastError = event.code;
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
