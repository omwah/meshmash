// Whether the screen timing out locks the keys, for installs from before the Cardputer had a
// lock button (G0). Its keys always woke the screen then, so an install that already exists keeps
// that; a new one locks. The T-Deck always had the lock and keeps whatever it chose.
#pragma once
#include <stdint.h>

namespace app {

// `lockOff` and `lockSet` are the stored preference: lockSet 0 is a file that predates the choice.
inline void settleLockPref(uint8_t& lockOff, uint8_t& lockSet, bool freshInstall, bool hasTrackball) {
  if (freshInstall) {
    lockOff = 0;
    lockSet = 1;
  } else if (!lockSet) {
    lockSet = 1;
    if (!hasTrackball) lockOff = 1;
  }
}

}  // namespace app
