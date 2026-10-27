// What the T-Deck does while the screen is off: a slower CPU clock (see CpuClock) and slower
// polling of the keys and the touch panel, which only have to wake the screen. The clock also
// follows Settings > CPU speed while the screen is on.
#pragma once
#include <Arduino.h>
#include "hal/idle.h"
#include "cpu_clock.h"
#include "gt911_touch.h"
#include "tdeck_keyboard.h"

class TDeckIdle : public hal::IdleHook {
public:
  TDeckIdle(TDeckKeyboard& k, Gt911Touch& t) : _k(k), _t(t) {}
  void begin() { _clock.begin(); }
  void setIdle(bool idle) override {
    _k.setSlow(idle);
    _t.setSlow(idle);
    _clock.setIdle(idle);
  }
  void setSpeed(uint8_t level) override { _clock.setLevel(level); }
  uint32_t lowSeconds() const { return _clock.lowSeconds(); }

private:
  TDeckKeyboard& _k;
  Gt911Touch& _t;
  CpuClock _clock;
};
