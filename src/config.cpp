#include "config.h"
#include <Preferences.h>
#include <esp_system.h>

namespace sw {
namespace { int activeSlot = 0; }
bool loadConfig(Config& cfg) {
  Preferences p; if (!p.begin("steering", true, "settings")) { cfg = defaults(); return false; }
  int selected = p.getUChar("active", 0) % 2;
  activeSlot = selected;
  bool ok = false;
  for (int i = 0; i < 2 && !ok; ++i) {
    const char* key = ((selected + i) % 2) ? "config1" : "config0";
    size_t size = p.getBytesLength(key);
    if (!size || size > 24576) continue;
    std::vector<uint8_t> buf(size);
    if (p.getBytes(key, buf.data(), size) != size) continue;
    JsonDocument doc; std::string error;
    if (!deserializeJson(doc, buf.data(), size) && fromJson(doc.as<JsonVariantConst>(), cfg, error)) {
      ok = true; activeSlot = (selected + i) % 2;
    }
  }
  p.end(); if (!ok) cfg = defaults(); return ok;
}
bool saveConfig(const Config& cfg, String& error) {
  JsonDocument doc; toJson(cfg, doc.to<JsonObject>());
  if (doc.overflowed() || measureJson(doc) > 24576) { error = "設定が大きすぎます（上限24KB）"; return false; }
  String data; serializeJson(doc, data);
  Preferences p;
  if (!p.begin("steering", false, "settings")) { error = "設定領域を開けません"; return false; }
  // Preserve the slot actually loaded, even when the persisted selector was bad.
  int target = 1 - activeSlot;
  const char* key = target ? "config1" : "config0";
  bool ok = p.putBytes(key, data.c_str(), data.length()) == data.length();
  if (ok) {
    std::vector<char> check(data.length());
    ok = p.getBytes(key, check.data(), check.size()) == check.size() && memcmp(check.data(), data.c_str(), check.size()) == 0;
  }
  if (ok) ok = p.putUChar("active", target) == 1;
  if (ok) activeSlot = target;
  p.end(); if (!ok) error = "設定保存に失敗しました。現在の設定を維持します";
  return ok;
}
String randomHex(size_t bytes) {
  String out; out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) { uint8_t n = esp_random(); char b[3]; snprintf(b, sizeof(b), "%02x", n); out += b; }
  return out;
}
} // namespace sw
