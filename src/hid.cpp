#include "hid.h"
#include "ble_reconnect.h"
namespace sw {
HidState hidState;
namespace {
NimBLECharacteristic *keyboard = nullptr, *media = nullptr, *bootKeyboard = nullptr, *protocolMode = nullptr;
constexpr uint8_t bootProtocol = 0, reportProtocol = 1;
constexpr uint16_t fastAdvertisingUnits = 32;  // 20 ms in 0.625 ms units.
constexpr uint16_t slowAdvertisingUnits = 244; // 152.5 ms in 0.625 ms units.
constexpr uint32_t maintenanceIntervalMs = 250;
BleReconnectSchedule reconnect;
uint32_t lastMaintenanceAt = 0;
std::atomic<bool> restartAdvertising{true}, peerBonded{false};
std::atomic<uint16_t> peerHandle{BLE_HS_CONN_HANDLE_NONE};
void clearConnectionState() {
  hidState.encrypted = false;
  hidState.keyboardSubscribed = false;
  hidState.mediaSubscribed = false;
  hidState.bootKeyboardSubscribed = false;
  hidState.bootMode = false;
  hidState.reportMapRead = false;
  peerBonded = false;
}
bool encryptedLink() {
  return !hidState.suspended && hidState.connected && hidState.encrypted;
}
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    peerHandle = info.getConnHandle();
    if (hidState.suspended) { server->disconnect(info.getConnHandle()); return; }
    clearConnectionState();
    protocolMode->setValue(reportProtocol);
    hidState.connected = true;
    hidState.epoch.fetch_add(1);
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    hidState.connected = false;
    clearConnectionState();
    hidState.disconnectReason = reason;
    hidState.epoch.fetch_add(1);
    restartAdvertising = true;
    peerHandle = BLE_HS_CONN_HANDLE_NONE;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (hidState.suspended) { NimBLEDevice::getServer()->disconnect(info.getConnHandle()); return; }
    peerBonded = info.isBonded();
    hidState.encrypted = info.isEncrypted();
    if (!info.isEncrypted()) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
} serverCallbacks;
class ReportCallbacks : public NimBLECharacteristicCallbacks {
  std::atomic<bool>& subscribed_;
 public:
  explicit ReportCallbacks(std::atomic<bool>& subscribed) : subscribed_(subscribed) {}
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
    subscribed_ = (value & 1) != 0;
  }
} keyCallbacks(hidState.keyboardSubscribed), mediaCallbacks(hidState.mediaSubscribed),
  bootKeyCallbacks(hidState.bootKeyboardSubscribed);
class ProtocolCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const auto value = characteristic->getValue();
    if (!info.isEncrypted() || value.size() != 1 || (value[0] != bootProtocol && value[0] != reportProtocol)) {
      characteristic->setValue(hidState.bootMode ? bootProtocol : reportProtocol);
      return;
    }
    const bool boot = value[0] == bootProtocol;
    if (hidState.bootMode.exchange(boot) != boot) hidState.epoch.fetch_add(1);
  }
} protocolCallbacks;
class ReportMapCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic*, NimBLEConnInfo&) override { hidState.reportMapRead = true; }
} reportMapCallbacks;

