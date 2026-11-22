#include "config_codec.h"
#include <cstring>
namespace sw {
Config defaults() {
  Config c;
  const auto voltage = [](double ohms) { return std::round(3300 * ohms / (ohms + 330)); };
  for (int i = 0; i < kChannels; ++i) {
    auto& channel = c.channels[i];
    channel.name = i == 0 ? "KEY1" : "KEY2";
    channel.mode = "mv";
    channel.idleCenter = voltage(i == 0 ? 5211 : 5187);
    channel.idleTolerance = 120;
    channel.hysteresis = 15;
  }
  auto add = [&](const char* id, const char* name, int ch, double ohms,
                 const char* tap, const char* dbl = "none", const char* hold = "none") {
    Button b; b.id = id; b.name = name; b.channel = ch; b.center = voltage(ohms); b.tolerance = 120;
    b.shortAction = tap; b.doubleAction = dbl; b.longAction = hold; c.buttons.push_back(b);
  };
  add("call", "通話", 0, 57.6, "switch_app");
  add("phone_on", "電話ON", 0, 243.3, "none", "none", "passcode");
  add("phone_off", "電話OFF", 0, 753, "lock");
  add("mute", "MUTE", 1, 57.5, "play_pause");
  add("plus", "＋", 1, 132.7, "volume_up");
  add("minus", "－", 1, 242.7, "volume_down");
  add("mode", "MODE", 1, 423.4, "home", "app_list", "siri");
  add("next", "＞", 1, 754, "next", "none", "maps");
  add("previous", "＜", 1, 1573, "previous", "none", "youtube");
  // These five named macros are editable presets. Built-in actions expand them.
  const char* ids[] = {"maps", "youtube", "siri", "lock", "app_list"};
  const char* names[] = {"Googleマップ起動", "YouTube起動", "Siri", "画面ロック", "アプリ一覧"};
  for (int i = 0; i < 5; ++i) {
    Macro m; m.id = ids[i]; m.name = names[i];
    Step s; s.type = "chord"; s.key = 30 + i; s.modifiers = 7; m.steps.push_back(s); c.macros.push_back(m);
  }
  return c;
}
void toJson(const Config& c, JsonObject o) {
  o["version"] = c.version; o["name"] = c.name; o["ssid"] = c.ssid; o["outputEnabled"] = c.outputEnabled;
  auto ble = o["ble"].to<JsonObject>();
  ble["name"] = c.ble.name;
  ble["enabled"] = c.ble.enabled;
  auto channels = o["channels"].to<JsonArray>();
  for (const auto& ch : c.channels) {
    auto a = channels.add<JsonObject>();
    a["name"] = ch.name; a["mode"] = ch.mode; a["pullup"] = ch.pullup; a["supplyMv"] = ch.supplyMv;
    a["idleCenter"] = ch.idleCenter; a["idleTolerance"] = ch.idleTolerance; a["hysteresis"] = ch.hysteresis;
  }
  auto buttons = o["buttons"].to<JsonArray>();
  for (const auto& b : c.buttons) {
    auto a = buttons.add<JsonObject>();
    a["id"] = b.id; a["name"] = b.name; a["channel"] = b.channel; a["center"] = b.center; a["tolerance"] = b.tolerance;
    a["shortAction"] = b.shortAction; a["doubleAction"] = b.doubleAction; a["longAction"] = b.longAction;
  }
  auto macros = o["macros"].to<JsonArray>();
  for (const auto& m : c.macros) {
    auto a = macros.add<JsonObject>(); a["id"] = m.id; a["name"] = m.name;
    auto steps = a["steps"].to<JsonArray>();
    for (const auto& s : m.steps) {
      auto v = steps.add<JsonObject>(); v["type"] = s.type;
      if (s.type == "wait") v["ms"] = s.ms;
      if (s.type == "text") v["text"] = s.text;
      if (s.type == "chord") { v["key"] = s.key; v["modifiers"] = s.modifiers; }
      if (s.type == "media") v["usage"] = s.usage;
    }
  }
  auto t = o["timing"].to<JsonObject>();
  t["debounceMs"] = c.debounceMs; t["longMs"] = c.longMs; t["doubleMs"] = c.doubleMs;
  t["repeatDelayMs"] = c.repeatDelayMs; t["repeatMs"] = c.repeatMs; t["wakeDelayMs"] = c.wakeDelayMs;
  t["wakeSpace"] = c.wakeSpace;
  auto acc = o["acc"].to<JsonObject>();
  acc["enabled"] = c.accEnabled; acc["thresholdMv"] = c.accThresholdMv; acc["debounceMs"] = c.accDebounceMs;
}
namespace {
bool str(JsonVariantConst v, std::string& out, size_t max = 128) {
  if (!v.is<const char*>()) return false;
  auto s = v.as<JsonString>();
  if (s.size() > max || strlen(s.c_str()) != s.size()) return false;
  out.assign(s.c_str(), s.size()); return true;
}
bool number(JsonVariantConst v, double& out) {
  if (!v.is<double>() || v.is<bool>()) return false;
  out = v.as<double>(); return std::isfinite(out);
}
bool integer(JsonVariantConst v, int& out) {
  if (!v.is<int>() || v.is<bool>()) return false;
  out = v.as<int>(); return true;
}
bool duration(JsonVariantConst v, uint32_t& out) {
  if (!v.is<uint32_t>() || v.is<bool>()) return false;
  out = v.as<uint32_t>(); return true;
}
bool boolean(JsonVariantConst v, bool& out) {
  if (!v.is<bool>()) return false;
  out = v.as<bool>(); return true;
}
}
bool readMacroSteps(JsonArrayConst in, std::vector<Step>& steps) {
  if (in.isNull() || in.size() > 32) return false;
  for (auto v : in) {
    Step s;
    if (!str(v["type"], s.type, 8)) return false;
    if (s.type == "wait" && !duration(v["ms"], s.ms)) return false;
    if (s.type == "text" && !str(v["text"], s.text, 128)) return false;
    if (s.type == "chord" && (!integer(v["key"], s.key) || !integer(v["modifiers"], s.modifiers))) return false;
    if (s.type == "media" && !integer(v["usage"], s.usage)) return false;
    steps.push_back(std::move(s));
  }
  return true;
}
bool fromJson(JsonVariantConst in, Config& result, std::string& error) {
  Config c;
  auto fail = [&]() { error = "設定JSONの必須項目または型が不正です"; return false; };
  if (!in.is<JsonObjectConst>() || !integer(in["version"], c.version) || !str(in["name"], c.name, 96) ||
      !str(in["ssid"], c.ssid, 32) || !boolean(in["outputEnabled"], c.outputEnabled)) return fail();
  // Migrate the old preset label while preserving user-defined names.
  if (c.name == "クロスビー / 汎用") c.name = Config{}.name;
  const auto ble = in["ble"].as<JsonVariantConst>();
  if (!ble.isUnbound() && (!ble.is<JsonObjectConst>() || !str(ble["name"], c.ble.name, kMaxBleNameBytes) ||
                           !boolean(ble["enabled"], c.ble.enabled))) return fail();
  auto cs = in["channels"].as<JsonArrayConst>();
  if (cs.size() != kChannels) return fail();
  for (int i = 0; i < kChannels; ++i) {
    auto a = cs[i]; auto& ch = c.channels[i];
    if (!str(a["name"], ch.name, 48) || !str(a["mode"], ch.mode, 8) || !number(a["pullup"], ch.pullup) ||
        !number(a["supplyMv"], ch.supplyMv) || !number(a["idleCenter"], ch.idleCenter) ||
        !number(a["idleTolerance"], ch.idleTolerance) || !number(a["hysteresis"], ch.hysteresis)) return fail();
  }
  if (!in["buttons"].is<JsonArrayConst>() || in["buttons"].size() > kMaxButtons ||
      !in["macros"].is<JsonArrayConst>() || in["macros"].size() > 16) return fail();
  for (auto a : in["buttons"].as<JsonArrayConst>()) {
    Button b;
    if (!str(a["id"], b.id, 24) || !str(a["name"], b.name, 96) || !integer(a["channel"], b.channel) ||
        !number(a["center"], b.center) || !number(a["tolerance"], b.tolerance) ||
        !str(a["shortAction"], b.shortAction, 32) || !str(a["doubleAction"], b.doubleAction, 32) || !str(a["longAction"], b.longAction, 32)) return fail();
    c.buttons.push_back(b);
  }
  for (auto a : in["macros"].as<JsonArrayConst>()) {
    Macro m;
    if (!str(a["id"], m.id, 24) || !str(a["name"], m.name, 96) ||
        !readMacroSteps(a["steps"].as<JsonArrayConst>(), m.steps)) return fail();
    c.macros.push_back(m);
  }
  auto t = in["timing"];
  if (!duration(t["debounceMs"], c.debounceMs) || !duration(t["longMs"], c.longMs) || !duration(t["doubleMs"], c.doubleMs) ||
      !duration(t["repeatDelayMs"], c.repeatDelayMs) || !duration(t["repeatMs"], c.repeatMs) ||
      !duration(t["wakeDelayMs"], c.wakeDelayMs) || !boolean(t["wakeSpace"], c.wakeSpace)) return fail();
  auto acc = in["acc"];
  if (!boolean(acc["enabled"], c.accEnabled) || !integer(acc["thresholdMv"], c.accThresholdMv) || !duration(acc["debounceMs"], c.accDebounceMs)) return fail();
  auto msg = validate(c);
  if (!msg.empty()) { error = msg.c_str(); return false; }
  // Named actions must always have a valid, editable macro behind them.
  for (const char* id : {"maps", "youtube", "siri", "lock", "app_list"}) {
    bool found = false; for (const auto& m : c.macros) if (m.id == id) found = true;
    if (!found) { error = std::string("標準ショートカット用マクロを残してください: ") + id; return false; }
  }
  result = std::move(c); return true;
}
bool patchConfig(JsonObjectConst changes, const Config& current, Config& result, std::string& error) {
  if (changes.size() == 0) {
    error = "保存する設定を指定してください";
    return false;
  }
  JsonDocument merged;
  auto settings = merged.to<JsonObject>();
  toJson(current, settings);
  for (JsonPairConst change : changes) {
    const auto key = change.key();
    if (settings[key].isUnbound() || key == "ssid" || key == "version") {
      error = "変更できない設定項目です";
      return false;
    }
    settings[key].set(change.value());
  }
  if (merged.overflowed()) {
    error = "設定を展開できません";
    return false;
  }
  return fromJson(merged.as<JsonVariantConst>(), result, error);
}
} // namespace sw
