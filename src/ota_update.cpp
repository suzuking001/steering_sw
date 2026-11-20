#include "ota_update.h"
#include "ota_release.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <WiFi.h>
#include <ctime>
#include <memory>

namespace sw {
namespace {
constexpr char apiHost[] = "https://api.github.com/repos/";
constexpr unsigned maxMetadataBytes = 32768;
constexpr uint32_t maxDownloadMs = 180000;
constexpr char noPartition[] = "更新用領域が見つかりません。USBでOTA対応版を書き込んでください。";

enum class Stage { Idle, Checking, Current, Available, Downloading, Verifying, Restarting, Failed };
struct Status {
  Stage stage = Stage::Idle;
  FirmwareRelease release;
  uint32_t received = 0;
  const char* message = "";
};
Status state;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

Status statusSnapshot() {
  portENTER_CRITICAL(&stateLock);
  const Status snapshot = state;
  portEXIT_CRITICAL(&stateLock);
  return snapshot;
}

void setStage(Stage stage, const char* message = "", const FirmwareRelease* release = nullptr) {
  portENTER_CRITICAL(&stateLock);
  state.stage = stage;
  state.message = message;
  if (stage == Stage::Checking) {
    state.release = {};
    state.received = 0;
  }
  if (release) state.release = *release;
  portEXIT_CRITICAL(&stateLock);
}

bool isInstalling(Stage stage) {
  return stage == Stage::Downloading || stage == Stage::Verifying || stage == Stage::Restarting;
}

const esp_partition_t* updatePartition() {
  const auto* running = esp_ota_get_running_partition();
  const auto* target = esp_ota_get_next_update_partition(nullptr);
  if (!running || !target || running->address == target->address ||
      (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 && running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) return nullptr;
  return target;
}

bool ensureClock() {
  constexpr time_t recent = 1735689600;
  if (std::time(nullptr) >= recent) return true;
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  const uint32_t start = millis();
  while (std::time(nullptr) < recent && millis() - start < 12000 && WiFi.status() == WL_CONNECTED) delay(100);
  return std::time(nullptr) >= recent;
}

class GithubResponse {
public:
  GithubResponse() = default;
  GithubResponse(const GithubResponse&) = delete;
  GithubResponse& operator=(const GithubResponse&) = delete;
  ~GithubResponse() { close(); }

  int open(const String& url, const String& token, bool binary) {
    close();
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 8000;
    config.disable_auto_redirect = true;
    config.event_handler = event;
    config.user_data = this;
    client_ = esp_http_client_init(&config);
    if (!client_) return -1;
    const bool apiRequest = url.startsWith(apiHost);
    if ((apiRequest && esp_http_client_set_header(client_, "X-GitHub-Api-Version", "2026-03-10") != ESP_OK) ||
        esp_http_client_set_header(client_, "User-Agent", "Steering-Switch/" SW_FIRMWARE_VERSION) != ESP_OK ||
        esp_http_client_set_header(client_, "Accept", binary ? "application/octet-stream" : "application/vnd.github+json") != ESP_OK) return -1;
    String authorization;
    if (!token.isEmpty()) {
      if (!apiRequest) return -1;
      authorization = "Bearer " + token;
      if (authorization.length() != token.length() + 7 ||
          esp_http_client_set_header(client_, "Authorization", authorization.c_str()) != ESP_OK) return -1;
    }
    if (esp_http_client_open(client_, 0) != ESP_OK || esp_http_client_fetch_headers(client_) < 0) return -1;
    return esp_http_client_get_status_code(client_);
  }

  int read(char* buffer, size_t length) { return esp_http_client_read(client_, buffer, length); }
  int64_t contentLength() const { return esp_http_client_get_content_length(client_); }
  bool complete() const { return esp_http_client_is_complete_data_received(client_); }
  const String& location() const { return location_; }

private:
  static esp_err_t event(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Location") == 0 && strlen(e->header_value) <= 4096) {
      static_cast<GithubResponse*>(e->user_data)->location_ = e->header_value;
    }
    return ESP_OK;
  }
  void close() {
    if (client_) esp_http_client_cleanup(client_);
    client_ = nullptr;
    location_ = "";
  }
  esp_http_client_handle_t client_ = nullptr;
  String location_;
};

String releaseUrl() { return String(apiHost) + kGithubRepository + "/releases/"; }

const char* githubError(int code, bool authenticated) {
  if (code == 401) return "GitHubトークンが無効か期限切れです。USBから更新してください。";
  if (code == 403 || code == 429) return authenticated ?
    "GitHubがアクセスを拒否しました。トークンの権限を確認するか、時間をおいて再試行してください。" :
    "GitHubがアクセスを制限しています。時間をおいて再試行してください。";
  if (code == 404) return authenticated ?
    "GitHubにリリースがないか、トークンにリポジトリの読み取り権限がありません。" :
    "公開リリースが見つかりません。更新元が非公開の場合はGitHubの認証が必要です。";
  return "GitHubと通信できませんでした。本体のインターネット接続を確認してください。";
}

const char* checkRelease(const String& token) {
  const auto* target = updatePartition();
  if (!target) return noPartition;
  String json;
  bool authenticated = false;
  {
    GithubResponse response;
    const String url = releaseUrl() + "latest";
    int code = response.open(url, "", false);
    if (!token.isEmpty() && (code == 403 || code == 404 || code == 429)) {
      authenticated = true;
      code = response.open(url, token, false);
    }
    if (code != 200) return githubError(code, authenticated);
    if (response.contentLength() > maxMetadataBytes) return "リリース情報が大きすぎます。";
    char buffer[1024];
    const uint32_t start = millis();
    while (!response.complete()) {
      const int length = response.read(buffer, sizeof(buffer));
      if (length == 0 && response.complete()) break;
      if (length <= 0 || millis() - start > 30000) return "リリース情報を受信できませんでした。";
      if (json.length() + length > maxMetadataBytes) return "リリース情報が大きすぎます。";
      if (!json.concat(buffer, length)) return "リリース情報の読み込みに必要な空きメモリーがありません。";
    }
  }

  JsonDocument filter, doc;
  for (const char* field : {"tag_name", "draft", "prerelease"}) filter[field] = true;
  for (const char* field : {"name", "id", "size", "digest", "state"}) filter["assets"][0][field] = true;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter))) return "リリース情報の形式が不正です。";
  FirmwareRelease release;
  switch (readFirmwareRelease(doc.as<JsonVariantConst>(), SW_FIRMWARE_VERSION, target->size, release)) {
    case ReleaseResult::Available:
      release.authenticated = authenticated;
      setStage(Stage::Available, "新しいファームウェアがあります。", &release);
      return nullptr;
    case ReleaseResult::Current: setStage(Stage::Current, "現在のバージョンから更新する必要はありません。"); return nullptr;
    case ReleaseResult::TooLarge: return "ファームウェアが更新用領域に収まりません。";
    default: return "対応するファームウェアまたはSHA-256がリリースにありません。";
  }
}

