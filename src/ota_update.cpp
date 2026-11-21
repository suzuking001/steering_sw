#include "ota_update.h"
#include "ota_release.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_tls_errors.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <WiFi.h>
#include <cerrno>
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
  char detail[96] = "";
};
Status state;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;

Status statusSnapshot() {
  portENTER_CRITICAL(&stateLock);
  const Status snapshot = state;
  portEXIT_CRITICAL(&stateLock);
  return snapshot;
}

void setStage(Stage stage, const char* message = "", const FirmwareRelease* release = nullptr, const char* detail = "") {
  portENTER_CRITICAL(&stateLock);
  state.stage = stage;
  state.message = message;
  snprintf(state.detail, sizeof(state.detail), "%s", detail);
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

bool isBusy(Stage stage) { return stage == Stage::Checking || isInstalling(stage); }

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
    config.buffer_size_tx = githubRequestTxBufferSize(url.length());
    // Covers the TCP connect and the TLS handshake, which a weak 2.4GHz link can
    // stretch past eight seconds on the first attempt.
    config.timeout_ms = 20000;
    config.disable_auto_redirect = true;
    config.event_handler = event;
    config.user_data = this;
    client_ = esp_http_client_init(&config);
    if (!client_) { error_ = ESP_ERR_NO_MEM; return -1; }
    const int scheme = url.indexOf("://");
    const int hostStart = scheme < 0 ? 0 : scheme + 3;
    const int hostEnd = url.indexOf('/', hostStart);
    host_ = url.substring(hostStart, hostEnd < 0 ? url.length() : hostEnd);
    const bool apiRequest = url.startsWith(apiHost);
    if ((apiRequest && esp_http_client_set_header(client_, "X-GitHub-Api-Version", "2026-03-10") != ESP_OK) ||
        esp_http_client_set_header(client_, "User-Agent", "Steering-Switch/" SW_FIRMWARE_VERSION) != ESP_OK ||
        esp_http_client_set_header(client_, "Accept", binary ? "application/octet-stream" : "application/vnd.github+json") != ESP_OK) {
      error_ = ESP_FAIL;
      return -1;
    }
    String authorization;
    if (!token.isEmpty()) {
      if (!apiRequest) { error_ = ESP_ERR_INVALID_ARG; return -1; }
      authorization = "Bearer " + token;
      if (authorization.length() != token.length() + 7 ||
          esp_http_client_set_header(client_, "Authorization", authorization.c_str()) != ESP_OK) { error_ = ESP_FAIL; return -1; }
    }
    if (esp_http_client_open(client_, 0) != ESP_OK || esp_http_client_fetch_headers(client_) < 0) {
      error_ = esp_http_client_get_errno(client_);
      esp_http_client_get_and_clear_last_tls_error(client_, &tlsError_, &tlsFlags_);
      return -1;
    }
    return esp_http_client_get_status_code(client_);
  }

  int read(char* buffer, size_t length) { return esp_http_client_read(client_, buffer, length); }
  int64_t contentLength() const { return esp_http_client_get_content_length(client_); }
  bool complete() const { return esp_http_client_is_complete_data_received(client_); }
  const String& location() const { return location_; }
  const String& host() const { return host_; }
  int transportError() const { return error_; }
  int tlsError() const { return tlsError_; }
  int tlsFlags() const { return tlsFlags_; }

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
    host_ = "";
    error_ = tlsError_ = tlsFlags_ = 0;
  }
  esp_http_client_handle_t client_ = nullptr;
  String location_, host_;
  int error_ = 0, tlsError_ = 0, tlsFlags_ = 0;
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

// Filled by the single running worker and handed to the next setStage() call.
char transportDetail[96];

