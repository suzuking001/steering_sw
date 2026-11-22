#include "runtime.h"
#include <esp_adc/adc_cali_scheme.h>
namespace sw {
Runtime runtime;
namespace {
constexpr int pins[kChannels + 1] = {36, 39, 34};
constexpr int ledPin = 25;
constexpr uint32_t historyIntervalMs = 25;
struct Guard {
  SemaphoreHandle_t m;
  explicit Guard(SemaphoreHandle_t mutex) : m(mutex) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(m); }
};
int median(const int* values) { int v[5]; std::copy(values, values + 5, v); std::sort(v, v + 5); return v[2]; }
Step chord(int key, int modifiers) { Step s; s.type = "chord"; s.key = key; s.modifiers = modifiers; return s; }
}
bool Runtime::beginAdc() {
  adc_oneshot_unit_init_cfg_t unit = {};
  unit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &adc_) != ESP_OK) return false;
  const adc_oneshot_chan_cfg_t channelConfig = {ADC_ATTEN_DB_12, ADC_BITWIDTH_12};
  for (int i = 0; i <= kChannels; ++i) {
    adc_unit_t pinUnit;
    if (adc_oneshot_io_to_channel(pins[i], &pinUnit, &adcChannels_[i]) != ESP_OK || pinUnit != ADC_UNIT_1 ||
        adc_oneshot_config_channel(adc_, adcChannels_[i], &channelConfig) != ESP_OK) {
      adc_oneshot_del_unit(adc_);
      adc_ = nullptr;
      return false;
    }
  }
  const adc_cali_line_fitting_config_t cal = {ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_BITWIDTH_12, 1100};
  if (adc_cali_create_scheme_line_fitting(&cal, &calibration_) != ESP_OK) {
    log("ADCの電圧校正を開始できません。ADC値でのみ判定できます");
  }
  return true;
}
int Runtime::millivolts(int raw) const {
  int mv = 0;
  return raw >= 0 && calibration_ && adc_cali_raw_to_voltage(calibration_, raw, &mv) == ESP_OK ? mv : -1;
}
bool Runtime::begin(const Config& cfg, const String& passcode) {
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  cfg_ = cfg; passcode_ = passcode;
  pinMode(ledPin, OUTPUT); digitalWrite(ledPin, LOW);
  if (!beginAdc()) { vSemaphoreDelete(mutex_); mutex_ = nullptr; return false; }
  hidEpoch_ = hidState.epoch;
  BaseType_t created = xTaskCreatePinnedToCore([](void* arg) {
    auto self = static_cast<Runtime*>(arg);
    TickType_t wake = xTaskGetTickCount();
    for (;;) { self->tick(); vTaskDelayUntil(&wake, pdMS_TO_TICKS(5)); }
  }, "switch-input", 12288, this, 2, nullptr, 1);
  if (created != pdPASS) {
    if (calibration_) adc_cali_delete_scheme_line_fitting(calibration_);
    adc_oneshot_del_unit(adc_);
    vSemaphoreDelete(mutex_);
    return false;
  }
  log("起動。未押下状態を確認してから操作を受け付けます");
  return true;
}
bool Runtime::hasSamples() {
  Guard lock(mutex_);
  return readings_[0].raw >= 0 && readings_[1].raw >= 0;
}
bool Runtime::stopHidForFirmware() {
  Guard lock(mutex_);
  runner_.cancel();
  resetEngines();
  return stopHid();
}
bool Runtime::resumeHidAfterFirmware() {
  Guard lock(mutex_);
  return beginHid(cfg_.ble.name);
}
bool Runtime::reconnectHid() {
  Guard lock(mutex_);
  runner_.cancel();
  resetEngines();
  setHidSuspended(true);
  const bool named = setHidName(cfg_.ble.name);
  setHidSuspended(!cfg_.ble.enabled);
  return named;
}
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
void Runtime::setFunctionOrder(std::string order) {
  Guard lock(mutex_);
  cfg_.functionOrder = std::move(order);
}
void Runtime::setPasscode(const String& passcode) {
  Guard lock(mutex_); runner_.cancel(); passcode_ = passcode; resetEngines();
  logLocked(passcode.isEmpty() ? "パスコードを削除しました" : "パスコードを更新しました（内容は非表示）");
}
void Runtime::setInputMode(bool enabled) {
  Guard lock(mutex_);
  if (inputModePaused_ == enabled) return;
  inputModePaused_ = enabled;
  if (enabled) runner_.cancel();
  resetEngines();
  logLocked(enabled ? "設定画面の操作中はキー送信を一時停止します" : "キー送信の一時停止を解除しました");
}
bool Runtime::canSendLocked(String& error) {
  if (!cfg_.outputEnabled) { error = "キー送信が無効です"; return false; }
  if (!cfg_.ble.enabled) { error = "Bluetoothが無効です"; return false; }
  if (learning_.active) { error = "学習中は送信できません"; return false; }
  if (inputModePaused_) { error = "キー送信が一時停止中です。開いている設定画面で入力や学習を終了してください"; return false; }
  if (!hidReady()) { error = "BLEが操作可能な状態ではありません"; return false; }
  if (runner_.busy()) { error = "操作実行中のため追加操作を破棄しました"; return false; }
  return true;
}
bool Runtime::startStepsLocked(std::vector<Step>&& steps, const char* label, String& error) {
  if (!runner_.start(std::move(steps))) { error = "この操作に必要なキー通知が準備中です"; return false; }
  logLocked(String("送信開始: ") + label);
  return true;
}
bool Runtime::actionLocked(const std::string& action, Gesture gesture, bool physical, String& error) {
  if (action == "none") return true;
  if (!canSendLocked(error)) return false;
  std::vector<Step> steps;
  if (action == "passcode" && (!physical || gesture != Gesture::Long)) {
    error = "パスコードは実ボタンの長押し専用です"; return false;
  }
  if (action == "test_digits") {
    Step text; text.type = "text"; text.text = "0123456789"; steps.push_back(text);
  } else if (const auto* function = findAction(cfg_, action)) {
    for (const auto& step : function->steps) {
      if (step.type != "passcode") { steps.push_back(step); continue; }
      if (passcode_.isEmpty()) { error = "パスコードが未登録です"; return false; }
      if (!passcodeGate_.allow(millis(), gesture)) { error = "パスコードの再送間隔は10秒以上必要です"; return false; }
      if (cfg_.wakeSpace) steps.push_back(chord(44, 0));
      Step wait; wait.type = "wait"; wait.ms = cfg_.wakeDelayMs; steps.push_back(wait);
      Step text; text.type = "text"; text.text = passcode_.c_str(); steps.push_back(std::move(text));
    }
  }
  if (steps.empty()) { error = "操作の参照先がありません"; return false; }
  return startStepsLocked(std::move(steps), action.c_str(), error);
}
bool Runtime::test(const std::string& action, String& error) {
  Guard lock(mutex_);
  if (action != "test_digits" && (!isAction(cfg_, action) || action == "passcode")) {
    error = "テストできない操作です"; return false;
  }
  return actionLocked(action, Gesture::Short, false, error);
}
bool Runtime::testMacro(std::vector<Step>&& steps, String& error) {
  Guard lock(mutex_);
  return canSendLocked(error) && startStepsLocked(std::move(steps), "マクロのテスト", error);
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
  int raw[kChannels + 1];
  for (int i = 0; i <= kChannels; ++i) {
    if (adc_oneshot_read(adc_, adcChannels_[i], &raw[i]) != ESP_OK) raw[i] = -1;
  }
  const int accMv = millivolts(raw[kChannels]);
  const uint32_t acquiredAt = millis();
  Guard lock(mutex_);
  const uint32_t now = millis();
  ++samples_; sampled_ = true; sampleAt_ = acquiredAt; accMv_ = accMv;
  const bool ready = cfg_.ble.enabled && hidReady(); const uint32_t epoch = hidState.epoch;
  if (ready != ready_ || epoch != hidEpoch_) {
    runner_.cancel(); resetEngines(); hidEpoch_ = epoch; ready_ = ready;
    logLocked(ready ? "BLE操作可能。全キーを解放して未押下を待ちます" : "BLE待機中。保留操作を破棄しました");
  }
  if (int result = runner_.tick(now)) logLocked(result > 0 ? "送信手順が完了しました（接続先の端末の結果は未確認）" : "BLE送信失敗。操作を中止しました");
  const bool canSend = ready && cfg_.outputEnabled && !inputModePaused_ && !learning_.active;
  bool pressed = false;
  for (int i = 0; i < kChannels; ++i) {
    auto& r = readings_[i]; const auto& ch = cfg_.channels[i];
    r.raw = raw[i];
    if (r.raw >= 0) {
      if (!r.initialized) {
        std::fill(r.rawWindow, r.rawWindow + 5, r.raw);
        r.initialized = true;
      }
      r.rawWindow[r.cursor] = r.raw;
      r.cursor = (r.cursor + 1) % 5;
      r.filtered = median(r.rawWindow);
      r.filteredMv = millivolts(r.filtered);
      r.lastValidAt = acquiredAt;
    } else {
      r.filtered = r.filteredMv = -1;
      if (uint32_t(acquiredAt - r.lastValidAt) >= 25) r.initialized = false;
    }
    r.ohm = resistance(r.filteredMv, ch.supplyMv, ch.pullup, r.filtered >= 4090);
    const double value = readingValue(ch, r.filtered < 0 ? NAN : r.filtered,
                                      r.filteredMv < 0 ? NAN : r.filteredMv, r.ohm);
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
    if (canSend) {
      engines_[i].update(now, r.recognized, cfg_, [&](int b, Gesture g, const std::string& action) {
        String error;
        if (!actionLocked(action, g, true, error) && g != Gesture::Repeat)
          logLocked(String(cfg_.buttons[b].name.c_str()) + ": " + error);
      });
    } else engines_[i].reset();
  }
  if (!historyCount_ || uint32_t(acquiredAt - history_[(historyNext_ + kHistorySize - 1) % kHistorySize].at) >= historyIntervalMs) {
    auto& sample = history_[historyNext_];
    sample.at = acquiredAt;
    for (int i = 0; i < kChannels; ++i) sample.mv[i] = readings_[i].filteredMv;
    historyNext_ = (historyNext_ + 1) % kHistorySize;
    if (historyCount_ < kHistorySize) ++historyCount_;
  }
  if (learning_.active && uint32_t(now - learning_.started) >= 2400) {
    auto& l = learning_; l.active = false; l.done = true; resetEngines();
    if (l.count < 100 || l.invalid > 0) l.error = "有効な測定が不足しています。ADC方式または配線を確認してください";
    else if ((l.maximum - l.minimum) > std::max(cfg_.channels[l.channel].mode == "ohm" ? 5.0 : 80.0, std::abs(l.mean) * 0.12))
      l.error = "値が安定していません。ボタンを押し続けて再測定してください";
    logLocked(l.error.isEmpty() ? "学習完了。提案値はWeb画面で確認・適用してください" : l.error);
  }
  if (cfg_.accEnabled && canSend && accMv_ >= 0) {
    if (acc_.update(now, accMv_, cfg_.accThresholdMv, cfg_.accDebounceMs)) {
      runner_.cancel(); String error;
      if (!actionLocked("lock", Gesture::Short, false, error)) logLocked("ACCロック送信失敗: " + error);
    }
  } else acc_.reset();
  digitalWrite(ledPin, pressed || learning_.active || runner_.busy() ? HIGH : LOW);
}
void Runtime::status(JsonObject o, bool includeLogs) {
  bool advertising = false;
  int bonds = -1;
  if (NimBLEDevice::isInitialized()) {
    advertising = NimBLEDevice::getAdvertising()->isAdvertising();
    bonds = NimBLEDevice::getNumBonds();
  }
  Guard lock(mutex_);
  o["outputEnabled"] = cfg_.outputEnabled;
  o["bleConnected"] = hidState.connected.load(); o["bleReady"] = hidReady(); o["busy"] = runner_.busy();
  o["bleSuspended"] = hidState.suspended.load();
  auto ble = o["ble"].to<JsonObject>();
  ble["name"] = cfg_.ble.name;
  ble["enabled"] = cfg_.ble.enabled;
  ble["encrypted"] = hidState.encrypted.load();
  ble["keyboardReady"] = hidKeyboardReady();
  ble["mediaReady"] = hidMediaReady();
  ble["protocolMode"] = hidState.bootMode ? "boot" : "report";
  ble["reportMapRead"] = hidState.reportMapRead.load();
  ble["securityError"] = hidState.securityError.load();
  ble["disconnectReason"] = hidState.disconnectReason.load();
  ble["automaticReconnects"] = hidState.automaticReconnects.load();
  ble["serviceRefreshes"] = hidState.serviceRefreshes;
  ble["serviceRefreshConfirmations"] = hidState.serviceRefreshConfirmations.load();
  ble["automaticRetryUsed"] = hidAutomaticRetryUsed();
  ble["automaticRetryExhausted"] = hidAutomaticRetryExhausted();
  ble["advertising"] = advertising;
  if (advertising) ble["advertisingIntervalMs"] = hidState.advertisingIntervalUnits * 0.625;
  if (bonds >= 0) ble["bonds"] = bonds; else ble["bonds"] = nullptr;
  o["passcodeSet"] = !passcode_.isEmpty();
  if (accMv_ >= 0) o["accMv"] = accMv_; else o["accMv"] = nullptr;
  auto l = o["learning"].to<JsonObject>();
  l["active"] = learning_.active; l["done"] = learning_.done; l["channel"] = learning_.channel;
  l["count"] = learning_.count; l["error"] = learning_.error;
  l["remainingMs"] = learning_.active ? 2400 - std::min(uint32_t(millis() - learning_.started), uint32_t(2400)) : 0;
  l["center"] = learning_.mean; l["minimum"] = learning_.minimum; l["maximum"] = learning_.maximum;
  double margin = cfg_.channels[learning_.channel].mode != "ohm" ? 10 : std::max(2.0, std::abs(learning_.mean) * 0.03);
  l["tolerance"] = std::max(learning_.maximum - learning_.mean, learning_.mean - learning_.minimum) + margin;
  if (includeLogs) {
    auto logs = o["logs"].to<JsonArray>();
    for (size_t j = 0; j < logCount_; ++j) {
      const auto& entry = logs_[(logCursor_ + 32 - logCount_ + j) % 32];
      auto a = logs.add<JsonObject>(); a["at"] = entry.at; a["message"] = entry.message;
    }
  }
}
void Runtime::writeReadingsLocked(JsonObject o) {
  o["samples"] = samples_; o["inputModePaused"] = inputModePaused_;
  if (sampled_) o["sampleAtMs"] = sampleAt_; else o["sampleAtMs"] = nullptr;
  auto rs = o["readings"].to<JsonArray>();
  for (int i = 0; i < kChannels; ++i) {
    const auto& r = readings_[i]; const auto& ch = cfg_.channels[i];
    auto a = rs.add<JsonObject>();
    a["name"] = ch.name; a["mode"] = ch.mode; a["pin"] = pins[i];
    if (r.raw >= 0) a["raw"] = r.raw; else a["raw"] = nullptr;
    if (r.filtered >= 0) a["adc"] = r.filtered; else a["adc"] = nullptr;
    if (r.filteredMv >= 0) a["mv"] = r.filteredMv; else a["mv"] = nullptr;
    const bool ohmValid = std::isfinite(r.ohm);
    if (ohmValid) a["ohm"] = r.ohm; else a["ohm"] = nullptr;
    a["ohmState"] = ohmValid ? "ok" : !sampled_ ? "waiting" : r.raw < 0 ? "adc_error" :
                     r.filtered >= 4090 ? "adc_saturated" : r.filteredMv < 0 ? "voltage_unavailable" : "supply_limit";
    if (r.recognized >= 0) {
      a["button"] = cfg_.buttons[r.recognized].name;
      a["buttonId"] = cfg_.buttons[r.recognized].id;
    } else {
      const bool valid = ch.mode == "ohm" ? ohmValid : ch.mode == "mv" ? r.filteredMv >= 0 : r.filtered >= 0;
      a["button"] = r.recognized == kIdle ? "未押下" : !sampled_ ? "測定待ち" : valid ? "判定範囲外" : "測定不可";
      a["buttonId"] = nullptr;
    }
  }
}
void Runtime::liveReadings(JsonObject o, bool includeHistory, uint32_t after) {
  VoltageSample points[kHistorySize];
  size_t count = 0;
  {
    Guard lock(mutex_);
    o["uptimeMs"] = millis();
    writeReadingsLocked(o);
    if (includeHistory) {
      const size_t first = (historyNext_ + kHistorySize - historyCount_) % kHistorySize;
      for (size_t i = 0; i < historyCount_; ++i) {
        const auto& p = history_[(first + i) % kHistorySize];
        if (int32_t(p.at - after) > 0) points[count++] = p;
      }
    }
  }
  auto history = o["history"].to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    auto point = history.add<JsonArray>();
    point.add(points[i].at);
    for (int mv : points[i].mv) {
      if (mv >= 0) point.add(mv); else point.add(nullptr);
    }
  }
}
} // namespace sw
