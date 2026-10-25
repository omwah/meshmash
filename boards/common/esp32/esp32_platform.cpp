#include "esp32_platform.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "target.h"

uint32_t Esp32Platform::heapFree() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t Esp32Platform::heapMin() { return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); }
uint32_t Esp32Platform::heapMaxBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }

model::Gps Esp32Platform::gps() {
  model::Gps g;
  memset(&g, 0, sizeof g);
  // Not gps.isEnabled(): with no enable pin it always says yes. Whether the
  // sensor manager is reading the receiver is its "gps" setting.
  const char* on = sensors.getSettingByKey("gps");
  g.enabled = on && !strcmp(on, "1");
  g.fix = ::gps.isValid();
  g.sats = (uint8_t)::gps.satellitesCount();
  g.lat_e6 = ::gps.getLatitude();
  g.lon_e6 = ::gps.getLongitude();
  g.altCm = ::gps.getAltitude() / 10;   // provider reports mm
  long ts = ::gps.getTimestamp();
  g.lastFixS = g.fix && ts > 0 ? (uint32_t)ts : 0;
  return g;
}

void Esp32Platform::setGpsPower(bool on) {
  // MeshCore's sensor manager owns the GPS UART; the setting it listens to is "gps".
  sensors.setSettingValue("gps", on ? "1" : "0");
}

fs::FS* Esp32Platform::sd() {
  // Card detection: try to mount at most every 5 s while absent; after a
  // successful mount, check the card is still answering.
  uint32_t now = millis();
  if (_sdOk) {
    if (now - _sdCheckAt > 5000) {
      _sdCheckAt = now;
      if (SD.cardType() == CARD_NONE) { SD.end(); _sdOk = false; }
    }
    return _sdOk ? &SD : nullptr;
  }
  if (_sdTried && now - _sdCheckAt < 5000) return nullptr;
  _sdTried = true;
  _sdCheckAt = now;
  _sdOk = SD.begin(PIN_SD_CS, loraSpi, 16000000);   // shared with LoRa; RadioLib drives its own CS
  // The project's folder before it was called MeshMash: backups and history
  // move over once.
  if (_sdOk && !SD.exists("/meshmash") && SD.exists("/meshcore-adv-ui")) SD.rename("/meshcore-adv-ui", "/meshmash");
  return _sdOk ? &SD : nullptr;
}

void Esp32Platform::sdSpace(uint32_t& usedKb, uint32_t& totalKb) {
  usedKb = totalKb = 0;
  if (!_sdOk) return;
  usedKb = (uint32_t)(SD.usedBytes() / 1024);
  totalKb = (uint32_t)(SD.totalBytes() / 1024);
}

void Esp32Platform::reboot() { ESP.restart(); }

uint16_t Esp32Platform::batteryMv() { return board.getBattMilliVolts(); }
