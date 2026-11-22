#ifdef SW_BLE_TRACE
#include <Arduino.h>
#include "ble_trace.h"

namespace {
struct Event {
  uint32_t at;
  uint16_t handle;
  SwBleTraceEvent event;
  uint8_t detail;
  int value;
};
constexpr size_t capacity = 64;
Event events[capacity];
size_t next = 0, count = 0;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
constexpr const char* names[] = {
  "connected", "disconnected", "smp_tx", "smp_rx", "ltk_request", "ltk_lookup",
  "controller_encryption", "smp_timeout", "security_result", "hid_subscribe"
};
}

extern "C" void sw_ble_trace(SwBleTraceEvent event, uint16_t handle, uint8_t detail, int value) {
  const uint32_t at = millis();
  portENTER_CRITICAL(&mutex);
  events[next] = {at, handle, event, detail, value};
  next = (next + 1) % capacity;
  if (count < capacity) ++count;
  portEXIT_CRITICAL(&mutex);
}

void writeBleTrace(JsonArray out) {
  Event snapshot[capacity];
  portENTER_CRITICAL(&mutex);
  const size_t size = count;
  const size_t first = (next + capacity - size) % capacity;
  for (size_t i = 0; i < size; ++i) snapshot[i] = events[(first + i) % capacity];
  portEXIT_CRITICAL(&mutex);
  for (size_t i = 0; i < size; ++i) {
    const auto& entry = snapshot[i];
    auto row = out.add<JsonObject>();
    row["atMs"] = entry.at;
    row["handle"] = entry.handle;
    row["event"] = names[entry.event];
    row["detail"] = entry.detail;
    row["value"] = entry.value;
  }
}
#endif
