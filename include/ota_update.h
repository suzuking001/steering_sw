#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace sw {
bool startFirmwareCheck(const String& token, const char*& error);
bool startFirmwareInstall(const String& token, const String& version, const char*& error);
bool firmwareInstalling();
bool firmwareRestartPending();
void firmwareStatus(ArduinoJson::JsonObject out);
bool pendingFirmwareBoot();
bool confirmFirmwareBoot();
void rollBackFirmwareBoot();
}
