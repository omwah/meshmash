// Device services for the adapter: heap figures, GPS, SD card, reboot. For
// any ESP32 board whose target.h names `board`, `sensors`, `gps` and the SPI
// bus the SD card is on (`loraSpi`, with PIN_SD_CS).
#pragma once
#include <SD.h>
#include <esp_random.h>
#include "app/platform.h"
#include "app/power_stats.h"

class Esp32Platform : public app::Platform {
public:
  uint32_t heapFree() override;
  uint32_t heapMin() override;
  uint32_t heapMaxBlock() override;
  uint16_t batteryMv() override;
  model::Gps gps() override;
  void setGpsPower(bool on) override;
  fs::FS* sd() override;
  void sdSpace(uint32_t& usedKb, uint32_t& totalKb) override;
  uint32_t cpuMhz() override;
  uint32_t lowClockSeconds() override { return lowClockSecondsFn ? lowClockSecondsFn() : 0; }
  uint32_t (*lowClockSecondsFn)() = nullptr;
  model::Power power() override;
  void resetPower() override { stats.reset(millis()); }
  // Call once per loop pass: counts screen-off time and takes a rested battery reading when one
  // is due. `txCount` is the radio's packets sent so far, `audioBusy` a sound playing.
  void powerTick(bool screenOff, uint32_t txCount, bool audioBusy);
  app::PowerStats stats;
  void reboot() override;
  uint32_t savedBlePin() override;
  void saveBlePin(uint32_t pin) override;
  void clearBlePin() override;
  bool blePairing() override { return blePairingFn && blePairingFn(); }
  bool (*blePairingFn)() = nullptr;
  void randomBytes(uint8_t* out, int n) override { esp_fill_random(out, n); }

private:
  bool _sdTried = false, _sdOk = false;
  uint32_t _sdCheckAt = 0;
  uint32_t _txCount = 0, _txAt = 0;
};
