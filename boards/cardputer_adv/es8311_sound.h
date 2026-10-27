// Speaker on the Cardputer ADV: an ES8311 codec (I2C 0x18) fed by I2S.
// Pins and codec set-up as M5Stack's M5Unified does it for this board.
#pragma once
#include <Arduino.h>
#include "hal/sound.h"

class Es8311Sound : public hal::Sound {
public:
  bool begin();
  void play(const hal::Tone* tones, int n) override;
  bool present() const { return _ok; }
  // A pattern is still playing out of the DMA ring.
  bool busy() const { return (int32_t)(_until - millis()) > 0; }

private:
  bool _ok = false;
  uint32_t _until = 0;
};
