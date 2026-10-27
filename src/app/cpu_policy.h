// Which CPU clock to run: what Settings > CPU speed asks for while the screen is on, and a floor
// while it is off. Plain arithmetic so the rules can be tested on the host; the board applies it.
#pragma once
#include <stdint.h>

namespace app {

struct CpuSpeeds {
  uint32_t board;      // the board's own speed (what it booted at)
  uint32_t balanced;   // Settings: balanced
  uint32_t saver;      // Settings: battery saver
  uint32_t idle;       // screen off or locked; never below this (0: leave the clock alone)
};

// `level`: 0 the board's own, 1 balanced, 2 or more battery saver. Never above the board's speed.
inline uint32_t cpuTargetMhz(const CpuSpeeds& s, uint8_t level, bool screenOff) {
  if (screenOff && s.idle) return s.board < s.idle ? s.board : s.idle;
  uint32_t mhz = level == 1 ? s.balanced : level >= 2 ? s.saver : s.board;
  return mhz < s.board ? mhz : s.board;
}

}  // namespace app
