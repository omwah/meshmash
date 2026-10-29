#include "esp32_platform.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#if defined(BLE_PIN_CODE)
#include <NimBLEDevice.h>
#include "nimble_serial.h"
#endif
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

uint32_t Esp32Platform::cpuMhz() { return getCpuFrequencyMhz(); }

void Esp32Platform::reboot() { ESP.restart(); }

#if defined(BLE_PIN_CODE)   // the USB-only builds have no Bluetooth and no use for NVS
// In NVS, which a firmware install leaves alone; the settings file does not survive one.
uint32_t Esp32Platform::savedBlePin() {
  nvs_handle_t h;
  uint32_t pin = 0;
  if (nvs_open("meshmash", NVS_READONLY, &h) != ESP_OK) return 0;
  if (nvs_get_u32(h, "blepin", &pin) != ESP_OK) pin = 0;
  nvs_close(h);
  return pin;
}

void Esp32Platform::saveBlePin(uint32_t pin) {
  nvs_handle_t h;
  if (nvs_open("meshmash", NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_set_u32(h, "blepin", pin) == ESP_OK) nvs_commit(h);
  nvs_close(h);
}
#else
uint32_t Esp32Platform::savedBlePin() { return 0; }
void Esp32Platform::saveBlePin(uint32_t) {}
#endif

model::Power Esp32Platform::power() {
  model::Power p = stats.snapshot(millis());
  p.cpuMhz = cpuMhz();
  p.lowClockS = lowClockSeconds();
#ifdef MC_BOARD_TDECK   // Bluetooth pairing probes (see docs/POWER-PLAN.md), until that bug is found
  nvs_stats_t ns;
  if (nvs_get_stats(nullptr, &ns) == ESP_OK) {
    p.nvsUsed = (uint16_t)ns.used_entries;
    p.nvsTotal = (uint16_t)ns.total_entries;
  }
#if defined(BLE_PIN_CODE)
  p.bleBonds = (int8_t)NimBLEDevice::getNumBonds();
  p.bleConnects = bleLastPeer.connects;
  p.bleAuthOk = bleLastPeer.authOk;
  p.bleAuthFail = bleLastPeer.authFail;
  p.blePeerOta = bleLastPeer.otaType;
  p.blePeerId = bleLastPeer.idType;
  p.blePeerBonded = bleLastPeer.bonded;
#endif
#endif
  return p;
}

static bool usbHostAttached() {
#if ARDUINO_USB_MODE == 1 && ARDUINO_USB_CDC_ON_BOOT
  return HWCDC::isConnected();
#else
  return false;
#endif
}

void Esp32Platform::powerTick(bool screenOff, uint32_t txCount, bool audioBusy) {
  uint32_t now = millis();
  stats.screen(screenOff, now);
  if (txCount != _txCount) { _txCount = txCount; _txAt = now; }
  if (!screenOff) return;
  bool quiet = !audioBusy && now - _txAt >= 5000;
  if (!stats.sampleDue(now, quiet)) return;
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += board.getBattMilliVolts();
  stats.sample(sum / 8, usbHostAttached(), now);
}

uint16_t Esp32Platform::batteryMv() { return board.getBattMilliVolts(); }
