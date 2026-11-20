#pragma once
#include <cstdint>

namespace sw {
enum class NetworkPhase { Disabled, Settling, Connecting, Connected };
enum class NetworkAction { None, Begin, Disconnect };

// One owner for retries. Let an association/DHCP attempt finish before starting
// another one, and allow the asynchronous disconnect to settle first.
class NetworkSchedule {
  NetworkPhase phase_ = NetworkPhase::Disabled;
  uint32_t phaseAt_ = 0;
 public:
  NetworkPhase phase() const { return phase_; }
  void configure(uint32_t now, bool enabled) {
    phase_ = enabled ? NetworkPhase::Settling : NetworkPhase::Disabled;
    phaseAt_ = now;
  }
  NetworkAction update(uint32_t now, bool hasIp) {
    if (phase_ == NetworkPhase::Disabled) return NetworkAction::None;
    if (phase_ == NetworkPhase::Settling) {
      if (uint32_t(now - phaseAt_) < 300) return NetworkAction::None;
      phase_ = NetworkPhase::Connecting; phaseAt_ = now; return NetworkAction::Begin;
    }
    if (hasIp) { phase_ = NetworkPhase::Connected; return NetworkAction::None; }
    if (phase_ == NetworkPhase::Connected) {
      phase_ = NetworkPhase::Settling; phaseAt_ = now;
      return NetworkAction::Disconnect;
    }
    if (uint32_t(now - phaseAt_) >= 30000) {
      phase_ = NetworkPhase::Settling; phaseAt_ = now; return NetworkAction::Disconnect;
    }
    return NetworkAction::None;
  }
};
}
