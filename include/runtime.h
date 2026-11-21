#pragma once
#include "config.h"
#include "hid.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace sw {
class Runtime {
  struct Reading {
    int raw = 0, mv = 0;
    double filtered = 0, filteredMv = 0, ohm = NAN;
    int recognized = kUnknown;
    int rawWindow[5] = {}, mvWindow[5] = {}, cursor = 0;
    bool initialized = false;
  } readings_[kChannels];
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
  int accMv_ = 0;
  uint32_t samples_ = 0;
  void tick();
  void resetEngines();
  void logLocked(const String& message);
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
  void log(const String& message);
  bool startLearning(int channel, String& error);
  void stopLearning();
  bool test(const std::string& action, String& error);
  void cancel();
};
extern Runtime runtime;
}
