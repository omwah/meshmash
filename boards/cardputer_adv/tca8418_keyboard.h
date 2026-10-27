// The Cardputer ADV keyboard: a TCA8418 key-scan controller (I2C 0x34,
// interrupt on GPIO11) wired as 7 rows x 8 columns. Register sequence ported
// from esp-vim's esp_kbd_cardputer_dev.c (Apache-2.0) to Arduino Wire; key
// mapping is cardputer_keymap.c.
#pragma once
#include "hal/keyboard.h"
extern "C" {
#include "keymap/cardputer_keymap.h"
}
#include "report_diff.h"

class Tca8418Keyboard : public hal::Keyboard {
public:
  bool begin() override;
  bool poll(hal::KeyEvent& ev) override;
  void setTextMode(bool on) override { cardputer_keymap_text_mode(&_st, on); }
  bool present() const { return _ok; }
  // G0 (GPIO0, the button beside the keys) unlocks, and held locks: the T-Deck's trackball click.
  bool hasUnlockButton() const override { return true; }
  const char* unlockHow() const override { return "press G0"; }
  bool unlockPressed() override { bool u = _unlock; _unlock = false; return u; }
  bool lockPressed() override { bool l = _lock; _lock = false; return l; }
  void setLocked(bool locked) override;
  bool g0Down() const { return _g0Down; }
  // Feed a matrix event as if the key were pressed (bring-up diagnostics).
  void inject(int row, int col, bool down);

private:
  void drain();
  void g0();
  void flush();
  cardputer_keymap_t _st{};
  ReportDiff _diff;
  bool _ok = false;
  bool _locked = false, _unlock = false, _lock = false;
  // G0: down (debounced), and spent: this press already did what it could, so it needs to be
  // released before it can do anything else (a hold that locked must not then unlock).
  bool _g0Down = false, _g0Spent = false, _g0Reported = false;
  uint32_t _g0From = 0;
  uint32_t _lastPoll = 0;
};
