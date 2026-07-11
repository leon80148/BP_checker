#ifndef NETWORK_LIFECYCLE_H
#define NETWORK_LIFECYCLE_H

#include <cstdint>

namespace bp_network {

enum class NetworkPhase : uint8_t {
  LOCKED,
  PROVISIONING_AP,
  STA_ONLY,
  RECOVERY_AP,
};

class NetworkLifecycle {
public:
  static constexpr uint32_t kRecoveryWindowMs = 10U * 60U * 1000U;

  void startLocked() {
    _phase = NetworkPhase::LOCKED;
    clearRecovery();
  }

  void startProvisioning() {
    _phase = NetworkPhase::PROVISIONING_AP;
    clearRecovery();
  }

  void startStaOnly() {
    _phase = NetworkPhase::STA_ONLY;
    clearRecovery();
  }

  bool beginRecovery(bool physicalPresence, bool hasCredentials,
                     uint32_t nowMs) {
    if (!physicalPresence || _phase == NetworkPhase::PROVISIONING_AP ||
        _phase == NetworkPhase::RECOVERY_AP) {
      return false;
    }
    _returnPhase = hasCredentials
      ? NetworkPhase::STA_ONLY : NetworkPhase::LOCKED;
    _recoveryStartedAt = nowMs;
    _phase = NetworkPhase::RECOVERY_AP;
    return true;
  }

  bool tick(uint32_t nowMs) {
    if (_phase != NetworkPhase::RECOVERY_AP ||
        static_cast<uint32_t>(nowMs - _recoveryStartedAt) <
          kRecoveryWindowMs) {
      return false;
    }
    _phase = _returnPhase;
    clearRecovery();
    return true;
  }

  void observeStaConnected(bool connected) {
    _staConnected = connected;
  }

  NetworkPhase phase() const { return _phase; }
  bool apRequired() const {
    return _phase == NetworkPhase::PROVISIONING_AP ||
           _phase == NetworkPhase::RECOVERY_AP;
  }
  bool staConnected() const { return _staConnected; }

private:
  NetworkPhase _phase = NetworkPhase::LOCKED;
  NetworkPhase _returnPhase = NetworkPhase::LOCKED;
  uint32_t _recoveryStartedAt = 0;
  bool _staConnected = false;

  void clearRecovery() {
    _returnPhase = NetworkPhase::LOCKED;
    _recoveryStartedAt = 0;
  }
};

}  // namespace bp_network

#endif