// A connect failure never reaches GitHub, so no HTTP status names it. The TLS
// layer's own error and certificate flags are the only clues, so keep them visible.
const char* transportFailure(const GithubResponse& response) {
  IPAddress resolved;
  const bool dns = WiFi.hostByName(response.host().c_str(), resolved);
  const int status = WiFi.status();
  const int errno_ = response.transportError(), tls = response.tlsError(), flags = response.tlsFlags();
  Serial.printf("GitHub OTA: %s に接続できませんでした (errno=0x%x, TLS=0x%x, 証明書=%x, DNS=%s, Wi-Fi=%d, freeHeap=%u)\n",
                response.host().c_str(), errno_, tls, flags, dns ? "成功" : "失敗", status,
                static_cast<unsigned>(ESP.getFreeHeap()));
  char detail[64];
  snprintf(detail, sizeof(detail), "errno=0x%x TLS=0x%x 証明書=%x DNS=%s 空きRAM=%uKB", errno_, tls, flags,
           dns ? "成功" : "失敗", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  snprintf(transportDetail, sizeof(transportDetail), "%s", detail);
  if (!dns) return "Wi-Fi上でGitHubのドメイン名を解決できませんでした。ルーターのDNS設定とインターネット接続を確認してください。";
  if (status != WL_CONNECTED) return "通信の途中でWi-Fiが切断されました。接続状態を確認して再試行してください。";
  if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
    return "本体の時刻がずれているためGitHubの証明書を受け入れられません。インターネットに接続できるWi-Fiで再試行してください。";
  if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) return "GitHubの証明書を信頼済みの一覧で確認できませんでした。";
  if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) return "GitHubのサーバー名が証明書と一致しませんでした。";
  if (tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || tls == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT)
    return "GitHubとの暗号通信が時間内に確立できませんでした。電波の状態の良い場所で再試行してください。";
  if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME || tls == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST)
    return "GitHubのサーバー（443番ポート）に到達できませんでした。ルーターの通信制限や端末分離を確認してください。";
  if (errno_ == ENOMEM) return "暗号通信を開始するための空きメモリーがありません。";
  return "GitHubとHTTPS通信を確立できませんでした。画面の状態表示にある詳細を確認してください。";
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
    if (code < 0) return transportFailure(response);
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
  if (code < 0) return transportFailure(response);
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
    transportDetail[0] = '\0';
    const char* error = ensureClock() ? (job->install ? installRelease(job->token, job->release) : checkRelease(job->token)) :
                                      "時刻を取得できませんでした。インターネット接続を確認してください。";
    if (error) setStage(Stage::Failed, error, nullptr, transportDetail);
  }
  vTaskDelete(nullptr);
}

bool launch(const String& token, bool install, const FirmwareRelease& release, bool (*prepareNetwork)(), const char*& error) {
  if (install && release.authenticated && token.isEmpty()) { error = "非公開リリースにアクセスできません。更新を確認し直してください。"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "本体をインターネットに接続できるWi-Fiにつないでください。"; return false; }
  if (!updatePartition()) { error = noPartition; return false; }
  // Free the BLE host and controller buffers before TLS allocates its handshake workspace.
  if (!prepareNetwork()) { error = "更新のためにBluetoothを一時停止できませんでした。"; return false; }
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

bool startFirmwareCheck(const String& token, bool (*prepareNetwork)(), const char*& error) {
  if (firmwareBusy()) { error = "更新処理が実行中です。"; return false; }
  return launch(token, false, {}, prepareNetwork, error);
}

bool startFirmwareInstall(const String& token, const String& version, bool (*prepareNetwork)(), const char*& error) {
  const auto status = statusSnapshot();
  if (status.stage != Stage::Available || version != status.release.version) { error = "更新を確認し直してください。"; return false; }
  return launch(token, true, status.release, prepareNetwork, error);
}

bool firmwareBusy() { return isBusy(statusSnapshot().stage); }
bool firmwareRestartPending() { return statusSnapshot().stage == Stage::Restarting; }

void firmwareStatus(JsonObject out) {
  static constexpr const char* names[] = {"idle", "checking", "current", "available", "downloading", "verifying", "restarting", "failed"};
  const auto status = statusSnapshot();
  out["version"] = SW_FIRMWARE_VERSION;
  out["repository"] = kGithubRepository;
  out["availableVersion"] = std::string(status.release.version);
  out["stage"] = names[static_cast<unsigned>(status.stage)];
  out["message"] = status.message;
  out["detail"] = std::string(status.detail);
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
