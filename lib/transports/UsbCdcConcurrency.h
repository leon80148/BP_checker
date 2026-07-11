#ifndef USB_CDC_CONCURRENCY_H
#define USB_CDC_CONCURRENCY_H

#include <stddef.h>
#include <stdint.h>

#include "UsbCdcState.h"

template <typename Mutex>
class UsbCdcScopedLock {
public:
  explicit UsbCdcScopedLock(Mutex& mutex) : _mutex(mutex) { _mutex.lock(); }
  ~UsbCdcScopedLock() { _mutex.unlock(); }

  UsbCdcScopedLock(const UsbCdcScopedLock&) = delete;
  UsbCdcScopedLock& operator=(const UsbCdcScopedLock&) = delete;

private:
  Mutex& _mutex;
};

enum class UsbCdcCloseCompletion : uint8_t {
  RELEASED = 0,
  DRIVER_FAILED,
  CALLBACKS_ACTIVE,
  STALE_CONTEXT,
};

enum class UsbCdcByteAdmission : uint8_t {
  ADMITTED = 0,
  DROP_RECORDED,
  REJECTED_TERMINAL,
  REJECTED_QUARANTINED,
  REJECTED_INACTIVE,
};

struct UsbCdcDiagnosticsSnapshot {
  uint32_t producerEpoch = 0;
  uint32_t droppedBytes = 0;
  uint32_t lossEpisodes = 0;
  uint32_t overflowEpisodes = 0;
};

// One synchronized boundary shared by target production code and host/TSan
// tests. Mutex is portMUX-backed on ESP32 and std::mutex on the host; lock
// scope, channel ordering, producer gate, and context retirement are identical.
template <typename Mutex, size_t OrderedCapacity, size_t ContextCapacity>
class UsbCdcSynchronizedState {
  static_assert(ContextCapacity > 0, "CDC context capacity must be nonzero");

public:
  class CallbackLease {
  public:
    CallbackLease() = default;
    ~CallbackLease() { release(); }

    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;

    CallbackLease(CallbackLease&& other) noexcept
      : _owner(other._owner), _slot(other._slot), _session(other._session) {
      other._owner = nullptr;
    }

    CallbackLease& operator=(CallbackLease&& other) noexcept {
      if (this == &other) return *this;
      release();
      _owner = other._owner;
      _slot = other._slot;
      _session = other._session;
      other._owner = nullptr;
      return *this;
    }

    explicit operator bool() const { return _owner != nullptr; }
    size_t slot() const { return _slot; }
    uint32_t session() const { return _session; }

  private:
    friend class UsbCdcSynchronizedState;

    CallbackLease(UsbCdcSynchronizedState* owner, size_t slot,
                  uint32_t session)
      : _owner(owner), _slot(slot), _session(session) {}

    void release() {
      if (_owner == nullptr) return;
      _owner->releaseCallback(_slot, _session);
      _owner = nullptr;
    }

    UsbCdcSynchronizedState* _owner = nullptr;
    size_t _slot = 0;
    uint32_t _session = 0;
  };

  class ByteCommitLease {
  public:
    ByteCommitLease() = default;
    ~ByteCommitLease() { release(); }

    ByteCommitLease(const ByteCommitLease&) = delete;
    ByteCommitLease& operator=(const ByteCommitLease&) = delete;

    ByteCommitLease(ByteCommitLease&& other) noexcept
      : _owner(other._owner), _slot(other._slot), _session(other._session) {
      other._owner = nullptr;
    }

    ByteCommitLease& operator=(ByteCommitLease&& other) noexcept {
      if (this == &other) return *this;
      release();
      _owner = other._owner;
      _slot = other._slot;
      _session = other._session;
      other._owner = nullptr;
      return *this;
    }

    explicit operator bool() const { return _owner != nullptr; }

  private:
    friend class UsbCdcSynchronizedState;

    ByteCommitLease(UsbCdcSynchronizedState* owner, size_t slot,
                    uint32_t session)
      : _owner(owner), _slot(slot), _session(session) {}

    void release() {
      if (_owner == nullptr) return;
      _owner->releaseByteCommit(_slot, _session);
      _owner = nullptr;
    }

