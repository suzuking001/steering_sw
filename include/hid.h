#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <atomic>
#include "model.h"
#include "macro_runner.h"
namespace sw {
struct HidState {
  std::atomic<bool> connected{false}, encrypted{false}, keyboardSubscribed{false}, mediaSubscribed{false};
  std::atomic<bool> bootMode{false}, bootKeyboardSubscribed{false}, reportMapRead{false};
  std::atomic<bool> suspended{false};
  std::atomic<uint32_t> epoch{0};
  std::atomic<int> disconnectReason{0}, securityError{0};
  std::atomic<uint32_t> automaticReconnects{0}, serviceRefreshConfirmations{0};
  uint32_t serviceRefreshes = 0;
  uint16_t advertisingIntervalUnits = 0;
};
extern HidState hidState;
bool beginHid(const std::string& name);
bool setHidName(const std::string& name);
bool stopHid();
void setHidSuspended(bool suspended);
void maintainHid();
bool hidAutomaticRetryUsed();
bool hidAutomaticRetryExhausted();
}
