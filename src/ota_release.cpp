#include "ota_release.h"
#include <cstring>

namespace sw {
namespace {
bool parseVersion(std::string_view text, std::array<unsigned, 3>& parts) {
  for (size_t i = 0; i < parts.size(); ++i) {
    const size_t dot = text.find('.');
    const auto part = text.substr(0, dot);
    if (part.empty() || part.size() > 4 || (part.size() > 1 && part.front() == '0') ||
        (dot == text.npos) != (i == parts.size() - 1)) return false;
    for (const char digit : part) {
      if (digit < '0' || digit > '9') return false;
      parts[i] = 10 * parts[i] + digit - '0';
    }
    if (dot != text.npos) text.remove_prefix(dot + 1);
  }
  return true;
}

int hexDigit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}
}

ReleaseResult readFirmwareRelease(ArduinoJson::JsonVariantConst doc, std::string_view running,
                                  size_t slotSize, FirmwareRelease& release) {
  const std::string_view tag = doc["tag_name"] | "";
  std::array<unsigned, 3> current{}, version{};
  if (tag.empty() || tag.front() != 'v' || !parseVersion(tag.substr(1), version) ||
      !parseVersion(running, current) || doc["draft"] != false || doc["prerelease"] != false) {
    return ReleaseResult::Invalid;
  }
  if (version <= current) return ReleaseResult::Current;

  for (const auto asset : doc["assets"].as<ArduinoJson::JsonArrayConst>()) {
    if (asset["name"] != kFirmwareAsset) continue;
    if (asset["state"] != "uploaded" || !asset["id"].is<uint64_t>() || !asset["size"].is<uint32_t>()) {
      return ReleaseResult::Invalid;
    }
    FirmwareRelease candidate;
    candidate.assetId = asset["id"].as<uint64_t>();
    candidate.size = asset["size"].as<uint32_t>();
    const std::string_view digest = asset["digest"] | "";
    if (!candidate.assetId || !candidate.size || digest.size() != 71 || digest.substr(0, 7) != "sha256:") {
      return ReleaseResult::Invalid;
    }
    if (candidate.size > slotSize) return ReleaseResult::TooLarge;
    for (size_t i = 0; i < candidate.digest.size(); ++i) {
      const int high = hexDigit(digest[7 + 2 * i]);
      const int low = hexDigit(digest[8 + 2 * i]);
      if (high < 0 || low < 0) return ReleaseResult::Invalid;
      candidate.digest[i] = static_cast<uint8_t>(high * 16 + low);
    }
    std::memcpy(candidate.version, tag.data() + 1, tag.size() - 1);
    release = candidate;
    return ReleaseResult::Available;
  }
  return ReleaseResult::Invalid;
}

bool isGithubAssetRedirect(std::string_view url) {
  constexpr std::string_view scheme = "https://";
  if (url.substr(0, scheme.size()) != scheme) return false;
  url.remove_prefix(scheme.size());
  const auto slash = url.find('/');
  if (slash == url.npos || url.find_first_of("\\\r\n\t #") != url.npos) return false;
  const auto host = url.substr(0, slash);
  return host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com" ||
         host == "github-releases.githubusercontent.com";
}

bool isGithubToken(std::string_view token) {
  if (token.size() <= 11 || token.size() > 160 || token.substr(0, 11) != "github_pat_") return false;
  for (const char ch : token) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
  }
  return true;
}
}