    UsbCdcSynchronizedState* _owner = nullptr;
    size_t _slot = 0;
    uint32_t _session = 0;
  };

  void startSession(uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _gate.startSession(session);
    _terminalFact = UsbCdcTerminalFact::NONE;
    _pendingTerminalFact = UsbCdcTerminalFact::NONE;
    _terminalLossActive = false;
    _overflowLossActive = false;
    if (!_ordered.fallbackPending()) _quarantined = false;
  }

  void recordRejectedDrop(uint32_t droppedBytes) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _droppedBytes = saturatingAdd(_droppedBytes, droppedBytes);
  }

  uint32_t beginOverflowLoss(uint32_t droppedBytes) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _droppedBytes = saturatingAdd(_droppedBytes, droppedBytes);
    if (!_overflowLossActive) {
      _overflowLossActive = true;
      saturatingIncrement(_overflowEpisodes);
      saturatingIncrement(_lossEpisodes);
      saturatingIncrement(_producerEpoch);
    }
    return _producerEpoch;
  }

  uint32_t beginTerminalLoss() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!_terminalLossActive) {
      _terminalLossActive = true;
      saturatingIncrement(_lossEpisodes);
      saturatingIncrement(_producerEpoch);
    }
    return _producerEpoch;
  }

  uint32_t producerEpoch() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _producerEpoch;
  }

  UsbCdcDiagnosticsSnapshot diagnosticsSnapshot() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    UsbCdcDiagnosticsSnapshot snapshot;
    snapshot.producerEpoch = _producerEpoch;
    snapshot.droppedBytes = _droppedBytes;
    snapshot.lossEpisodes = _lossEpisodes;
    snapshot.overflowEpisodes = _overflowEpisodes;
    return snapshot;
  }

  UsbCdcConfigToken configurationToken() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _gate.configurationToken();
  }

  bool configurationTokenValid(const UsbCdcConfigToken& token) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _gate.configurationTokenValid(token);
  }

  bool commitConfiguration(const UsbCdcConfigToken& token,
                           UsbCdcOrderedPublishResult startResult) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    bool enable = startResult == UsbCdcOrderedPublishResult::QUEUED &&
                  !_quarantined;
    return _gate.commitConfiguration(token, enable);
  }

  int acquireContext(uint32_t session) {
    if (session == 0) return -1;
    UsbCdcScopedLock<Mutex> lock(_mutex);
    for (size_t offset = 0; offset < ContextCapacity; ++offset) {
      size_t index = (_nextContext + offset) % ContextCapacity;
      if (!_contexts[index].active) {
        _contexts[index].active = true;
        _contexts[index].retiring = false;
        _contexts[index].session = session;
        _contexts[index].callbacks = 0;
        _contexts[index].byteCommits = 0;
        _nextContext = (index + 1) % ContextCapacity;
        return static_cast<int>(index);
      }
    }
    return -1;
  }

  bool abandonContext(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!matchesUnlocked(slot, session) ||
        _contexts[slot].callbacks != 0 ||
        _contexts[slot].byteCommits != 0) {
      return false;
    }
    _contexts[slot] = ContextState{};
    return true;
  }

  CallbackLease acquireCallback(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!matchesUnlocked(slot, session) || _contexts[slot].retiring ||
        !_gate.sessionCurrent(session)) {
      return CallbackLease{};
    }
    _contexts[slot].callbacks++;
    return CallbackLease(this, slot, session);
  }

  CallbackLease acquireTerminalCallback(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!matchesUnlocked(slot, session) ||
        !_gate.sessionCurrent(session)) {
      return CallbackLease{};
    }
    _contexts[slot].callbacks++;
    return CallbackLease(this, slot, session);
  }

  ByteCommitLease acquireByteCommitOrRecordDrop(
      size_t slot, uint32_t session, uint32_t epoch,
      uint32_t rejectedBytes, UsbCdcByteAdmission& admission) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    admission = UsbCdcByteAdmission::REJECTED_INACTIVE;
    if (!matchesUnlocked(slot, session) || _contexts[slot].retiring ||
        _contexts[slot].callbacks <= _contexts[slot].byteCommits) {
      return ByteCommitLease{};
    }
    if (_gate.terminalSeen()) {
      admission = UsbCdcByteAdmission::REJECTED_TERMINAL;
      return ByteCommitLease{};
    }
    if (_gate.callbackEnabled(session) && !_quarantined) {
      _contexts[slot].byteCommits++;
      admission = UsbCdcByteAdmission::ADMITTED;
      return ByteCommitLease(this, slot, session);
    }
    if (_quarantined && rejectedBytes != 0 &&
        _ordered.appendDroppedBytesToResume(session, epoch, rejectedBytes)) {
      admission = UsbCdcByteAdmission::DROP_RECORDED;
    } else if (_quarantined) {
      admission = UsbCdcByteAdmission::REJECTED_QUARANTINED;
    }
    return ByteCommitLease{};
  }

  bool callbacksQuiescent(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return !matchesUnlocked(slot, session) ||
           (_contexts[slot].callbacks == 0 &&
            _contexts[slot].byteCommits == 0);
  }

  bool callbackEnabled(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return matchesUnlocked(slot, session) && !_contexts[slot].retiring &&
           _gate.callbackEnabled(session);
  }

  bool latchTerminal(uint32_t session) {
    return noteTerminalFact(session, UsbCdcTerminalFact::ERROR) !=
           UsbCdcTerminalUpdate::IGNORED;
  }

  UsbCdcTerminalUpdate noteTerminalFact(uint32_t session,
                                        UsbCdcTerminalFact fact) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!_gate.sessionCurrent(session) || fact == UsbCdcTerminalFact::NONE ||
        static_cast<uint8_t>(fact) <= static_cast<uint8_t>(_terminalFact)) {
      return UsbCdcTerminalUpdate::IGNORED;
    }
    bool first = _terminalFact == UsbCdcTerminalFact::NONE;
    _terminalFact = fact;
    if (static_cast<uint8_t>(fact) >
        static_cast<uint8_t>(_pendingTerminalFact)) {
      _pendingTerminalFact = fact;
    }
    _quarantined = true;
    _gate.noteTerminal(session);
    return first ? UsbCdcTerminalUpdate::FIRST
                 : UsbCdcTerminalUpdate::UPGRADED;
  }

  UsbCdcTerminalFact pendingTerminalFact(uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _gate.sessionCurrent(session) ? _pendingTerminalFact
                                         : UsbCdcTerminalFact::NONE;
  }

  void acknowledgeTerminalFact(uint32_t session,
                               UsbCdcTerminalFact delivered) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (_gate.sessionCurrent(session) && _pendingTerminalFact == delivered) {
      _pendingTerminalFact = UsbCdcTerminalFact::NONE;
    }
  }

  void quarantineTerminal(uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _quarantined = true;
    if (session != 0 && _gate.sessionCurrent(session)) {
      _gate.noteTerminal(session);
      if (_terminalFact == UsbCdcTerminalFact::NONE) {
        _terminalFact = UsbCdcTerminalFact::ERROR;
      }
    }
  }

  void quarantineProducer(uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _quarantined = true;
    if (session != 0) _gate.stopProducer(session);
  }

  void stopProducer(uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _gate.stopProducer(session);
  }

  UsbCdcOrderedPublishResult publish(const UsbCdcOrderedEvent& event,
                                     bool resumeProducer) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    UsbCdcOrderedPublishResult result =
      _ordered.publish(event, resumeProducer);
    if (result != UsbCdcOrderedPublishResult::QUEUED) {
      _quarantined = true;
      _gate.stopProducer(event.session);
    }
    return result;
  }

  UsbCdcOrderedPublishResult publishQuarantineBarrier(
      const UsbCdcOrderedEvent& event) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _quarantined = true;
    _gate.stopProducer(event.session);
    return _ordered.publish(event, true, true);
  }

  UsbCdcOrderedClaimResult claim(const UsbCdcOrderedCursor& cursor,
                                 UsbCdcOrderedDelivery& delivery,
                                 bool connected) {
    return claim(cursor, delivery, connected, []() {});
  }

  template <typename BeforeResume>
  UsbCdcOrderedClaimResult claim(const UsbCdcOrderedCursor& cursor,
                                 UsbCdcOrderedDelivery& delivery,
                                 bool connected,
                                 BeforeResume beforeResume) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    UsbCdcOrderedClaimResult result = _ordered.claim(cursor, delivery);
    delivery.producerResumed = false;
    if (result == UsbCdcOrderedClaimResult::CLAIMED &&
        !_ordered.fallbackPending() &&
        !_ordered.hasPendingQuarantineBarrier() &&
        (delivery.quarantineBarrier || delivery.fromFallback)) {
      _quarantined = false;
      if (delivery.resumeProducer && connected &&
          _gate.sessionCurrent(delivery.event.session) &&
          !_gate.terminalSeen()) {
        beforeResume();
        delivery.producerResumed =
          _gate.resumeProducer(delivery.event.session);
        if (delivery.producerResumed) _overflowLossActive = false;
      }
    } else if (result ==
               UsbCdcOrderedClaimResult::STALE_FALLBACK_DISCARDED) {
      _quarantined = false;
      _gate.stopProducer(_gate.session());
    }
    return result;
  }

  size_t pendingCount() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _ordered.pendingCount();
  }

  bool fallbackPending() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _ordered.fallbackPending();
  }

  bool quarantined() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return _quarantined;
  }

  void clearOrdered() {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    _ordered.clear();
    _quarantined = false;
    _overflowLossActive = false;
  }

  bool retireContext(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!matchesUnlocked(slot, session)) return false;
    _contexts[slot].retiring = true;
    _gate.stopProducer(session);
    return true;
  }

  UsbCdcCloseCompletion finishClose(size_t slot, uint32_t session,
                                    bool driverSucceeded) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (!matchesUnlocked(slot, session)) {
      return UsbCdcCloseCompletion::STALE_CONTEXT;
    }
    if (!driverSucceeded) return UsbCdcCloseCompletion::DRIVER_FAILED;
    if (_contexts[slot].callbacks != 0 ||
        _contexts[slot].byteCommits != 0) {
      return UsbCdcCloseCompletion::CALLBACKS_ACTIVE;
    }
    _contexts[slot] = ContextState{};
    return UsbCdcCloseCompletion::RELEASED;
  }

  bool contextMatches(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return matchesUnlocked(slot, session);
  }

  bool configurationContextValid(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    return matchesUnlocked(slot, session) && !_contexts[slot].retiring &&
           _gate.sessionCurrent(session) && !_gate.terminalSeen();
  }

