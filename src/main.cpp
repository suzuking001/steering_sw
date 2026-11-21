#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"
#include "connection.h"
#include "access.h"
#include "network_state.h"
#include "ota_release.h"
#include "ota_update.h"
#include "runtime.h"
#include "web_assets.h"

using namespace sw;
namespace {
WebServer server(80);
DNSServer portalDns;
SetupButton setupButton;
String wifiPassword, apPassword, apName, webCsrf, githubToken;
String inputModeClient;
uint32_t inputModeUpdatedAt = 0;
constexpr uint32_t inputModeTimeoutMs = 15000;
uint32_t rebootAt = 0;
uint32_t firmwareBootAt = 0;
uint32_t lastHidResumeAt = 0;
bool verifyFirmware = false, otaOutputPaused = false, inputModePaused = false, bleEnabled = true;
uint32_t lastApAttempt = 0;
bool apStarted = false, mdnsStarted = false, wasConnected = false, networkChanged = false;
NetworkSchedule networkSchedule;
std::string networkSsid;
std::atomic<int> wifiFailureReason{0};
std::atomic<bool> wifiAssociated{false};
unsigned wifiAttempts = 0;
String announcedIp;
void headers();

void applyInputMode(bool enabled) {
  inputModePaused = enabled;
  enabled = enabled || otaOutputPaused || !bleEnabled;
  if (hidState.suspended.load() == enabled) return;
  if (enabled) runtime.cancel();
  setHidSuspended(enabled);
  runtime.log(!enabled ? "Bluetoothの再接続を待っています" : !bleEnabled ? "Bluetoothを設定で停止しました" :
              otaOutputPaused ? "更新のためBluetoothを一時停止しました" : "文字入力のためBluetoothを一時停止しました");
}

bool pauseHidForFirmware() {
  if (otaOutputPaused) return true;
  otaOutputPaused = runtime.stopHidForFirmware();
  if (!otaOutputPaused) applyInputMode(inputModePaused);
  return otaOutputPaused;
}

void printSetupInfo(const std::string& bleName) {
  Serial.println("\n=== EASY CONNECTION ===");
  Serial.printf("1. Wi-Fi: %s\n   Password: %s\n", apName.c_str(), apPassword.c_str());
  Serial.println("2. Open the setup page (or http://192.168.4.1/)");
  Serial.println("   Web page: opens directly. No Web login or password.");
  if (WiFi.status() == WL_CONNECTED) Serial.printf("   PC on the router network: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("3. On your device > Settings > Bluetooth > %s\n", bleName.c_str());
  Serial.println("Setup Wi-Fi starts at power-on. Hold BOOT for 3 seconds to retry if needed.");
  Serial.println("=======================\n");
}

bool onSetupNetwork() {
  return apStarted && server.client().localIP() == WiFi.softAPIP();
}
bool canonicalSetupHost() {
  const String ip = WiFi.softAPIP().toString();
  return server.hostHeader() == ip || server.hostHeader() == ip + ":80";
}
void redirectToSetup() {
  headers();
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain; charset=utf-8", "設定画面を開いています");
}

bool saveSecret(const char* key, const String& value) {
  Preferences p; if (!p.begin("sw-secrets", false)) return false;
  p.putString(key, value);
  bool ok = p.getString(key, "\x01") == value; p.end(); return ok;
}
void response(int code, const String& message) {
  JsonDocument d; d[code < 400 ? "message" : "error"] = message;
  String json; serializeJson(d, json); server.send(code, "application/json; charset=utf-8", json);
}
void jsonResponse(JsonDocument& d) {
  String json; serializeJson(d, json); server.send(200, "application/json; charset=utf-8", json);
}
void headers() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'");
}
bool allowRequest(bool write = false) {
  headers();
  String host = server.hostHeader(); host.toLowerCase();
  const bool canonical = canonicalLocalHost(host.c_str(), server.client().localIP().toString().c_str());
  const bool tokenMatches = !webCsrf.isEmpty() && server.header("X-CSRF-Token") == webCsrf;
  if (!checkAccess(canonical, write, tokenMatches)) {
    response(403, "設定画面を開き直してください"); return false;
  }
  if (write && (firmwareBusy() || otaOutputPaused)) {
    response(409, "ファームウェアの処理中です。完了後にもう一度操作してください");
    return false;
  }
  return true;
}
bool body(JsonDocument& d, size_t max = 24576) {
  String plain = server.arg("plain");
  if (plain.length() > max) { response(413, "リクエストが大きすぎます"); return false; }
  if (deserializeJson(d, plain, DeserializationOption::NestingLimit(12)) || !d.is<JsonObject>()) {
    response(400, "JSONの形式が不正です"); return false;
  }
  return true;
}
void startAP() {
  if (apStarted && WiFi.AP.started()) return;
  lastApAttempt = millis();
  if (apStarted) portalDns.stop();
  apStarted = WiFi.softAP(apName.c_str(), apPassword.c_str());
  if (!apStarted) {
    runtime.log("設定用Wi-Fiの起動に失敗しました。もう一度試します");
    return;
  }
  if (!portalDns.start(53, "*", WiFi.softAPIP())) runtime.log("設定画面の自動案内を開始できません。IPアドレスから開いてください");
  runtime.log("設定用APを起動: " + apName);
  Serial.printf("Setup Wi-Fi ready: %s / http://%s/\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
}
void connectNetwork() {
  auto c = runtime.config();
  networkSsid = c.ssid;
  networkSchedule.configure(millis(), !networkSsid.empty());
  wifiAttempts = 0; wifiFailureReason = 0; wifiAssociated = false;
  WiFi.disconnectAsync(false, false);
}
void updateNetwork() {
  if (networkChanged) { networkChanged = false; connectNetwork(); }
  const bool hasIp = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0) && WiFi.SSID() == networkSsid.c_str();
  const auto action = networkSchedule.update(millis(), hasIp);
  if (action == NetworkAction::Disconnect) WiFi.disconnectAsync(false, false);
  if (action == NetworkAction::Begin) {
    wifiFailureReason = 0; wifiAssociated = false; ++wifiAttempts;
    WiFi.begin(networkSsid.c_str(), wifiPassword.c_str());
    runtime.log(String("Wi-Fiへ接続を試みています（") + wifiAttempts + "回目）");
  }
  const bool connected = networkSchedule.phase() == NetworkPhase::Connected && hasIp;
  String ip = connected ? WiFi.localIP().toString() : String();
  if (connected && (!wasConnected || ip != announcedIp)) {
    wifiFailureReason = 0;
    if (mdnsStarted) MDNS.end();
    mdnsStarted = MDNS.begin("steering-sw");
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
    announcedIp = ip;
    runtime.log("Wi-Fi接続成功。PCから開くURL: http://" + ip + "/");
    Serial.printf("PC URL: http://%s/ (SSID: %s)\n", ip.c_str(), networkSsid.c_str());
  }
  if (!connected && wasConnected) {
    if (mdnsStarted) MDNS.end();
    mdnsStarted = false; announcedIp = "";
    runtime.log("Wi-Fi切断。設定用Wi-Fiから接続状態を確認できます");
  }
  wasConnected = connected;
}
void networkStatus(JsonObject out) {
  const bool connected = networkSchedule.phase() == NetworkPhase::Connected && WiFi.status() == WL_CONNECTED;
  const int reason = wifiFailureReason.load();
  const bool configured = !networkSsid.empty();
  out["wifiConfigured"] = configured; out["wifiSavedSsid"] = networkSsid;
  out["wifiPasswordSet"] = !wifiPassword.isEmpty(); out["wifiConnected"] = connected;
  out["wifiConnectedSsid"] = connected ? WiFi.SSID() : String();
  out["ip"] = connected ? WiFi.localIP().toString() : String("0.0.0.0");
  out["wifiLanUrl"] = connected ? String("http://") + WiFi.localIP().toString() + "/" : String();
  out["wifiGateway"] = connected ? WiFi.gatewayIP().toString() : String();
  out["rssi"] = connected ? WiFi.RSSI() : 0;
  out["mdnsReady"] = connected && mdnsStarted; out["wifiAttempts"] = wifiAttempts;
  out["wifiFailureReason"] = reason;
  out["wifiState"] = !configured ? "unconfigured" : connected ? "connected" :
    wifiAssociated ? "getting_ip" : (reason && reason != WIFI_REASON_ASSOC_LEAVE) ? "failed" : "connecting";
  String error;
  if (reason == WIFI_REASON_NO_AP_FOUND) error = "Wi-Fiが見つかりません。SSID・2.4GHz設定・電波の届く場所を確認してください";
  else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
    error = "認証に失敗しました。Wi-Fiパスワードとルーターの接続設定を確認してください";
  else if (reason && reason != WIFI_REASON_ASSOC_LEAVE) error = "接続できませんでした。電波とルーターの設定を確認してください";
  out["wifiError"] = error;
}
void setupRoutes() {
  const char* keys[] = {"X-CSRF-Token"}; server.collectHeaders(keys, 1);
  server.on("/", HTTP_GET, [] {
    if (onSetupNetwork() && !canonicalSetupHost()) { redirectToSetup(); return; }
    headers(); server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char*>(web_index), sizeof(web_index));
  });
  server.on("/app.js", HTTP_GET, [] {
    headers(); server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/javascript; charset=utf-8", reinterpret_cast<const char*>(web_js), sizeof(web_js));
  });
  server.on("/style.css", HTTP_GET, [] {
    headers(); server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/css; charset=utf-8", reinterpret_cast<const char*>(web_css), sizeof(web_css));
  });
  server.on("/api/session", HTTP_GET, [] {
    if (!allowRequest()) return;
    JsonDocument d; d["csrf"] = webCsrf; jsonResponse(d);
  });
  server.on("/api/status", HTTP_GET, [] {
    if (!allowRequest()) { return; }
    JsonDocument d;
    auto firmware = d["firmware"].to<JsonObject>();
    firmwareStatus(firmware); firmware["tokenSet"] = !githubToken.isEmpty(); firmware["bootChecking"] = verifyFirmware;
    if (firmwareBusy()) {
      // Leave heap available to TLS while the browser polls update progress.
      d["wifiConnected"] = WiFi.status() == WL_CONNECTED;
      jsonResponse(d);
      return;
    }
    runtime.status(d.as<JsonObject>());
    networkStatus(d.as<JsonObject>());
    d["apActive"] = apStarted && WiFi.AP.started(); d["apName"] = apName;
    d["apIp"] = WiFi.softAPIP().toString(); d["freeHeap"] = ESP.getFreeHeap(); jsonResponse(d);
  });
  server.on("/api/firmware/check", HTTP_POST, [] {
    if (!allowRequest(true)) return;
    if (rebootAt || verifyFirmware) { response(409, "本体の起動確認が完了してから再試行してください"); return; }
    const char* error = nullptr;
    if (!startFirmwareCheck(githubToken, pauseHidForFirmware, error)) { response(409, error); return; }
    response(202, "更新の確認を開始しました");
  });
  server.on("/api/firmware/install", HTTP_POST, [] {
    if (!allowRequest(true)) return;
    if (rebootAt || verifyFirmware) { response(409, "本体の起動確認が完了してから再試行してください"); return; }
    JsonDocument d; if (!body(d, 128)) return;
    if (!d["version"].is<const char*>()) { response(400, "更新するバージョンを指定してください"); return; }
    const char* error = nullptr;
    if (!startFirmwareInstall(githubToken, d["version"].as<String>(), pauseHidForFirmware, error)) { response(409, error); return; }
    response(202, "ファームウェア更新を開始しました");
  });
  server.on("/api/config", HTTP_GET, [] {
    if (!allowRequest()) { return; } auto c = runtime.config(); JsonDocument d; toJson(c, d.to<JsonObject>()); jsonResponse(d);
  });
  auto save = [] {
    if (!allowRequest(true)) { return; }
    JsonDocument d; if (!body(d)) return;
    Config c; String error; std::string parseError;
    if (!fromJson(d.as<JsonVariantConst>(), c, parseError)) { response(400, parseError.c_str()); return; }
    auto old = runtime.config();
    if (d["ble"].isUnbound()) c.ble = old.ble;
    // Wi-Fi changes use /api/wifi so credentials can never leak into backups.
    if (c.ssid != old.ssid) { response(400, "SSIDはWi-Fi設定から変更してください。復元時は現在のSSIDを使用してください"); return; }
    if (!saveConfig(c, error)) { response(507, error); return; }
    runtime.apply(c);
    bleEnabled = c.ble.enabled;
    if (c.ble.name != old.ble.name && !runtime.reconnectHid(inputModePaused)) {
      response(200, "設定は保存済みです。Bluetooth名を適用するため、本体を再起動してください"); return;
    }
    applyInputMode(inputModePaused);
    response(200, "設定を保存・適用しました");
  };
  server.on("/api/config", HTTP_PUT, save);
  server.on("/api/backup", HTTP_GET, [] {
    if (!allowRequest()) { return; }
    auto c = runtime.config(); JsonDocument d; toJson(c, d.to<JsonObject>());
    server.sendHeader("Content-Disposition", "attachment; filename=steering-config.json"); jsonResponse(d);
  });
  server.on("/api/restore", HTTP_POST, save);
  server.on("/api/validate", HTTP_POST, [] {
    if (!allowRequest(true)) { return; }
    JsonDocument d; if (!body(d)) return;
    Config c; std::string error;
    if (!fromJson(d.as<JsonVariantConst>(), c, error)) { response(400, error.c_str()); return; }
    if (d["ble"].isUnbound()) c.ble = runtime.config().ble;
    JsonDocument out; toJson(c, out.to<JsonObject>()); jsonResponse(out);
  });
  server.on("/api/defaults", HTTP_GET, [] {
    if (!allowRequest()) { return; }
    const auto current = runtime.config();
    auto c = defaults(); c.ssid = current.ssid; c.ble = current.ble;
    JsonDocument d; toJson(c, d.to<JsonObject>()); jsonResponse(d);
  });
  server.on("/api/learn/start", HTTP_POST, [] {
    if (!allowRequest(true)) { return; }
    JsonDocument d; if (!body(d, 128)) return;
    if (!d["channel"].is<int>()) { response(400, "入力系統を指定してください"); return; }
    String error;
    if (!runtime.startLearning(d["channel"], error)) { response(409, error); return; }
    response(200, "400ms後から2秒間、対象の状態を保持してください");
  });
  server.on("/api/learn/stop", HTTP_POST, [] { if (!allowRequest(true)) { return; } runtime.stopLearning(); response(200, "学習を中止しました"); });
  server.on("/api/test", HTTP_POST, [] {
    if (!allowRequest(true)) { return; } JsonDocument d; if (!body(d, 128)) return;
    if (!d["action"].is<const char*>()) { response(400, "操作を指定してください"); return; }
    String error;
    if (!runtime.test(d["action"].as<std::string>(), error)) { response(409, error); return; }
    response(200, "操作を開始しました。接続先の端末で結果を確認してください");
  });
  server.on("/api/stop", HTTP_POST, [] { if (!allowRequest(true)) { return; } runtime.cancel(); response(200, "送信を停止しました"); });
  server.on("/api/ble/input-mode", HTTP_PUT, [] {
    if (!allowRequest(true)) { return; }
    JsonDocument d; if (!body(d, 128)) return;
    if (!d["enabled"].is<bool>()) { response(400, "文字入力モードを指定してください"); return; }
    const bool enabled = d["enabled"].as<bool>();
    String client;
    if (!d["client"].isNull()) {
      if (!d["client"].is<const char*>()) { response(400, "入力元が不正です"); return; }
      client = d["client"].as<String>();
      if (client.length() != 16) { response(400, "入力元が不正です"); return; }
    }
    if (enabled) {
      inputModeClient = client;
      inputModeUpdatedAt = millis();
      applyInputMode(true);
    } else if (client.isEmpty() || client == inputModeClient) {
      inputModeClient = "";
      applyInputMode(false);
    }
    response(200, "文字入力の状態を更新しました");
  });
  server.on("/api/passcode", HTTP_PUT, [] {
    if (!allowRequest(true)) { return; } JsonDocument d; if (!body(d, 128)) return;
    if (!d["passcode"].is<const char*>()) { response(400, "数字列または削除用の空文字を指定してください"); return; }
    String pin = d["passcode"].as<String>();
    if (!pin.isEmpty()) {
      if (pin.length() < 4 || pin.length() > 12) { response(400, "パスコードは4〜12桁の数字です"); return; }
      for (char ch : pin) if (ch < '0' || ch > '9') { response(400, "半角数字で入力してください"); return; }
    }
    if (!saveSecret("passcode", pin)) { response(507, "保存できませんでした"); return; }
    runtime.setPasscode(pin); response(200, pin.isEmpty() ? "パスコードを削除しました" : "パスコードを登録しました");
  });
  server.on("/api/wifi", HTTP_PUT, [] {
    if (!allowRequest(true)) { return; } JsonDocument d; if (!body(d, 512)) return;
    if (!d["ssid"].is<const char*>() || (!d["password"].isNull() && !d["password"].is<const char*>())) {
      response(400, "SSIDとパスワードの形式を確認してください"); return;
    }
    String ssid = d["ssid"].as<String>();
    auto c = runtime.config();
    const bool passwordSupplied = d["password"].is<const char*>();
    if (!ssid.isEmpty() && c.ssid != ssid.c_str() && !passwordSupplied) {
      response(400, "新しいWi-Fiのパスワードを入力するか、「パスワードなし」を選んでください"); return;
    }
    String password = ssid.isEmpty() ? String() : passwordSupplied ? d["password"].as<String>() : wifiPassword;
    if (ssid.length() > 32 || password.length() > 63 || (!password.isEmpty() && password.length() < 8)) { response(400, "SSIDは32バイト以内、パスワードは空または8〜63バイトです"); return; }
    if (strlen(ssid.c_str()) != ssid.length() || strlen(password.c_str()) != password.length()) {
      response(400, "SSIDまたはパスワードに使用できない文字があります"); return;
    }
    c.ssid = ssid.c_str(); String error;
    String old = wifiPassword;
    if (!saveSecret("wifi", password)) { response(507, "Wi-Fiパスワードを保存できません"); return; }
    if (!saveConfig(c, error)) { saveSecret("wifi", old); response(507, error); return; }
    wifiPassword = password; runtime.apply(c); networkChanged = true;
    response(200, "Wi-Fi設定は保存済みです。接続結果とPC用URLは下の接続状態で確認できます");
  });
  server.on("/api/wifi/reconnect", HTTP_POST, [] {
    if (!allowRequest(true)) return;
    networkChanged = true; response(200, "保存済みのWi-Fi設定で接続をやり直します");
  });
  server.on("/api/reset", HTTP_POST, [] {
    if (!allowRequest(true)) { return; } JsonDocument d; if (!body(d, 128)) return;
    if (d["confirm"].as<String>() != "RESET") { response(400, "初期化の確認文字が不正です"); return; }
    auto c = defaults(); String error;
    c.ble = runtime.config().ble;
    if (!saveConfig(c, error)) { response(507, error); return; }
    runtime.apply(c);
    bool ok = saveSecret("passcode", "") && saveSecret("wifi", "");
    runtime.setPasscode(""); wifiPassword = ""; networkChanged = true;
    response(ok ? 200 : 507, ok ? "入力・Wi-Fi・パスコード設定を初期化しました。設定用APのパスワードとBLEペアリングは維持します" : "設定は初期化しましたが秘密情報の削除に失敗しました。再度初期化してください");
  });
  server.on("/api/ble/reconnect", HTTP_POST, [] {
    if (!allowRequest(true)) return;
    if (!bleEnabled || inputModePaused || rebootAt) {
      response(409, "Bluetoothを有効にし、一時停止が解除されてから再接続してください"); return;
    }
    if (!runtime.reconnectHid(false)) { response(503, "Bluetoothの再接続を開始できませんでした"); return; }
    runtime.log("Bluetoothの再接続を待っています");
    response(200, "Bluetoothを再接続できる状態にしました");
  });
  server.on("/api/ble/forget", HTTP_POST, [] {
    if (!allowRequest(true)) { return; } runtime.cancel();
    // Reboot immediately after bond deletion. A live connection must not keep using stale keys.
    if (!NimBLEDevice::deleteAllBonds()) { response(500, "ペアリング情報を削除できませんでした"); return; }
    response(200, "ペアリング情報を削除しました。再起動後、接続先の端末側でも登録を解除して再ペアリングしてください");
    rebootAt = millis() + 700;
  });
  server.on("/api/reboot", HTTP_POST, [] {
    if (!allowRequest(true)) { return; } runtime.cancel(); response(200, "再起動します"); rebootAt = millis() + 700;
  });
  // Captive-network probes must lead to the canonical IP so the page
  // and its relative assets use one origin. Never redirect an unknown API request.
  const char* probes[] = {"/hotspot-detect.html", "/library/test/success.html", "/generate_204",
                         "/gen_204", "/connecttest.txt", "/ncsi.txt", "/redirect"};
  for (const char* path : probes) server.on(path, HTTP_GET, [] {
    if (onSetupNetwork()) redirectToSetup();
    else { headers(); response(404, "ページまたはAPIがありません"); }
  });
  server.onNotFound([] {
    if (onSetupNetwork() && server.method() == HTTP_GET && !server.uri().startsWith("/api/")) { redirectToSetup(); return; }
    headers(); response(404, "ページまたはAPIがありません");
  });
  server.begin();
}
void serialCommand() {
  static String line;
  static bool overflow = false;
  for (int i = 0; i < 64 && Serial.available(); ++i) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (overflow) { line = ""; overflow = false; Serial.println("Command too long."); continue; }
      line.trim();
      // Keep older USB commands as harmless aliases for the connection guide.
      if (line == "SETUP" || line == "EASY_SETUP" || line == "RESET_ADMIN") {
        startAP(); printSetupInfo(runtime.config().ble.name);
      }
      if (line == "HELP") {
        Serial.println("SETUP: show/start setup Wi-Fi; GITHUB_TOKEN <fine-grained PAT>: save read-only release access; GITHUB_TOKEN_CLEAR: remove it; HELP");
      }
      if (line.startsWith("GITHUB_TOKEN ") || line == "GITHUB_TOKEN_CLEAR") {
        String token = line == "GITHUB_TOKEN_CLEAR" ? String() : line.substring(13);
        if (firmwareBusy()) Serial.println("Firmware update in progress. Try after it completes.");
        else if (!token.isEmpty() && !isGithubToken({token.c_str(), token.length()})) Serial.println("Use a fine-grained GitHub personal access token.");
        else if (!saveSecret("github", token)) Serial.println("Could not save GitHub token.");
        else { githubToken = token; Serial.println(token.isEmpty() ? "GitHub token cleared." : "GitHub token registered."); }
      }
      line = "";
    } else if (line.length() < 192) line += ch;
    else overflow = true;
  }
}

