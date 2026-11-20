#pragma once
#include <cstdint>

namespace sw {
// GPIO0 is also the boot strap. Accept a long press only after observing its
// release in the running firmware; holding it during startup is not an action.
class SetupButton {
  bool armed_ = false, pressed_ = false, fired_ = false;
  uint32_t pressedAt_ = 0;
 public:
  bool update(uint32_t now, bool pressed) {
    if (!pressed) { armed_ = true; pressed_ = fired_ = false; return false; }
    if (!armed_) return false;
    if (!pressed_) { pressed_ = true; pressedAt_ = now; }
    if (!fired_ && uint32_t(now - pressedAt_) >= 3000) { fired_ = true; return true; }
    return false;
  }
};
}
