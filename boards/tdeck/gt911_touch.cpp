#include "gt911_touch.h"
#include <Arduino.h>
#include <Wire.h>

static constexpr uint16_t GT_STATUS = 0x814E;    // ready (bit 7), touches; then points
static constexpr uint16_t GT_X_MAX = 0x8048;     // configuration: the reported range
static constexpr uint16_t GT_SWITCH1 = 0x804D;   // configuration: Module_Switch1
static constexpr uint8_t SWITCH_REF = 0xB5;      // the first T-Deck Plus panel checked
static constexpr uint8_t TURN_SWAP = 1, TURN_MX = 2, TURN_MY = 4;
static constexpr int W = 320, H = 240;

bool Gt911Touch::read(uint16_t reg, uint8_t* out, int n) {
  Wire.beginTransmission(_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(_addr, (uint8_t)n) != n) return false;
  for (int i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

void Gt911Touch::write(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(_addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  Wire.write(v);
  Wire.endTransmission();
}

bool Gt911Touch::begin() {
  uint8_t m[4];
  for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
    _addr = a;
    if (read(GT_X_MAX, m, 4)) break;
    _addr = 0;
  }
  if (!_addr) return false;
  _xMax = (uint16_t)(m[0] | m[1] << 8);
  _yMax = (uint16_t)(m[2] | m[3] << 8);
  _turn = TURN_SWAP | TURN_MY;
  uint8_t sw;
  if (read(GT_SWITCH1, &sw, 1)) {
    uint8_t d = (sw ^ SWITCH_REF) & 0xC8;
    bool swap = _turn & TURN_SWAP;
    if (d & 0x80) _turn ^= swap ? TURN_MY : TURN_MX;   // its X reversed
    if (d & 0x40) _turn ^= swap ? TURN_MX : TURN_MY;   // its Y reversed
    if (d & 0x08) _turn ^= TURN_SWAP;                  // swaps, or stopped
  }
  write(GT_STATUS, 0);
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  return true;
}

// The first point in the panel's own axes: 1 touching, 0 not, -1 no new
// report yet (the last one still holds).
int Gt911Touch::readRaw(int& rx, int& ry) {
  uint8_t st, p[5];
  if (!read(GT_STATUS, &st, 1)) return 0;
  if (!(st & 0x80)) return -1;
  int n = st & 0x0F;
  bool ok = n >= 1 && n <= 5 && read(GT_STATUS + 1, p, 5);
  write(GT_STATUS, 0);   // taken: the next report may come
  if (!ok) return 0;
  bool swap = _turn & TURN_SWAP;
  int w = swap ? H : W, h = swap ? W : H;
  rx = (p[1] | p[2] << 8) * w / (_xMax ? _xMax : w);
  ry = (p[3] | p[4] << 8) * h / (_yMax ? _yMax : h);
  return 1;
}

bool Gt911Touch::poll(hal::TouchEvent& ev) {
  if (!_addr) return false;
  uint32_t now = millis();
  if (now - _lastPoll < _pollMs) return false;   // about 60 reports a second
  _lastPoll = now;
  int rx, ry;
  int r = readRaw(rx, ry);
  if (r < 0) {
    // No new report: a finger held still. A lost "up" ends it after a while.
    if (_down && now - _lastMove > 1000) {
      _down = false;
      ev = {_x, _y, hal::TouchEvent::UP};
      return true;
    }
    return false;
  }
  if (!r) {
    if (!_down) return false;
    _down = false;
    ev = {_x, _y, hal::TouchEvent::UP};
    return true;
  }
  int sx = _turn & TURN_SWAP ? ry : rx, sy = _turn & TURN_SWAP ? rx : ry;
  if (_turn & TURN_MX) sx = W - 1 - sx;
  if (_turn & TURN_MY) sy = H - 1 - sy;
  _x = (int16_t)(sx < 0 ? 0 : sx >= W ? W - 1 : sx);
  _y = (int16_t)(sy < 0 ? 0 : sy >= H ? H - 1 : sy);
  _lastMove = now;
  ev = {_x, _y, _down ? hal::TouchEvent::MOVE : hal::TouchEvent::DOWN};
  _down = true;
  return true;
}
