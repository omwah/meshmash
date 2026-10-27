// Keyboard seen by the UI: key down/up events as HID usages (page 7) with
// the modifiers they went down with. Boards translate their matrix into this.
#pragma once
#include <stdint.h>

namespace hal {

struct KeyEvent {
  uint8_t usage;   // HID usage: a-z 0x04.., Enter 0x28, Esc 0x29, arrows 0x4f-0x52
  uint8_t mods;    // KBD_MOD_* style bits (ctrl 0x11, shift 0x22, alt 0x44)
  bool down;
};

class Keyboard {
public:
  virtual ~Keyboard() {}
  virtual bool begin() = 0;
  // Next pending event, if any. Called from the UI loop.
  virtual bool poll(KeyEvent& ev) = 0;
  // Text entry vs. navigation layout (boards with dual-purpose keys).
  virtual void setTextMode(bool on) { (void)on; }

  // A dedicated unlock button, on boards that have one (the T-Deck's trackball
  // click). The UI locks itself when the screen times out only if there is one.
  virtual bool hasUnlockButton() const { return false; }
  // What to do to unlock, for the hint while locked ("click the trackball").
  virtual const char* unlockHow() const { return "press the unlock button"; }
  // True once for each press of it. The press also arrives as an ordinary key
  // event (an Enter), so the UI tells the two apart by this.
  virtual bool unlockPressed() { return false; }
  // True once when that button has been held long enough to mean "lock now"
  // (it then sends no Enter).
  virtual bool lockPressed() { return false; }
  // The UI is locked: keys and rolls are not activity, so a pocket full of
  // them must not light the key backlight. The unlock button still counts.
  virtual void setLocked(bool locked) { (void)locked; }
};

}  // namespace hal
