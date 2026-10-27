// The one owner of the CPU clock on an ESP32 board. The clock follows two things: the speed
// chosen for the active UI (Settings > CPU speed: the board's own, 160 MHz, or 80 MHz) and whether
// the screen is off. Screen off or locked never goes below 160 MHz, which still verifies an
// advert in good time for the radio's one packet buffer; the peripheral bus stays at 80 MHz, so
// the radio, display and I2C are unaffected. -D MC_IDLE_CPU_MHZ=0 leaves the idle clock alone.
#pragma once
#include <Arduino.h>
#include "app/cpu_policy.h"

#ifndef MC_IDLE_CPU_MHZ
#define MC_IDLE_CPU_MHZ 160
#endif
#ifndef MC_CPU_BALANCED_MHZ
#define MC_CPU_BALANCED_MHZ 160
#endif
#ifndef MC_CPU_SAVER_MHZ
#define MC_CPU_SAVER_MHZ 80
#endif

class CpuClock {
public:
  // The board's own speed is whatever it booted at.
  void begin() { _board = _now = getCpuFrequencyMhz(); }
  // The screen went off (or came on): the idle clock, or back to the UI's.
  void setIdle(bool idle) {
    _idle = idle;
    apply();
  }
  // Settings > CPU speed: 0 board speed, 1 balanced, 2 battery saver.
  void setLevel(uint8_t level) {
    _level = level;
    apply();
  }
  // How long the CPU has run on the idle clock since boot, for Diagnostics: it shows the mode works.
  uint32_t lowSeconds() const { return (_lowMs + (_low ? millis() - _lowSince : 0)) / 1000; }

private:
  uint32_t target() const {
    return app::cpuTargetMhz({_board, MC_CPU_BALANCED_MHZ, MC_CPU_SAVER_MHZ, MC_IDLE_CPU_MHZ}, _level, _idle);
  }
  void apply() {
    if (!_board) return;   // begin() has not run
    uint32_t want = target(), now = millis();
    if (want != _now) {
      setCpuFrequencyMhz(want);
      _now = want;
    }
    bool low = _idle && _now < _board;
    if (low != _low) {
      if (_low) _lowMs += now - _lowSince;   // the stretch that ends now
      _low = low;
      _lowSince = now;
    }
  }

  uint32_t _board = 0, _now = 0;
  uint8_t _level = 0;
  bool _idle = false, _low = false;
  uint32_t _lowMs = 0, _lowSince = 0;
};
