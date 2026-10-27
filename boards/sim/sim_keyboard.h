// Simulated keyboard: keys are pressed by their printed legend at their place
// in the real matrix and go through the same keymap code as the device, so
// the simulator types and navigates exactly like it. The Cardputer's 4x14
// keys, or with MC_BOARD_TDECK the T-Deck's 5x7 and its trackball.
#pragma once
#include <stdint.h>
#include "hal/keyboard.h"
extern "C" {
#ifdef MC_BOARD_TDECK
#include "tdeck_keymap.h"
#else
#include "cardputer_keymap.h"
#endif
}
#include "report_diff.h"

namespace sim {

// Keys pressed together: the last one is the key, those before it are held
// around it (modifiers). A row of BALL is a trackball roll, col its direction
// (the T-Deck's col 4 is its click).
struct Chord {
  static constexpr int8_t BALL = -1;
  int n = 0;
  int8_t row[3], col[3];
};

class SimKeyboard : public hal::Keyboard {
public:
  explicit SimKeyboard(const uint32_t* nowMs);

  bool begin() override { return true; }
  void setTextMode(bool on) override;
  bool poll(hal::KeyEvent& ev) override;
  bool hasUnlockButton() const override { return true; }   // the trackball's click, or G0
#ifdef MC_BOARD_TDECK
  const char* unlockHow() const override { return "click the trackball"; }
#else
  const char* unlockHow() const override { return "press G0"; }
#endif
  bool unlockPressed() override { bool u = _unlock; _unlock = false; return u; }
  bool lockPressed() override { bool l = _lock; _lock = false; return l; }

  // Schedule a matrix change (or a trackball roll) at an absolute time.
  void at(uint32_t ms, int row, int col, bool down);
  // A key by its printed legend ("a", ";", "enter", "fn", "esc"...), an alias
  // for what it does ("up", "menu"), or mod+key ("ctrl+o").
  static bool find(const char* legend, Chord& out);
  // The keys that type character `c`.
  static bool findChar(char c, Chord& out);
  uint32_t lastScheduled() const { return _last; }

private:
  struct Ev {
    uint32_t ms;
    int8_t row, col;
    bool down;
  };
  static constexpr int Q = 256;
  Ev _q[Q];
  int _n = 0;
  uint32_t _last = 0;
  const uint32_t* _now;
#ifdef MC_BOARD_TDECK
  tdeck_keymap_t _st{};
#else
  cardputer_keymap_t _st{};
#endif
  ReportDiff _diff;
  bool _unlock = false, _lock = false;
};

}  // namespace sim
