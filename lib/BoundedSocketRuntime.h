#ifndef BOUNDED_SOCKET_RUNTIME_H
#define BOUNDED_SOCKET_RUNTIME_H

#include <cstddef>
#include <cstdint>

#include "BoundedWebInput.h"

namespace bp_web {

enum class SocketReadStatus : uint8_t {
  DATA,
  WOULD_BLOCK,
  PEER_CLOSED,
  ERROR,
};

struct SocketReadResult {
  SocketReadStatus status;
  size_t length;
};

enum class SocketWriteStatus : uint8_t {
  PROGRESS,
  WOULD_BLOCK,
  ERROR,
};

struct SocketWriteResult {
  SocketWriteStatus status;
  size_t length;
};

enum class SocketShutdownStatus : uint8_t {
  COMPLETE,
  WOULD_BLOCK,
  ERROR,
};

using SocketClock = uint32_t (*)(void* context);
using SocketReceive = SocketReadResult (*)(
  void* context, int socket, uint8_t* target, size_t capacity);
using SocketSend = SocketWriteResult (*)(
  void* context, int socket, const uint8_t* bytes, size_t length);
using SocketShutdownWrite = SocketShutdownStatus (*)(
  void* context, int socket);

struct BoundedSocketOps {
  void* context = nullptr;
  SocketClock clock = nullptr;
  SocketReceive receive = nullptr;
  SocketSend send = nullptr;
  SocketShutdownWrite shutdownWrite = nullptr;
};

enum class IngressIoResult : uint8_t {
  PROGRESS,
  WOULD_BLOCK,
  PEER_CLOSED,
  ERROR,
};

enum class DrainIoResult : uint8_t {
  INACTIVE,
  WAITING,
  PEER_CLOSED,
  DEADLINE,
  ERROR,
};

class BoundedSocketRuntime {
public:
  static constexpr size_t kDrainReadBudget = 64;
  static constexpr uint32_t kDrainDeadlineMs = 1000;

  explicit BoundedSocketRuntime(BoundedSocketOps ops) : _ops(ops) {}

  BoundedSocketRuntime(const BoundedSocketRuntime&) = delete;
  BoundedSocketRuntime& operator=(const BoundedSocketRuntime&) = delete;

  bool valid() const {
    return _ops.clock != nullptr && _ops.receive != nullptr &&
           _ops.send != nullptr && _ops.shutdownWrite != nullptr;
  }

  uint32_t nowMs() const {
    return _ops.clock == nullptr ? 0 : _ops.clock(_ops.context);
  }

  IngressIoResult receiveInto(int socket, BoundedIngressBuffer& ingress) {
    if (!valid() || socket < 0 || ingress.length() != 0) {
      ingress.clear();
      return IngressIoResult::ERROR;
    }
    uint8_t* target = ingress.writableData();
    const size_t capacity = ingress.writableCapacity();
    if (target == nullptr || capacity != BoundedIngressBuffer::kCapacity) {
      ingress.clear();
      return IngressIoResult::ERROR;
    }

    const SocketReadResult result = _ops.receive(
      _ops.context, socket, target, capacity);
    if (result.status == SocketReadStatus::DATA) {
      if (result.length == 0 || result.length > capacity ||
          !ingress.commit(result.length)) {
        ingress.clear();
        return IngressIoResult::ERROR;
      }
      return IngressIoResult::PROGRESS;
    }
    ingress.clear();
    if (result.length != 0) return IngressIoResult::ERROR;
    switch (result.status) {
      case SocketReadStatus::WOULD_BLOCK:
        return IngressIoResult::WOULD_BLOCK;
      case SocketReadStatus::PEER_CLOSED:
        return IngressIoResult::PEER_CLOSED;
      case SocketReadStatus::ERROR:
      case SocketReadStatus::DATA:
        return IngressIoResult::ERROR;
    }
    return IngressIoResult::ERROR;
  }

  SocketWriteResult sendSome(int socket, const uint8_t* bytes,
                             size_t length) {
    if (!valid() || socket < 0 || bytes == nullptr || length == 0) {
      return {SocketWriteStatus::ERROR, 0};
    }
    const SocketWriteResult result = _ops.send(
      _ops.context, socket, bytes, length);
    if (result.status == SocketWriteStatus::PROGRESS) {
      if (result.length == 0 || result.length > length) {
        return {SocketWriteStatus::ERROR, 0};
      }
      return result;
    }
    if (result.length != 0) return {SocketWriteStatus::ERROR, 0};
    return result.status == SocketWriteStatus::WOULD_BLOCK
      ? result : SocketWriteResult{SocketWriteStatus::ERROR, 0};
  }

  bool beginDrain() {
    if (!valid() || _drainState != DrainState::IDLE) return false;
    _drainStartedAt = nowMs();
    _drainState = DrainState::NEED_SHUTDOWN;
    return true;
  }

  DrainIoResult pollDrain(int socket) {
    if (_drainState == DrainState::IDLE) return DrainIoResult::INACTIVE;
    const uint32_t now = nowMs();
    if (static_cast<uint32_t>(now - _drainStartedAt) >=
        kDrainDeadlineMs) {
      resetDrain();
      return DrainIoResult::DEADLINE;
    }
    if (socket < 0) {
      resetDrain();
      return DrainIoResult::ERROR;
    }

    if (_drainState == DrainState::NEED_SHUTDOWN) {
      const SocketShutdownStatus shutdown = _ops.shutdownWrite(
        _ops.context, socket);
      if (shutdown == SocketShutdownStatus::WOULD_BLOCK) {
        return DrainIoResult::WAITING;
      }
      if (shutdown != SocketShutdownStatus::COMPLETE) {
        resetDrain();
        return DrainIoResult::ERROR;
      }
      _drainState = DrainState::WAIT_PEER;
    }

    uint8_t scratch[kDrainReadBudget] = {};
    const SocketReadResult read = _ops.receive(
      _ops.context, socket, scratch, sizeof(scratch));
    secureZero(scratch, sizeof(scratch));
    if (read.status == SocketReadStatus::DATA) {
      if (read.length == 0 || read.length > sizeof(scratch)) {
        resetDrain();
        return DrainIoResult::ERROR;
      }
      return DrainIoResult::WAITING;
    }
    if (read.length != 0) {
      resetDrain();
      return DrainIoResult::ERROR;
    }
    if (read.status == SocketReadStatus::WOULD_BLOCK) {
      return DrainIoResult::WAITING;
    }
    const DrainIoResult terminal =
      read.status == SocketReadStatus::PEER_CLOSED
        ? DrainIoResult::PEER_CLOSED : DrainIoResult::ERROR;
    resetDrain();
    return terminal;
  }

  bool drainActive() const { return _drainState != DrainState::IDLE; }

  void cancelDrain() { resetDrain(); }

private:
  enum class DrainState : uint8_t {
    IDLE,
    NEED_SHUTDOWN,
    WAIT_PEER,
  };

  BoundedSocketOps _ops{};
  DrainState _drainState = DrainState::IDLE;
  uint32_t _drainStartedAt = 0;

  static void secureZero(void* target, size_t length) {
    volatile uint8_t* bytes = static_cast<volatile uint8_t*>(target);
    while (length-- != 0) *bytes++ = 0;
  }

  void resetDrain() {
    _drainState = DrainState::IDLE;
    _drainStartedAt = 0;
  }
};

}  // namespace bp_web

#endif
