#pragma once
#include "model.h"
namespace sw {
bool hidReady();
bool sendKey(uint8_t key, uint8_t modifiers);
bool sendMedia(uint16_t usage);
bool releaseHid();
class MacroRunner {
  std::vector<Step> steps_;
  size_t index_ = 0, textIndex_ = 0;
  int phase_ = 0;
  uint32_t changedAt_ = 0;
 public:
  bool busy() const { return !steps_.empty(); }
  bool start(std::vector<Step> steps);
  void cancel();
  int tick(uint32_t now);
};
}