class OtaWrite {
public:
  ~OtaWrite() { if (open_) esp_ota_abort(handle_); }
  bool begin(const esp_partition_t* target) {
    open_ = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle_) == ESP_OK;
    return open_;
  }
  bool write(const void* buffer, size_t length) { return esp_ota_write(handle_, buffer, length) == ESP_OK; }
  bool finish() { open_ = false; return esp_ota_end(handle_) == ESP_OK; }

private:
  esp_ota_handle_t handle_{};
  bool open_ = false;
};

const char* installRelease(const String& token, const FirmwareRelease& release) {
  const auto* target = updatePartition();
  if (!target) return noPartition;
  if (release.size > target->size) return "ファームウェアが更新用領域に収まりません。";
  GithubResponse response;
  char id[24];
  snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(release.assetId));
  const String url = release.authenticated ? releaseUrl() + "assets/" + id :
    String("https://github.com/") + kGithubRepository + "/releases/download/v" + release.version + "/" + kFirmwareAsset;
  int code = response.open(url, release.authenticated ? token : String(), true);
  bool authenticated = release.authenticated;
  if (code == 302) {
    const String location = response.location();
    if (!isGithubAssetRedirect({location.c_str(), location.length()})) return "GitHubのダウンロード先を確認できませんでした。";
    code = response.open(location, "", true);
    authenticated = false;
  }
  if (code != 200) return githubError(code, authenticated);
  const auto length = response.contentLength();
  if (length > 0 && length != release.size) return "配布ファイルのサイズがリリース情報と一致しません。";

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
  if (!buffer) return "ファームウェアの受信に必要な空きメモリーがありません。";
  OtaWrite writer;
  if (!writer.begin(target)) return "更新用領域を開けませんでした。";

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const char* error = mbedtls_sha256_starts(&sha, 0) == 0 ? nullptr : "SHA-256の計算を開始できませんでした。";
  const uint32_t start = millis();
  uint32_t received = 0;
  while (!error && !response.complete()) {
    const int count = response.read(reinterpret_cast<char*>(buffer.get()), 4096);
    if (count == 0 && response.complete()) break;
    if (count <= 0 || millis() - start > maxDownloadMs) { error = "ダウンロードが途中で切断されたか、時間切れになりました。"; break; }
    if (static_cast<uint32_t>(count) > release.size - received) { error = "配布ファイルが指定サイズを超えています。"; break; }
    if (!writer.write(buffer.get(), count)) { error = "ファームウェアを書き込めませんでした。"; break; }
    if (mbedtls_sha256_update(&sha, buffer.get(), count) != 0) { error = "SHA-256の計算に失敗しました。"; break; }
    received += count;
    portENTER_CRITICAL(&stateLock);
    state.received = received;
    portEXIT_CRITICAL(&stateLock);
    delay(1);
  }
  std::array<uint8_t, 32> actual{};
  if (!error && (received != release.size || mbedtls_sha256_finish(&sha, actual.data()) != 0 || actual != release.digest)) {
    error = "配布ファイルのサイズまたはSHA-256が一致しません。";
  }
  mbedtls_sha256_free(&sha);
  if (error) return error;

  setStage(Stage::Verifying, "書き込んだファームウェアを検証しています…");
  if (!writer.finish()) return "ESP32用ファームウェアとして検証できませんでした。";
  if (esp_ota_set_boot_partition(target) != ESP_OK) return "再起動先のファームウェアを設定できませんでした。";
  setStage(Stage::Restarting, "検証が完了しました。本体を再起動します…");
  return nullptr;
}

