// Told when the screen goes off and comes back on, so a board can save power
// while nobody is looking (slower clocks, slower polling) and be quick again
// when someone is.
#pragma once

namespace hal {

class IdleHook {
public:
  virtual ~IdleHook() {}
  virtual void setIdle(bool idle) = 0;
};

}  // namespace hal
