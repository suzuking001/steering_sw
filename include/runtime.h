#pragma once
#include "config.h"
#include "hid.h"
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace sw {
class Runtime {
  struct Reading {
    int raw = -1, filtered = -1, filteredMv = -1;
    double ohm = NAN;
    int recognized = kUnknown;
    int rawWindow[5] = {}, cursor = 0;
    uint32_t lastValidAt = 0;
    bool initialized = false;
  } readings_[kChannels];
  struct VoltageSample { uint32_t at; int16_t mv[kChannels]; };
  static constexpr size_t kHistorySize = 96;
  VoltageSample history_[kHistorySize];
  size_t historyNext_ = 0, historyCount_ = 0;
  struct Learning {
    bool active = false, done = false;
    int channel = 0, count = 0, invalid = 0;
    uint32_t started = 0;
    double minimum = 0, maximum = 0, mean = 0, m2 = 0;
    String error;
  } learning_;
  struct Log { uint32_t at = 0; String message; } logs_[32];
  size_t logCursor_ = 0, logCount_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
  Config cfg_;
  String passcode_;
  GestureEngine engines_[kChannels];
  AccDetector acc_;
  PasscodeGate passcodeGate_;
  MacroRunner runner_;
  uint32_t hidEpoch_ = 0;
  bool ready_ = false, inputModePaused_ = false;
  adc_oneshot_unit_handle_t adc_ = nullptr;
  adc_cali_handle_t calibration_ = nullptr;
  adc_channel_t adcChannels_[kChannels + 1];
  int accMv_ = -1;
  uint32_t samples_ = 0, sampleAt_ = 0;
  bool sampled_ = false;
  bool beginAdc();
  int millivolts(int raw) const;
  void tick();
  void writeReadingsLocked(JsonObject out);
  void resetEngines();
  void logLocked(const String& message);
  bool canSendLocked(String& error);
  bool startStepsLocked(std::vector<Step>&& steps, const char* label, String& error);
  bool actionLocked(const std::string& action, Gesture gesture, bool physical, String& error);
 public:
  bool begin(const Config& cfg, const String& passcode);
  bool hasSamples();
  bool stopHidForFirmware();
  bool resumeHidAfterFirmware();
  bool reconnectHid();
  Config config();
  void apply(const Config& cfg);
  void setPasscode(const String& passcode);
  void setInputMode(bool enabled);
  void status(JsonObject out, bool includeLogs);
  void liveReadings(JsonObject out, bool includeHistory, uint32_t after);
  void log(const String& message);
  bool startLearning(int channel, String& error);
  void stopLearning();
  bool test(const std::string& action, String& error);
  bool testMacro(std::vector<Step>&& steps, String& error);
  void cancel();
};
extern Runtime runtime;
}
