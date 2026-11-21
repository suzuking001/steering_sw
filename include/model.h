#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sw {
constexpr int kChannels = 2, kMaxButtons = 32, kVersion = 1;
constexpr int kIdle = -1, kUnknown = -2;
constexpr size_t kMaxBleNameBytes = 29;
struct BleSettings {
  std::string name = "Steering Switch";
  bool enabled = true;
};
struct Channel {
  std::string name = "KEY", mode = "adc";
  double pullup = 330, supplyMv = 3300;
  double idleCenter = 3890, idleTolerance = 205, hysteresis = 25;
};
struct Button {
  std::string id, name;
  int channel = 0;
  double center = 0, tolerance = 100;
  std::string shortAction = "none", doubleAction = "none", longAction = "none";
};
struct Step {
  std::string type = "wait", text;
  int key = 0, modifiers = 0, usage = 0;
  uint32_t ms = 100;
};
struct Macro { std::string id, name; std::vector<Step> steps; };
struct Config {
  int version = kVersion;
  std::string name = "ステアリングスイッチ", ssid;
  bool outputEnabled = false;
  BleSettings ble;
  Channel channels[kChannels];
  std::vector<Button> buttons;
  std::vector<Macro> macros;
  uint32_t debounceMs = 30, longMs = 800, doubleMs = 300;
  uint32_t repeatDelayMs = 450, repeatMs = 100, wakeDelayMs = 700;
  bool wakeSpace = true, accEnabled = false;
  int accThresholdMv = 1000;
  uint32_t accDebounceMs = 1000;
};
inline double resistance(double mv, double supplyMv, double pullup, bool saturated = false) {
  if (saturated || !std::isfinite(mv) || mv < 0 || supplyMv <= 0 ||
      pullup <= 0 || mv >= supplyMv - 20) return NAN;
  return pullup * mv / (supplyMv - mv);
}
inline bool inside(double value, double center, double tolerance) {
  return std::isfinite(value) && std::abs(value - center) <= tolerance;
}
inline bool overlap(double a, double at, double b, double bt) { return std::abs(a - b) <= at + bt; }
inline double readingLimit(const Channel& channel) {
  if (channel.mode == "ohm") return 1000000;
  return channel.mode == "mv" ? 3600 : 4095;
}
inline double readingValue(const Channel& channel, double adc, double mv, double ohm) {
  if (channel.mode == "ohm") return ohm;
  return channel.mode == "mv" ? mv : adc;
}
// Hysteresis may retain a button into a gap, never into another explicit range.
inline int classify(const Config& cfg, int ch, double value, int previous = kUnknown) {
  if (!std::isfinite(value)) return kUnknown;
  const auto& c = cfg.channels[ch];
  if (inside(value, c.idleCenter, c.idleTolerance)) return kIdle;
  for (size_t i = 0; i < cfg.buttons.size(); ++i) {
    const auto& b = cfg.buttons[i];
    if (b.channel == ch && inside(value, b.center, b.tolerance)) return int(i);
  }
  if (previous >= 0 && size_t(previous) < cfg.buttons.size()) {
    const auto& b = cfg.buttons[previous];
    if (b.channel == ch && inside(value, b.center, b.tolerance + c.hysteresis)) return previous;
  }
  return kUnknown;
}
inline bool isVolume(const std::string& a) { return a == "volume_up" || a == "volume_down"; }
inline bool isAction(const Config& cfg, const std::string& a) {
  const char* known[] = {"none", "volume_up", "volume_down", "play_pause", "mute", "next", "previous",
    "home", "switch_app", "maps", "youtube", "siri", "lock", "app_list", "passcode"};
  for (auto s : known) if (a == s) return true;
  for (const auto& m : cfg.macros) if (a == "macro:" + m.id) return true;
  return false;
}
inline bool validId(const std::string& s) {
  if (s.empty() || s.size() > 24) return false;
  for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
inline std::string validate(const Config& c) {
  if (c.version != kVersion) return "設定バージョンが対応していません";
  if (c.name.empty() || c.name.size() > 96 || c.ssid.size() > 32) return "設定名またはSSIDの長さが不正です";
  if (c.ble.name.size() > kMaxBleNameBytes || c.ble.name.find_first_not_of(' ') == std::string::npos ||
      std::any_of(c.ble.name.begin(), c.ble.name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) {
    return "Bluetooth名は空白だけの名前と制御文字を除き29バイトまでです";
  }
  if (c.debounceMs < 5 || c.debounceMs > 250 || c.longMs < 400 || c.longMs > 5000 ||
      c.doubleMs < 100 || c.doubleMs > 600 || c.repeatDelayMs < 200 || c.repeatDelayMs > 3000 ||
      c.repeatMs < 50 || c.repeatMs > 1000 || c.wakeDelayMs > 10000 ||
      c.accThresholdMv < 100 || c.accThresholdMv > 3000 || c.accDebounceMs < 100 || c.accDebounceMs > 10000)
    return "操作時間またはACCしきい値が範囲外です";
  for (const auto& ch : c.channels) {
    if (ch.name.empty() || ch.name.size() > 48 || (ch.mode != "adc" && ch.mode != "ohm" && ch.mode != "mv")) return "入力系統の設定が不正です";
    const double limit = readingLimit(ch);
    if (!std::isfinite(ch.pullup) || ch.pullup < 10 || ch.pullup > 1000000 ||
        !std::isfinite(ch.supplyMv) || ch.supplyMv < 1000 || ch.supplyMv > 3600 ||
        !std::isfinite(ch.idleCenter) || ch.idleCenter < 0 || ch.idleCenter > limit ||
        !std::isfinite(ch.idleTolerance) || ch.idleTolerance <= 0 || ch.idleTolerance > limit ||
        !std::isfinite(ch.hysteresis) || ch.hysteresis < 0 || ch.hysteresis > limit / 4)
      return "回路定数・未押下範囲・ヒステリシスが不正です";
  }
  if (c.buttons.size() > kMaxButtons || c.macros.size() > 16) return "ボタンまたはマクロの登録数が上限を超えています";
  for (size_t i = 0; i < c.macros.size(); ++i) {
    const auto& m = c.macros[i];
    if (!validId(m.id) || m.name.empty() || m.name.size() > 96 || m.steps.empty() || m.steps.size() > 32) return "マクロの形式が不正です";
    for (size_t j = 0; j < i; ++j) if (m.id == c.macros[j].id) return "マクロIDが重複しています";
    uint32_t total = 0;
    for (const auto& s : m.steps) {
      if (s.type == "wait") {
        if (s.ms > 10000) return "待ち時間は10000ms以内にしてください";
        total += s.ms;
      } else if (s.type == "chord") {
        if (s.key < 4 || s.key > 115 || s.modifiers < 0 || s.modifiers > 255) return "HIDキーコードまたは修飾キーが不正です";
        total += 60;
      } else if (s.type == "media") {
        if (s.usage != 0xE9 && s.usage != 0xEA && s.usage != 0xE2 && s.usage != 0xCD && s.usage != 0xB5 && s.usage != 0xB6 && s.usage != 0xB7)
          return "未対応のメディアキーです";
        total += 60;
      } else if (s.type == "text") {
        if (s.text.empty() || s.text.size() > 128) return "文字列は1〜128文字にしてください";
        for (unsigned char ch : s.text) if (ch < 32 || ch > 126) return "文字列入力は半角英数・ASCII記号に対応します";
        total += s.text.size() * 60;
      } else return "未対応のマクロステップです";
    }
    if (total > 30000) return "マクロの合計実行時間は30秒以内にしてください";
  }
  int counts[kChannels] = {};
  for (size_t i = 0; i < c.buttons.size(); ++i) {
    const auto& b = c.buttons[i];
    if (!validId(b.id) || b.name.empty() || b.name.size() > 96 || b.channel < 0 || b.channel >= kChannels) return "ボタンの形式が不正です";
    const auto& ch = c.channels[b.channel];
    const double limit = readingLimit(ch);
    if (++counts[b.channel] > 16) return "1系統のボタン数は16個までです";
    if (!std::isfinite(b.center) || b.center < 0 || b.center > limit || !std::isfinite(b.tolerance) || b.tolerance <= 0 || b.tolerance > limit)
      return "ボタン中心値または許容幅が範囲外です";
    if (!isAction(c, b.shortAction) || !isAction(c, b.doubleAction) || !isAction(c, b.longAction)) return "操作の参照先がありません";
    if (b.shortAction == "passcode" || b.doubleAction == "passcode") return "パスコードは長押しにのみ割り当てできます";
    if (isVolume(b.shortAction) && (b.doubleAction != "none" || b.longAction != "none")) return "音量ボタンのダブル・長押しは未割り当てにしてください";
    if (overlap(b.center, b.tolerance, ch.idleCenter, ch.idleTolerance)) return "未押下範囲とボタンの判定範囲が重複しています: " + b.name;
    for (size_t j = 0; j < i; ++j) {
      const auto& other = c.buttons[j];
      if (b.id == other.id) return "ボタンIDが重複しています";
      if (b.channel == other.channel && overlap(b.center, b.tolerance, other.center, other.tolerance)) return "ボタン判定範囲が重複しています: " + other.name + " / " + b.name;
    }
  }
  return "";
}
enum class Gesture { Short, Double, Long, Repeat };
using Emit = std::function<void(int, Gesture, const std::string&)>;
// All time comparisons use unsigned subtraction so millis() wrap is harmless.
class GestureEngine {
  int candidate_ = kUnknown, stable_ = kUnknown, active_ = kIdle, pending_ = kIdle;
  uint32_t candidateAt_ = 0, pressedAt_ = 0, releasedAt_ = 0, lastRepeat_ = 0;
  bool armed_ = false, longSent_ = false, repeated_ = false;
 public:
  void reset() { *this = GestureEngine(); }
  int stable() const { return stable_; }
  void update(uint32_t now, int candidate, const Config& cfg, const Emit& emit) {
    // Defer a pending single tap while its second press is held.
    if (pending_ >= 0 && active_ != pending_ && uint32_t(now - releasedAt_) >= cfg.doubleMs) {
      emit(pending_, Gesture::Short, cfg.buttons[pending_].shortAction); pending_ = kIdle;
    }
    if (candidate != candidate_) { candidate_ = candidate; candidateAt_ = now; }
    if (candidate_ != stable_ && uint32_t(now - candidateAt_) >= cfg.debounceMs) {
      stable_ = candidate_;
      if (stable_ == kUnknown) {
        // An invalid measurement is not a confirmed release. Cancel everything
        // and require a neutral state before accepting another press.
        active_ = pending_ = kIdle; armed_ = false; return;
      }
      if (active_ >= 0) {
        const int old = active_;
        const auto& b = cfg.buttons[old];
        active_ = kIdle;
        if (!longSent_ && !isVolume(b.shortAction)) {
          if (pending_ == old) {
            emit(old, Gesture::Double, b.doubleAction); pending_ = kIdle;
          } else if (b.doubleAction != "none") {
            pending_ = old; releasedAt_ = now;
          } else emit(old, Gesture::Short, b.shortAction);
        }
      }
      if (stable_ == kIdle) armed_ = true;
      if (stable_ >= 0 && armed_) {
        if (pending_ >= 0 && pending_ != stable_) {
          emit(pending_, Gesture::Short, cfg.buttons[pending_].shortAction); pending_ = kIdle;
        }
        active_ = stable_; armed_ = false; pressedAt_ = now; lastRepeat_ = now;
        longSent_ = repeated_ = false;
        const auto& b = cfg.buttons[active_];
        if (isVolume(b.shortAction)) emit(active_, Gesture::Short, b.shortAction);
      }
    }
    if (active_ >= 0) {
      const auto& b = cfg.buttons[active_];
      if (candidate != active_) return;
      if (isVolume(b.shortAction)) {
        if (uint32_t(now - pressedAt_) >= cfg.repeatDelayMs &&
            (!repeated_ || uint32_t(now - lastRepeat_) >= cfg.repeatMs)) {
          repeated_ = true; lastRepeat_ = now; emit(active_, Gesture::Repeat, b.shortAction);
        }
      } else if (!longSent_ && b.longAction != "none" && uint32_t(now - pressedAt_) >= cfg.longMs) {
        pending_ = kIdle; longSent_ = true; emit(active_, Gesture::Long, b.longAction);
      }
    }
  }
};
class PasscodeGate {
  bool used_ = false;
  uint32_t last_ = 0;
 public:
  bool allow(uint32_t now, Gesture gesture) {
    if (gesture != Gesture::Long || (used_ && uint32_t(now - last_) < 10000)) return false;
    used_ = true; last_ = now; return true;
  }
};
class AccDetector {
  bool seenHigh_ = false, tracking_ = false, sent_ = false;
  uint32_t lowAt_ = 0;
 public:
  void reset() { *this = AccDetector(); }
  bool update(uint32_t now, int mv, int threshold, uint32_t debounce) {
    if (mv >= threshold + 100) { seenHigh_ = true; tracking_ = sent_ = false; return false; }
    if (mv >= threshold || !seenHigh_) { tracking_ = false; return false; }
    if (!tracking_) { tracking_ = true; lowAt_ = now; }
    if (!sent_ && uint32_t(now - lowAt_) >= debounce) { sent_ = true; return true; }
    return false;
  }
};
} // namespace sw
