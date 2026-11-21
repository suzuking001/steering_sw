#include "hid.h"
namespace sw {
HidState hidState;
namespace {
NimBLECharacteristic *keyboard = nullptr, *media = nullptr;
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    if (hidState.suspended) { server->disconnect(info.getConnHandle()); return; }
    hidState.epoch.fetch_add(1); hidState.connected = true; hidState.encrypted = false;
    hidState.keyboardSubscribed = false; hidState.mediaSubscribed = false;
    server->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    hidState.connected = false; hidState.encrypted = false;
    hidState.keyboardSubscribed = false; hidState.mediaSubscribed = false;
    hidState.epoch.fetch_add(1);
    if (!hidState.suspended) NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (hidState.suspended) { NimBLEDevice::getServer()->disconnect(info.getConnHandle()); return; }
    hidState.encrypted = info.isEncrypted();
    if (!info.isEncrypted()) NimBLEDevice::getServer()->disconnect(info.getConnHandle());
  }
} serverCallbacks;
class ReportCallbacks : public NimBLECharacteristicCallbacks {
  bool media_;
 public:
  explicit ReportCallbacks(bool m) : media_(m) {}
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t value) override {
    if (media_) hidState.mediaSubscribed = (value & 1) != 0;
    else hidState.keyboardSubscribed = (value & 1) != 0;
  }
} keyCallbacks(false), mediaCallbacks(true);
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
  hid.setHidInfo(0, 1);
  hid.setBatteryLevel(100);
  hid.setReportMap(reportMap, sizeof(reportMap));
  keyboard = hid.getInputReport(1); media = hid.getInputReport(2);
  hid.getOutputReport(1);
  keyboard->setCallbacks(&keyCallbacks); media->setCallbacks(&mediaCallbacks);
  uint8_t zeros[8] = {}; keyboard->setValue(zeros, 8); media->setValue(zeros, 2);
  server->start();
  auto advertising = NimBLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD); advertising->addServiceUUID(hid.getHidService()->getUUID());
  advertising->enableScanResponse(true);
  advertising->setName(name);
  if (!hidState.suspended) advertising->start();
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
  hidState.connected = false;
  hidState.encrypted = false;
  hidState.keyboardSubscribed = false;
  hidState.mediaSubscribed = false;
  hidState.epoch.fetch_add(1);
  return true;
}
bool hidReady() {
  return !hidState.suspended && hidState.connected && hidState.encrypted && hidState.keyboardSubscribed && hidState.mediaSubscribed;
}
void setHidSuspended(bool suspended) {
  if (hidState.suspended.exchange(suspended) == suspended) return;
  hidState.epoch.fetch_add(1);
  if (!NimBLEDevice::isInitialized()) return;
  auto server = NimBLEDevice::getServer();
  if (suspended) {
    NimBLEDevice::stopAdvertising();
    for (auto handle : server->getPeerDevices()) server->disconnect(handle);
  } else if (!server->getConnectedCount()) NimBLEDevice::startAdvertising();
}
bool sendKey(uint8_t key, uint8_t modifiers) {
  if (!hidReady()) return false;
  uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0};
  keyboard->setValue(report, sizeof(report)); return keyboard->notify();
}
bool sendMedia(uint16_t usage) {
  if (!hidReady()) return false;
  uint8_t report[2] = {uint8_t(usage & 0xff), uint8_t(usage >> 8)};
  media->setValue(report, sizeof(report)); return media->notify();
}
bool releaseHid() {
  uint8_t report[8] = {};
  if (!keyboard || !media) return false;
  keyboard->setValue(report, 8); media->setValue(report, 2);
  if (!hidReady()) return false;
  bool a = keyboard->notify(), b = media->notify(); return a && b;
}
} // namespace sw
