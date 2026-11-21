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
  std::atomic<bool> suspended{false};
  std::atomic<uint32_t> epoch{0};
};
extern HidState hidState;
bool beginHid(const std::string& name);
bool setHidName(const std::string& name);
bool stopHid();
void setHidSuspended(bool suspended);
bool hidReady();
bool sendKey(uint8_t key, uint8_t modifiers);
bool sendMedia(uint16_t usage);
bool releaseHid();
}
