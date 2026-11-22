#pragma once
#include <stdint.h>

enum SwBleTraceEvent {
  SW_BLE_CONNECTED, SW_BLE_DISCONNECTED, SW_BLE_SMP_TX, SW_BLE_SMP_RX,
  SW_BLE_LTK_REQUEST, SW_BLE_LTK_LOOKUP, SW_BLE_CONTROLLER_ENCRYPTION,
  SW_BLE_SMP_TIMEOUT, SW_BLE_SECURITY_RESULT, SW_BLE_HID_SUBSCRIBE
};

#ifdef __cplusplus
extern "C" {
#endif
void sw_ble_trace(enum SwBleTraceEvent event, uint16_t handle, uint8_t detail, int value);
#ifdef __cplusplus
}
#ifdef SW_BLE_TRACE
#include <ArduinoJson.h>
void writeBleTrace(JsonArray out);
#endif
#endif
