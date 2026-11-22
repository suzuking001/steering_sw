#pragma once
#include <cstdint>

namespace sw {
enum class BleRecoveryAction { None, RefreshServices, Reconnect };
enum class BleLinkPhase { Disconnected, Encrypting, AwaitingReports, Ready };

class BleReconnectSchedule {
  uint32_t waitingSince_ = 0;
  BleLinkPhase phase_ = BleLinkPhase::Disconnected;
  uint8_t attempts_ = 0;
  bool refreshed_ = false, fastAdvertising_ = true;

 public:
  static constexpr uint32_t fastAdvertisingMs = 30000;
  static constexpr uint32_t reportTimeoutMs = 10000;
  static constexpr uint32_t encryptionTimeoutMs = 35000;
  static constexpr uint32_t refreshDelayMs = 3000;
  static constexpr uint8_t maxRetries = 3;

  void disconnected() {
    fastAdvertising_ = true;
    phase_ = BleLinkPhase::Disconnected;
  }
  bool fastAdvertising() const { return fastAdvertising_; }
  void advertisingStarted() { fastAdvertising_ = false; }
  bool retryUsed() const { return attempts_ != 0; }
  bool retryExhausted() const { return attempts_ >= maxRetries; }
  void resetRecovery() {
    phase_ = BleLinkPhase::Disconnected;
    attempts_ = 0;
  }

  BleRecoveryAction update(uint32_t now, BleLinkPhase phase) {
    if (phase == BleLinkPhase::Ready) {
      resetRecovery();
      return BleRecoveryAction::None;
    }
    if (phase != phase_) {
      phase_ = phase;
      waitingSince_ = now;
      refreshed_ = false;
    }
    if (phase == BleLinkPhase::Disconnected || retryExhausted()) return BleRecoveryAction::None;
    const uint32_t elapsed = now - waitingSince_;
    if (phase == BleLinkPhase::AwaitingReports && !refreshed_) {
      if (elapsed < refreshDelayMs) return BleRecoveryAction::None;
      refreshed_ = true;
      waitingSince_ = now;
      return BleRecoveryAction::RefreshServices;
    }
    const uint32_t timeout = phase == BleLinkPhase::Encrypting ? encryptionTimeoutMs : reportTimeoutMs;
    if (elapsed < timeout + 10000U * attempts_) return BleRecoveryAction::None;
    ++attempts_;
    waitingSince_ = now;
    return BleRecoveryAction::Reconnect;
  }
};
} // namespace sw
