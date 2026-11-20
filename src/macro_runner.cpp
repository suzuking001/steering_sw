#include "macro_runner.h"
#include "keys.h"
namespace sw {
bool MacroRunner::start(std::vector<Step> steps) {
  if (busy() || steps.empty() || !hidReady()) return false;
  steps_ = std::move(steps); index_ = textIndex_ = 0; phase_ = 0; return true;
}
void MacroRunner::cancel() {
  releaseHid();
  for (auto& s : steps_) if (!s.text.empty()) std::fill(s.text.begin(), s.text.end(), '\0');
  steps_.clear(); index_ = textIndex_ = 0; phase_ = 0;
}
int MacroRunner::tick(uint32_t now) {
  if (!busy()) return 0;
  if (!hidReady()) { cancel(); return -1; }
  if (index_ >= steps_.size()) { cancel(); return 1; }
  auto& s = steps_[index_];
  if (phase_ == 1 && uint32_t(now - changedAt_) >= 30) {
    if (!releaseHid()) { cancel(); return -1; }
    phase_ = 2; changedAt_ = now;
  } else if (phase_ == 2 && uint32_t(now - changedAt_) >= 30) {
    if (s.type == "text" && ++textIndex_ < s.text.size()) phase_ = 0;
    else { ++index_; textIndex_ = 0; phase_ = 0; }
  } else if (phase_ == 3 && uint32_t(now - changedAt_) >= s.ms) {
    ++index_; phase_ = 0;
  } else if (phase_ == 0) {
    changedAt_ = now;
    if (s.type == "wait") phase_ = 3;
    else {
      bool ok = false;
      if (s.type == "chord") ok = sendKey(s.key, s.modifiers);
      if (s.type == "media") ok = sendMedia(s.usage);
      if (s.type == "text" && textIndex_ < s.text.size()) {
        uint8_t key = 0, mod = 0;
        ok = asciiKey(s.text[textIndex_], key, mod) && sendKey(key, mod);
      }
      if (!ok) { cancel(); return -1; }
      phase_ = 1;
    }
  }
  return 0;
}
}
