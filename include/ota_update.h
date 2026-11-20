#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace sw {
bool startFirmwareCheck(const String& token, bool (*prepareNetwork)(), const char*& error);
bool startFirmwareInstall(const String& token, const String& version, bool (*prepareNetwork)(), const char*& error);
bool firmwareBusy();
bool firmwareRestartPending();
void firmwareStatus(ArduinoJson::JsonObject out);
bool pendingFirmwareBoot();
bool confirmFirmwareBoot();
void rollBackFirmwareBoot();
}
