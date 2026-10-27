// Turns successive keys-held reports into key down/up events: a usage in the
// new report and not the old one went down, and the reverse went up. Shared
// by the Cardputer driver and the simulator so both feed the UI identically.
#pragma once
#include <string.h>
#include "kbd_report.h"
#include "hal/keyboard.h"

class ReportDiff {
public:
  // Queue the events between the last report and `r`.
  void update(const kbd_report_t& r) {
    for (int i = 0; i < _prev.n; i++)
      if (!has(r, _prev.key[i])) push({_prev.key[i], _prev.mods, false});
    for (int i = 0; i < r.n; i++)
      if (!has(_prev, r.key[i])) push({r.key[i], r.mods, true});
    _prev = r;
  }
  // Forget everything: nothing queued, no key held (a lock discards what was typed).
  void clear() { _head = _n = 0; _prev = kbd_report_t{}; }
  bool next(hal::KeyEvent& ev) {
    if (!_n) return false;
    ev = _q[_head];
    _head = (_head + 1) % Q;
    _n--;
    return true;
  }

private:
  static bool has(const kbd_report_t& r, uint8_t u) {
    for (int i = 0; i < r.n; i++)
      if (r.key[i] == u) return true;
    return false;
  }
  void push(const hal::KeyEvent& e) {
    if (_n == Q) return;
    _q[(_head + _n) % Q] = e;
    _n++;
  }
  static constexpr int Q = 16;
  kbd_report_t _prev{};
  hal::KeyEvent _q[Q];
  int _head = 0, _n = 0;
};
