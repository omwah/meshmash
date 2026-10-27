// Told when the screen goes off and comes back on, so a board can save power
// while nobody is looking (slower clocks, slower polling) and be quick again
// when someone is.
#pragma once

#include <stdint.h>

namespace hal {

class IdleHook {
public:
  virtual ~IdleHook() {}
  virtual void setIdle(bool idle) = 0;
  // Settings > CPU speed for the active UI: 0 the board's own, 1 balanced, 2 battery saver.
  virtual void setSpeed(uint8_t level) { (void)level; }
};

}  // namespace hal
