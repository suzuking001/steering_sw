#pragma once
#include <Arduino.h>
#include "config_codec.h"
namespace sw {
bool loadConfig(Config& cfg);
bool saveConfig(const Config& cfg, String& error);
String randomHex(size_t bytes);
}