void fatalStartup(const char* message) {
  Serial.println(message);
  if (verifyFirmware) { rollBackFirmwareBoot(); ESP.restart(); }
  for (;;) delay(1000);
}

}
void setup() {
  Serial.begin(115200); delay(200);
  verifyFirmware = pendingFirmwareBoot();
  firmwareBootAt = millis();
  pinMode(0, INPUT_PULLUP);
  Config c; bool restored = loadConfig(c);
  Preferences p;
  if (!p.begin("sw-secrets", false)) fatalStartup("FATAL: Cannot open NVS. No HID will start.");
  apPassword = p.getString("ap", "");
  wifiPassword = p.getString("wifi", ""); String passcode = p.getString("passcode", "");
  githubToken = p.getString("github", "");
  bool credentialOK = true;
  if (apPassword.length() < 8) { apPassword = randomHex(6); credentialOK &= p.putString("ap", apPassword) == apPassword.length(); }
  p.end();
  if (!credentialOK) fatalStartup("FATAL: Cannot store credentials. No HID will start.");
  apName = "SteeringSW-" + String(uint32_t(ESP.getEfuseMac() >> 24) & 0xFFFFFF, HEX);
  webCsrf = randomHex(24);
  Serial.println("Steering Switch / 115200 baud. HELP for serial commands.");
  printSetupInfo(c.ble.name);
  bleEnabled = c.ble.enabled;
  hidState.suspended = !bleEnabled;
  if (!beginHid(c.ble.name)) fatalStartup("FATAL: Cannot start Bluetooth.");
  if (!runtime.begin(c, passcode)) fatalStartup("FATAL: Cannot start input task.");
  runtime.log(restored ? "保存設定を復元しました" : "参考プリセットで開始。校正後に送信を有効にしてください");
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) { wifiAssociated = true; wifiFailureReason = 0; }
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiAssociated = false; wifiFailureReason = info.wifi_sta_disconnected.reason;
    }
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) { wifiAssociated = true; wifiFailureReason = 0; }
  });
  WiFi.setHostname("steering-sw");
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false); WiFi.setSleep(false);
  startAP();
  connectNetwork(); setupRoutes();
}
void loop() {
  server.handleClient(); updateNetwork(); serialCommand();
  portalDns.processNextRequest();
  if ((!apStarted || !WiFi.AP.started()) && millis() - lastApAttempt >= 2000) {
    startAP();
  }
  if (!inputModeClient.isEmpty() && millis() - inputModeUpdatedAt >= inputModeTimeoutMs) {
    inputModeClient = "";
    applyInputMode(false);
  }
  if (setupButton.update(millis(), digitalRead(0) == LOW)) {
    startAP();
  }
  if (verifyFirmware && millis() - firmwareBootAt >= 2500) {
    if (apStarted && WiFi.AP.started() && runtime.hasSamples() && confirmFirmwareBoot()) {
      verifyFirmware = false;
      runtime.log("更新後の起動確認が完了しました");
    } else if (millis() - firmwareBootAt >= 15000) fatalStartup("FATAL: Updated firmware did not pass startup checks.");
  }
  if (firmwareRestartPending() && !rebootAt) rebootAt = millis() + 1400;
  if (otaOutputPaused && !firmwareBusy() && millis() - lastHidResumeAt >= 1000) {
    lastHidResumeAt = millis();
    if (runtime.resumeHidAfterFirmware()) {
      otaOutputPaused = false;
      applyInputMode(inputModePaused);
    }
  }
  if (rebootAt && int32_t(millis() - rebootAt) >= 0) ESP.restart();
  delay(2);
}
