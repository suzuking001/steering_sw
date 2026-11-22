#pragma once
#include <ArduinoJson.h>
#include "model.h"
namespace sw {
Config defaults();
void toJson(const Config& cfg, JsonObject out);
bool fromJson(JsonVariantConst in, Config& cfg, std::string& error);
bool patchConfig(JsonObjectConst changes, const Config& current, Config& cfg, std::string& error);
}
