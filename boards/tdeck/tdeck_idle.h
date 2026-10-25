// What the T-Deck does while the screen is off: a slower CPU clock (the
// peripheral bus stays at 80 MHz, so the radio, display and I2C are unaffected;
// 160 MHz still verifies an advert in good time for the radio's one packet
// buffer) and slower polling of the keys and the touch panel, which only have to
// wake the screen. -D MC_IDLE_CPU_MHZ=0 leaves the clock alone.
#pragma once
#include <Arduino.h>
#include "hal/idle.h"
#include "gt911_touch.h"
#include "tdeck_keyboard.h"

#ifndef MC_IDLE_CPU_MHZ
#define MC_IDLE_CPU_MHZ 160
#endif

class TDeckIdle : public hal::IdleHook {
public:
  TDeckIdle(TDeckKeyboard& k, Gt911Touch& t) : _k(k), _t(t) {}
  void begin() { _active = getCpuFrequencyMhz(); }
  void setIdle(bool idle) override {
    _k.setSlow(idle);
    _t.setSlow(idle);
#if MC_IDLE_CPU_MHZ
    if (_active > MC_IDLE_CPU_MHZ) setCpuFrequencyMhz(idle ? MC_IDLE_CPU_MHZ : _active);
#endif
  }

private:
  TDeckKeyboard& _k;
  Gt911Touch& _t;
  uint32_t _active = 240;
};
