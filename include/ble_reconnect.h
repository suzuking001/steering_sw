#pragma once
#include <cstdint>

namespace sw {
enum class BleRecoveryAction { None, RefreshServices, Reconnect };

class BleReconnectSchedule {
  uint32_t waitingSince_ = 0;
  bool waiting_ = false, refreshed_ = false, retried_ = false, fastAdvertising_ = true;

 public:
  static constexpr uint32_t fastAdvertisingMs = 30000;
  static constexpr uint32_t reportTimeoutMs = 10000;

  void disconnected() {
    fastAdvertising_ = true;
    waiting_ = false;
  }
  bool fastAdvertising() const { return fastAdvertising_; }
  void advertisingStarted() { fastAdvertising_ = false; }
  bool retryUsed() const { return retried_; }
  void resetRecovery() {
    waiting_ = false;
    refreshed_ = false;
    retried_ = false;
  }

  BleRecoveryAction update(uint32_t now, bool ready, bool awaitingReports) {
    if (ready) resetRecovery();
    if (!awaitingReports || retried_) {
      waiting_ = false;
      return BleRecoveryAction::None;
    }
    if (!waiting_) {
      waiting_ = true;
      waitingSince_ = now;
    }
    if (now - waitingSince_ < reportTimeoutMs) return BleRecoveryAction::None;
    if (!refreshed_) {
      refreshed_ = true;
      waitingSince_ = now;
      return BleRecoveryAction::RefreshServices;
    }
    retried_ = true;
    return BleRecoveryAction::Reconnect;
  }
};
} // namespace sw