NimBLECharacteristic* readyKeyboard() {
  if (!encryptedLink()) return nullptr;
  if (hidState.bootMode) return hidState.bootKeyboardSubscribed ? bootKeyboard : nullptr;
  return hidState.keyboardSubscribed ? keyboard : nullptr;
}
// Standard boot-compatible keyboard and 16-bit Consumer Control reports.
uint8_t reportMap[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,
  0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
  0x95,0x01,0x75,0x08,0x81,0x01,
  0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,
  0x95,0x01,0x75,0x03,0x91,0x01,
  0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x73,0x05,0x07,0x19,0x00,0x29,0x73,0x81,0x00,0xC0,
  0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x02,
  0x15,0x00,0x26,0xFF,0x03,0x19,0x00,0x2A,0xFF,0x03,0x75,0x10,0x95,0x01,0x81,0x00,0xC0
};
}
bool beginHid(const std::string& name) {
  if (!NimBLEDevice::init(name)) return false;
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  auto server = NimBLEDevice::createServer(); server->setCallbacks(&serverCallbacks, false);
  NimBLEHIDDevice hid(server);
  hid.setManufacturer("Steering Switch");
  hid.setPnp(0x02, 0, 0, 0x0100);
  hid.setHidInfo(0, 3); // Remote Wake and Normally Connectable.
  hid.setBatteryLevel(100);
  hid.setReportMap(reportMap, sizeof(reportMap));
  keyboard = hid.getInputReport(1); media = hid.getInputReport(2);
  hid.getOutputReport(1);
  auto service = hid.getHidService();
  bootKeyboard = service->createCharacteristic("2a22", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  auto bootOutput = service->createCharacteristic("2a32", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  protocolMode = hid.getProtocolMode();
  bootKeyboard->setCallbacks(&bootKeyCallbacks);
  protocolMode->setCallbacks(&protocolCallbacks);
  service->getCharacteristic("2a4b")->setCallbacks(&reportMapCallbacks);
  keyboard->setCallbacks(&keyCallbacks); media->setCallbacks(&mediaCallbacks);
  uint8_t zeros[8] = {};
  keyboard->setValue(zeros, 8); media->setValue(zeros, 2);
  bootKeyboard->setValue(zeros, 8); bootOutput->setValue(zeros, 1);
  if (!server->start()) return false;
  auto advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD); advertising->addServiceUUID(hid.getHidService()->getUUID());
  advertising->enableScanResponse(true);
  advertising->setName(name);
  reconnect.resetRecovery();
  restartAdvertising = true;
  lastMaintenanceAt = millis() - maintenanceIntervalMs;
  maintainHid();
  return true;
}
bool setHidName(const std::string& name) {
  return NimBLEDevice::isInitialized() && NimBLEDevice::setDeviceName(name) && NimBLEDevice::getAdvertising()->setName(name);
}
bool stopHid() {
  setHidSuspended(true);
  if (!NimBLEDevice::deinit(true)) return false;
  keyboard = nullptr;
  media = nullptr;
  bootKeyboard = nullptr;
  protocolMode = nullptr;
  hidState.connected = false;
  peerHandle = BLE_HS_CONN_HANDLE_NONE;
  clearConnectionState();
  hidState.epoch.fetch_add(1);
  return true;
}
bool hidReady() {
  return hidKeyboardReady() || hidMediaReady();
}
bool hidKeyboardReady() {
  return readyKeyboard() != nullptr;
}
bool hidMediaReady() {
  return encryptedLink() && !hidState.bootMode && hidState.mediaSubscribed;
}
void setHidSuspended(bool suspended) {
  if (hidState.suspended.exchange(suspended) == suspended) return;
  hidState.epoch.fetch_add(1);
  if (!NimBLEDevice::isInitialized()) return;
  auto server = NimBLEDevice::getServer();
  if (suspended) {
    NimBLEDevice::stopAdvertising();
    for (auto handle : server->getPeerDevices()) server->disconnect(handle);
  } else {
    reconnect.resetRecovery();
    restartAdvertising = true;
    lastMaintenanceAt = millis() - maintenanceIntervalMs;
  }
}
void maintainHid() {
  const uint32_t now = millis();
  if (now - lastMaintenanceAt < maintenanceIntervalMs || !NimBLEDevice::isInitialized()) return;
  lastMaintenanceAt = now;
  auto advertising = NimBLEDevice::getAdvertising();
  if (hidState.suspended) {
    if (advertising->isAdvertising()) advertising->stop();
    return;
  }
  if (restartAdvertising.exchange(false)) reconnect.disconnected();
  const uint16_t handle = peerHandle;
  if (handle != BLE_HS_CONN_HANDLE_NONE) {
    const bool ready = hidReady();
    const bool awaitingReports = !ready && peerBonded && hidState.encrypted;
    const auto action = reconnect.update(now, ready, awaitingReports);
    if (action == BleRecoveryAction::None || peerHandle != handle || hidReady()) return;
    auto server = NimBLEDevice::getServer();
    if (action == BleRecoveryAction::RefreshServices) {
      server->sendServiceChangedIndication();
      ++hidState.serviceRefreshes;
    } else if (server->disconnect(handle)) {
      hidState.automaticReconnects.fetch_add(1);
    }
    return;
  }
  if (advertising->isAdvertising()) return;
  const bool fast = reconnect.fastAdvertising();
  const uint16_t interval = fast ? fastAdvertisingUnits : slowAdvertisingUnits;
  advertising->setAdvertisingInterval(interval);
  if (advertising->start(fast ? BleReconnectSchedule::fastAdvertisingMs : 0)) {
    hidState.advertisingIntervalUnits = interval;
    reconnect.advertisingStarted();
  }
}
bool hidAutomaticRetryUsed() { return reconnect.retryUsed(); }
bool sendKey(uint8_t key, uint8_t modifiers) {
  auto reportCharacteristic = readyKeyboard();
  if (!reportCharacteristic) return false;
  uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  reportCharacteristic->setValue(report, sizeof(report)); return reportCharacteristic->notify();
}
bool sendMedia(uint16_t usage) {
  if (!hidMediaReady()) return false;
  uint8_t report[2] = {uint8_t(usage & 0xff), uint8_t(usage >> 8)};
  media->setValue(report, sizeof(report)); return media->notify();
}
void releaseHid() {
  uint8_t report[8] = {};
  if (!keyboard || !media || !bootKeyboard) return;
  keyboard->setValue(report, 8); media->setValue(report, 2);
  bootKeyboard->setValue(report, 8);
  if (auto reportCharacteristic = readyKeyboard()) reportCharacteristic->notify();
  if (hidMediaReady()) media->notify();
}
} // namespace sw