struct Job { String token; bool install; FirmwareRelease release; };
void firmwareWorker(void* arg) {
  {
    const std::unique_ptr<Job> job(static_cast<Job*>(arg));
    const char* error = ensureClock() ? (job->install ? installRelease(job->token, job->release) : checkRelease(job->token)) :
                                      "時刻を取得できませんでした。インターネット接続を確認してください。";
    if (error) setStage(Stage::Failed, error);
  }
  vTaskDelete(nullptr);
}

bool launch(const String& token, bool install, const FirmwareRelease& release, const char*& error) {
  if (install && release.authenticated && token.isEmpty()) { error = "非公開リリースにアクセスできません。更新を確認し直してください。"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "本体をインターネットに接続できるWi-Fiにつないでください。"; return false; }
  if (!updatePartition()) { error = noPartition; return false; }
  auto* job = new (std::nothrow) Job{token, install, release};
  if (!job || job->token.length() != token.length()) {
    delete job;
    error = "更新処理を開始するための空きメモリーがありません。";
    return false;
  }
  setStage(install ? Stage::Downloading : Stage::Checking, install ? "ファームウェアを取得しています…" : "GitHubで更新を確認しています…");
  if (xTaskCreate(firmwareWorker, "github_ota", 8192, job, 1, nullptr) == pdPASS) return true;
  delete job;
  error = "更新処理を開始できませんでした。";
  setStage(Stage::Failed, error);
  return false;
}
}

bool startFirmwareCheck(const String& token, const char*& error) {
  const auto stage = statusSnapshot().stage;
  if (stage == Stage::Checking || isInstalling(stage)) { error = "更新処理が実行中です。"; return false; }
  return launch(token, false, {}, error);
}

bool startFirmwareInstall(const String& token, const String& version, const char*& error) {
  const auto status = statusSnapshot();
  if (status.stage != Stage::Available || version != status.release.version) { error = "更新を確認し直してください。"; return false; }
  return launch(token, true, status.release, error);
}

bool firmwareInstalling() { return isInstalling(statusSnapshot().stage); }
bool firmwareRestartPending() { return statusSnapshot().stage == Stage::Restarting; }

void firmwareStatus(JsonObject out) {
  static constexpr const char* names[] = {"idle", "checking", "current", "available", "downloading", "verifying", "restarting", "failed"};
  const auto status = statusSnapshot();
  out["version"] = SW_FIRMWARE_VERSION;
  out["repository"] = kGithubRepository;
  out["availableVersion"] = status.release.version;
  out["stage"] = names[static_cast<unsigned>(status.stage)];
  out["message"] = status.message;
  out["received"] = status.received;
  out["size"] = status.release.size;
}

bool pendingFirmwareBoot() {
  esp_ota_img_states_t state;
  return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool confirmFirmwareBoot() { return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK; }

void rollBackFirmwareBoot() { esp_ota_mark_app_invalid_rollback_and_reboot(); }
}
