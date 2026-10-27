#include "i2s_sound.h"
#include <Arduino.h>
#include <driver/i2s.h>

static constexpr i2s_port_t PORT = I2S_NUM_0;
static constexpr int RATE = 8000;
static constexpr int DMA_FRAMES = 256, DMA_BUFS = 4;   // 1024 frames = 128 ms of sound
static constexpr int16_t AMPLITUDE = 4000;

bool I2sSound::begin() {
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
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  _ok = i2s_set_pin(PORT, &pins) == ESP_OK;
  return _ok;
}

void I2sSound::play(const hal::Tone* tones, int n) {
  if (!_ok) return;
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
