// The T-Deck's GT911 touch panel on the shared I2C bus. Ported from esp-vim's
// esp_touch.c (Apache-2.0): with no reset line it may answer at 0x5D or 0x14;
// the range it reports in comes from its configuration; the panel is
// portrait, turned to the landscape screen (swapped, mirrored in Y), and
// panels whose Module_Switch1 axis bits differ from the first one checked
// (0xB5) are turned to follow them. Polled, as its interrupt line behaves
// differently between units.
#pragma once
#include "hal/touch.h"

class Gt911Touch : public hal::Touch {
public:
  bool begin() override;
  bool poll(hal::TouchEvent& ev) override;
  bool present() const { return _addr != 0; }
  // While the screen is off a touch only has to wake it: look less often.
  void setSlow(bool slow) { _pollMs = slow ? 80 : 16; }

private:
  bool read(uint16_t reg, uint8_t* out, int n);
  void write(uint16_t reg, uint8_t v);
  int readRaw(int& rx, int& ry);
  uint8_t _addr = 0;
  uint16_t _xMax = 0, _yMax = 0;
  uint8_t _turn = 0;
  bool _down = false;
  int16_t _x = 0, _y = 0;
  uint32_t _lastPoll = 0, _lastMove = 0, _pollMs = 16;
};