private:
  struct ContextState {
    uint32_t session = 0;
    uint32_t callbacks = 0;
    uint32_t byteCommits = 0;
    bool active = false;
    bool retiring = false;
  };

  Mutex _mutex;
  UsbCdcOrderedChannel<OrderedCapacity> _ordered;
  UsbCdcSessionGate _gate;
  ContextState _contexts[ContextCapacity] = {};
  size_t _nextContext = 0;
  bool _quarantined = false;
  UsbCdcTerminalFact _terminalFact = UsbCdcTerminalFact::NONE;
  UsbCdcTerminalFact _pendingTerminalFact = UsbCdcTerminalFact::NONE;
  uint32_t _producerEpoch = 0;
  uint32_t _droppedBytes = 0;
  uint32_t _lossEpisodes = 0;
  uint32_t _overflowEpisodes = 0;
  bool _terminalLossActive = false;
  bool _overflowLossActive = false;

  static uint32_t saturatingAdd(uint32_t left, uint32_t right) {
    return UINT32_MAX - left < right ? UINT32_MAX : left + right;
  }

  static void saturatingIncrement(uint32_t& value) {
    if (value != UINT32_MAX) value++;
  }

  bool matchesUnlocked(size_t slot, uint32_t session) const {
    return slot < ContextCapacity && session != 0 &&
           _contexts[slot].active && _contexts[slot].session == session;
  }

  void releaseCallback(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (matchesUnlocked(slot, session) && _contexts[slot].callbacks > 0) {
      _contexts[slot].callbacks--;
    }
  }

  void releaseByteCommit(size_t slot, uint32_t session) {
    UsbCdcScopedLock<Mutex> lock(_mutex);
    if (matchesUnlocked(slot, session) &&
        _contexts[slot].byteCommits > 0) {
      _contexts[slot].byteCommits--;
    }
  }
};

#endif
