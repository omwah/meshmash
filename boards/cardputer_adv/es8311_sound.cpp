#include "es8311_sound.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

static constexpr uint8_t CODEC = 0x18;
static constexpr i2s_port_t PORT = I2S_NUM_1;
static constexpr int RATE = 8000;
static constexpr int DMA_FRAMES = 256, DMA_BUFS = 4;   // 1024 frames = 128 ms of sound
static constexpr int16_t AMPLITUDE = 6000;

bool Es8311Sound::begin() {
  // ES8311: power up, MCLK from BCLK (x8), analog + DAC on, headphone driver
  // on, DAC at 0 dB, equalizer bypassed. Values from M5Unified's
  // _speaker_enabled_cb_cardputer_adv.
  static const uint8_t regs[][2] = {
      {0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
      {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xBF}, {0x37, 0x08},
  };
  for (auto& r : regs) {
    Wire.beginTransmission(CODEC);
    Wire.write(r[0]);
    Wire.write(r[1]);
    if (Wire.endTransmission() != 0) return false;
  }
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = DMA_BUFS;
  cfg.dma_buf_len = DMA_FRAMES;
  cfg.tx_desc_auto_clear = true;   // silence when nothing is queued
  if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = 41;
  pins.ws_io_num = 43;
  pins.data_out_num = 42;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  _ok = i2s_set_pin(PORT, &pins) == ESP_OK;
  return _ok;
}

void Es8311Sound::play(const hal::Tone* tones, int n) {
  if (!_ok) return;
  // Square waves written straight into the DMA ring: the whole pattern fits
  // (128 ms), so this returns at once and the radio loop never waits.
  int16_t frame[64 * 2];
  int budget = DMA_FRAMES * DMA_BUFS;
  uint32_t total = 0;
  for (int t = 0; t < n && budget > 0; t++) {
    int frames = RATE * tones[t].ms / 1000;
    if (frames > budget) frames = budget;
    budget -= frames;
    total += (uint32_t)frames * 1000 / RATE;
    int half = tones[t].hz ? RATE / (2 * tones[t].hz) : 0;
    int phase = 0;
    while (frames > 0) {
      int k = frames < 64 ? frames : 64;
      for (int i = 0; i < k; i++) {
        int16_t v = 0;
        if (half) {
          v = (phase / half) & 1 ? -AMPLITUDE : AMPLITUDE;
          phase++;
        }
        frame[2 * i] = frame[2 * i + 1] = v;
      }
      size_t written = 0;
      i2s_write(PORT, frame, (size_t)k * 4, &written, 0);
      frames -= k;
    }
  }
  _until = millis() + total;
}
