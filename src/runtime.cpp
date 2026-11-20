#include "runtime.h"
namespace sw {
Runtime runtime;
namespace {
constexpr int pins[] = {36, 39};
constexpr int accPin = 34, ledPin = 25;
struct Guard {
  SemaphoreHandle_t m;
  explicit Guard(SemaphoreHandle_t mutex) : m(mutex) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(m); }
};
int median(const int* values) { int v[5]; std::copy(values, values + 5, v); std::sort(v, v + 5); return v[2]; }
Step chord(int key, int modifiers) { Step s; s.type = "chord"; s.key = key; s.modifiers = modifiers; return s; }
}
bool Runtime::begin(const Config& cfg, const String& passcode) {
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  cfg_ = cfg; passcode_ = passcode;
  pinMode(ledPin, OUTPUT); digitalWrite(ledPin, LOW);
  analogReadResolution(12);
  for (int pin : pins) { pinMode(pin, INPUT); analogSetPinAttenuation(pin, ADC_11db); }
  pinMode(accPin, INPUT); analogSetPinAttenuation(accPin, ADC_11db);
  hidEpoch_ = hidState.epoch;
  BaseType_t created = xTaskCreatePinnedToCore([](void* arg) {
    auto self = static_cast<Runtime*>(arg);
    TickType_t wake = xTaskGetTickCount();
    for (;;) { self->tick(); vTaskDelayUntil(&wake, pdMS_TO_TICKS(5)); }
  }, "switch-input", 12288, this, 2, nullptr, 1);
  if (created != pdPASS) { vSemaphoreDelete(mutex_); mutex_ = nullptr; return false; }
  log("起動。未押下状態を確認してから操作を受け付けます");
  return true;
}
bool Runtime::hasSamples() { Guard lock(mutex_); return samples_ != 0; }
void Runtime::resetEngines() {
  for (auto& engine : engines_) engine.reset();
  for (auto& r : readings_) r.recognized = kUnknown;
  acc_.reset();
}
void Runtime::logLocked(const String& message) {
  logs_[logCursor_] = {millis(), message}; logCursor_ = (logCursor_ + 1) % 32;
  if (logCount_ < 32) ++logCount_;
}
void Runtime::log(const String& message) { Guard lock(mutex_); logLocked(message); }
Config Runtime::config() { Guard lock(mutex_); return cfg_; }
void Runtime::apply(const Config& cfg) {
  Guard lock(mutex_); runner_.cancel(); cfg_ = cfg; learning_ = Learning(); resetEngines();
  logLocked("設定を適用しました。未押下状態から再開します");
}
void Runtime::setPasscode(const String& passcode) {
  Guard lock(mutex_); runner_.cancel(); passcode_ = passcode; resetEngines();
  logLocked(passcode.isEmpty() ? "パスコードを削除しました" : "パスコードを更新しました（内容は非表示）");
}
bool Runtime::actionLocked(const std::string& action, Gesture gesture, bool physical, String& error) {
  if (action == "none") return true;
  if (!cfg_.outputEnabled) { error = "キー送信が無効です"; return false; }
  if (learning_.active) { error = "学習中は送信できません"; return false; }
  if (hidState.suspended) { error = "文字入力モード中は接続先の端末操作を停止しています"; return false; }
  if (!hidReady()) { error = "BLEが操作可能な状態ではありません"; return false; }
  if (runner_.busy()) { error = "操作実行中のため追加操作を破棄しました"; return false; }
  std::vector<Step> steps;
  if (action == "passcode") {
    if (!physical || gesture != Gesture::Long) { error = "パスコードは実ボタンの長押し専用です"; return false; }
    if (passcode_.isEmpty()) { error = "パスコードが未登録です"; return false; }
    if (!passcodeGate_.allow(millis(), gesture)) { error = "パスコードの再送間隔は10秒以上必要です"; return false; }
    if (cfg_.wakeSpace) steps.push_back(chord(44, 0));
    Step wait; wait.type = "wait"; wait.ms = cfg_.wakeDelayMs; steps.push_back(wait);
    Step text; text.type = "text"; text.text = passcode_.c_str(); steps.push_back(text);
  } else if (action == "home") steps.push_back(chord(11, 8));
  else if (action == "switch_app") steps.push_back(chord(43, 8));
  else {
    int usage = 0;
    if (action == "volume_up") usage = 0xE9;
    if (action == "volume_down") usage = 0xEA;
    if (action == "play_pause") usage = 0xCD;
    if (action == "mute") usage = 0xE2;
    if (action == "next") usage = 0xB5;
    if (action == "previous") usage = 0xB6;
    if (usage) { Step s; s.type = "media"; s.usage = usage; steps.push_back(s); }
    else {
      const auto id = action.rfind("macro:", 0) == 0 ? action.substr(6) : action;
      for (const auto& m : cfg_.macros) if (m.id == id) { steps = m.steps; break; }
    }
  }
  if (steps.empty()) { error = "操作の参照先がありません"; return false; }
  if (!runner_.start(std::move(steps))) { error = "操作を開始できません"; return false; }
  logLocked(String("送信開始: ") + action.c_str() + "（成功状態は接続先の端末で確認）"); return true;
}
bool Runtime::test(const std::string& action, String& error) {
  Guard lock(mutex_);
  if (!isAction(cfg_, action) || action == "passcode") { error = "テストできない操作です"; return false; }
  return actionLocked(action, Gesture::Short, false, error);
}
void Runtime::cancel() { Guard lock(mutex_); runner_.cancel(); resetEngines(); logLocked("操作を停止しました"); }
bool Runtime::startLearning(int channel, String& error) {
  Guard lock(mutex_);
  if (channel < 0 || channel >= kChannels) { error = "入力系統が不正です"; return false; }
  if (learning_.active) { error = "別の学習が実行中です"; return false; }
  runner_.cancel(); resetEngines(); learning_ = Learning();
  learning_.active = true; learning_.channel = channel; learning_.started = millis();
  logLocked("学習開始。400ms後から2秒間測定します"); return true;
}
void Runtime::stopLearning() {
  Guard lock(mutex_); learning_.active = false; learning_.done = false; resetEngines(); logLocked("学習を中止しました");
}
void Runtime::tick() {
  // ADC hardware is accessed only by this task, never by the HTTP task.
  int raw[kChannels], mv[kChannels];
  for (int i = 0; i < kChannels; ++i) { raw[i] = analogRead(pins[i]); mv[i] = analogReadMilliVolts(pins[i]); }
  int accMv = analogReadMilliVolts(accPin);
  Guard lock(mutex_);
  const uint32_t now = millis(); ++samples_; accMv_ = accMv;
  const bool ready = hidReady(); const uint32_t epoch = hidState.epoch;
  if (ready != ready_ || epoch != hidEpoch_) {
    runner_.cancel(); resetEngines(); hidEpoch_ = epoch; ready_ = ready;
    logLocked(ready ? "BLE操作可能。全キーを解放して未押下を待ちます" : "BLE待機中。保留操作を破棄しました");
  }
  if (int result = runner_.tick(now)) logLocked(result > 0 ? "送信手順が完了しました（接続先の端末の結果は未確認）" : "BLE送信失敗。操作を中止しました");
  bool pressed = false;
  for (int i = 0; i < kChannels; ++i) {
    auto& r = readings_[i]; const auto& ch = cfg_.channels[i];
    r.raw = raw[i]; r.mv = mv[i];
    if (!r.initialized) {
      std::fill(r.rawWindow, r.rawWindow + 5, raw[i]); std::fill(r.mvWindow, r.mvWindow + 5, mv[i]); r.initialized = true;
    }
    r.rawWindow[r.cursor] = raw[i]; r.mvWindow[r.cursor] = mv[i]; r.cursor = (r.cursor + 1) % 5;
    r.filtered = median(r.rawWindow); r.filteredMv = median(r.mvWindow);
    r.ohm = resistance(r.filteredMv, ch.supplyMv, ch.pullup, r.filtered >= 4090);
    const double value = readingValue(ch, r.filtered, r.filteredMv, r.ohm);
    r.recognized = classify(cfg_, i, value, r.recognized);
    pressed |= r.recognized >= 0;
    if (learning_.active && learning_.channel == i && uint32_t(now - learning_.started) >= 400) {
      auto& l = learning_;
      if (std::isfinite(value)) {
        if (!l.count) l.minimum = l.maximum = value;
        l.minimum = std::min(l.minimum, value); l.maximum = std::max(l.maximum, value);
        ++l.count; double delta = value - l.mean; l.mean += delta / l.count; l.m2 += delta * (value - l.mean);
      } else ++l.invalid;
    }
    if (!learning_.active && cfg_.outputEnabled && ready) {
      engines_[i].update(now, r.recognized, cfg_, [&](int b, Gesture g, const std::string& action) {
        String error;
        if (!actionLocked(action, g, true, error) && g != Gesture::Repeat)
          logLocked(String(cfg_.buttons[b].name.c_str()) + ": " + error);
      });
    } else engines_[i].reset();
  }
  if (learning_.active && uint32_t(now - learning_.started) >= 2400) {
    auto& l = learning_; l.active = false; l.done = true; resetEngines();
    if (l.count < 100 || l.invalid > 0) l.error = "有効な測定が不足しています。ADC方式または配線を確認してください";
    else if ((l.maximum - l.minimum) > std::max(cfg_.channels[l.channel].mode == "ohm" ? 5.0 : 80.0, std::abs(l.mean) * 0.12))
      l.error = "値が安定していません。ボタンを押し続けて再測定してください";
    logLocked(l.error.isEmpty() ? "学習完了。提案値はWeb画面で確認・適用してください" : l.error);
  }
  if (cfg_.accEnabled && cfg_.outputEnabled && ready && !learning_.active) {
    if (acc_.update(now, accMv_, cfg_.accThresholdMv, cfg_.accDebounceMs)) {
      runner_.cancel(); String error;
      if (!actionLocked("lock", Gesture::Short, false, error)) logLocked("ACCロック送信失敗: " + error);
    }
  } else acc_.reset();
  digitalWrite(ledPin, pressed || learning_.active || runner_.busy() ? HIGH : LOW);
}
void Runtime::status(JsonObject o) {
  Guard lock(mutex_);
  o["uptimeMs"] = millis(); o["samples"] = samples_; o["outputEnabled"] = cfg_.outputEnabled;
  o["bleConnected"] = hidState.connected.load(); o["bleReady"] = hidReady(); o["busy"] = runner_.busy();
  o["bleSuspended"] = hidState.suspended.load();
  o["passcodeSet"] = !passcode_.isEmpty(); o["accMv"] = accMv_;
  auto rs = o["readings"].to<JsonArray>();
  for (int i = 0; i < kChannels; ++i) {
    const auto& r = readings_[i]; auto a = rs.add<JsonObject>();
    a["name"] = cfg_.channels[i].name; a["mode"] = cfg_.channels[i].mode; a["pin"] = pins[i];
    a["raw"] = r.raw; a["adc"] = r.filtered; a["mv"] = r.filteredMv;
    if (std::isfinite(r.ohm)) a["ohm"] = r.ohm; else a["ohm"] = nullptr;
    if (r.recognized >= 0) { a["button"] = cfg_.buttons[r.recognized].name; a["buttonId"] = cfg_.buttons[r.recognized].id; }
    else { a["button"] = r.recognized == kIdle ? "未押下" : "範囲外 / 測定不可"; a["buttonId"] = nullptr; }
  }
  auto l = o["learning"].to<JsonObject>();
  l["active"] = learning_.active; l["done"] = learning_.done; l["channel"] = learning_.channel;
  l["count"] = learning_.count; l["error"] = learning_.error;
  l["remainingMs"] = learning_.active ? 2400 - std::min(uint32_t(millis() - learning_.started), uint32_t(2400)) : 0;
  l["center"] = learning_.mean; l["minimum"] = learning_.minimum; l["maximum"] = learning_.maximum;
  double margin = cfg_.channels[learning_.channel].mode != "ohm" ? 10 : std::max(2.0, std::abs(learning_.mean) * 0.03);
  l["tolerance"] = std::max(learning_.maximum - learning_.mean, learning_.mean - learning_.minimum) + margin;
  auto logs = o["logs"].to<JsonArray>();
  for (size_t j = 0; j < logCount_; ++j) {
    const auto& entry = logs_[(logCursor_ + 32 - logCount_ + j) % 32];
    auto a = logs.add<JsonObject>(); a["at"] = entry.at; a["message"] = entry.message;
  }
}
} // namespace sw
