#pragma once
#include <ArduinoJson.h>
#include <array>
#include <cstdint>
#include <string_view>

namespace sw {
inline constexpr char kGithubRepository[] = "suzuking001/steering_sw";
inline constexpr char kFirmwareAsset[] = "steering-sw-esp32dev.bin";

struct FirmwareRelease {
  char version[16]{};
  uint64_t assetId = 0;
  uint32_t size = 0;
  std::array<uint8_t, 32> digest{};
  bool authenticated = false;
};

enum class ReleaseResult { Available, Current, Invalid, TooLarge };
ReleaseResult readFirmwareRelease(ArduinoJson::JsonVariantConst doc, std::string_view running,
                                  size_t slotSize, FirmwareRelease& release);
bool isGithubAssetRedirect(std::string_view url);
bool isGithubToken(std::string_view token);
}
