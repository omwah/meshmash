// What the Cardputer does while the screen is off: the slower idle clock (see CpuClock), which also
// follows Settings > CPU speed while the screen is on. Key scanning is already cheap.
#pragma once
#include "hal/idle.h"
#include "cpu_clock.h"

class CardputerIdle : public hal::IdleHook {
public:
  void begin() { _clock.begin(); }
  void setIdle(bool idle) override { _clock.setIdle(idle); }
  void setSpeed(uint8_t level) override { _clock.setLevel(level); }
  uint32_t lowSeconds() const { return _clock.lowSeconds(); }

private:
  CpuClock _clock;
};
