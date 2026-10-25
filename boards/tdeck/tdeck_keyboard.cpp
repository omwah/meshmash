#include "tdeck_keyboard.h"
#include <Arduino.h>
#include <Wire.h>

static constexpr uint8_t ADDR = 0x55;
static constexpr uint8_t CMD_LIGHT = 0x01, CMD_RAW = 0x03;
static constexpr int BALL_STEP = 2;
static const uint8_t kBallPins[4] = {PIN_BALL_UP, PIN_BALL_DOWN, PIN_BALL_LEFT, PIN_BALL_RIGHT};
static volatile uint16_t s_toggles[4];

static void IRAM_ATTR onBall(void* arg) { s_toggles[(int)(intptr_t)arg]++; }

bool TDeckKeyboard::begin() {
  tdeck_keymap_reset(&_st);
  // The C3 starts with the peripheral power: give it up to a second.
  for (int i = 0; i < 10 && !_ok; i++) {
    Wire.beginTransmission(ADDR);
    _ok = Wire.endTransmission() == 0;
    if (!_ok) delay(100);
  }
  if (_ok) {
    Wire.beginTransmission(ADDR);
    Wire.write(CMD_RAW);
    Wire.endTransmission();
  }
  for (int d = 0; d < 4; d++) {
    pinMode(kBallPins[d], INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(kBallPins[d]), onBall, (void*)(intptr_t)d, CHANGE);
  }
  pinMode(PIN_BALL_CLICK, INPUT_PULLUP);
  return _ok;
}

// A key that is pressed and released at once (a roll, a click, a character).
void TDeckKeyboard::tap(uint8_t usage, uint8_t mods) {
  kbd_report_t r{};
  r.has_mods = true;
  r.mods = mods;
  r.key[r.n++] = usage;
  _diff.update(r);
  kbd_report_t none{};
  none.has_mods = true;
  _diff.update(none);
}

// The C3 firmware without raw mode: ASCII characters, Backspace as 0x08.
void TDeckKeyboard::typed(uint8_t c) {
  static const char kRow1[] = "1234567890";
  if (c == 0x08) return tap(0x2a, 0);
  if (c == '\r' || c == '\n') return tap(0x28, 0);
  if (c == ' ') return tap(0x2c, 0);
  if (c >= 'a' && c <= 'z') return tap((uint8_t)(0x04 + c - 'a'), 0);
  if (c >= 'A' && c <= 'Z') return tap((uint8_t)(0x04 + c - 'A'), 0x02);
  for (int i = 0; i < 10; i++)
    if (kRow1[i] == c) return tap((uint8_t)(0x1e + i), 0);
  // Punctuation on the US layout: {usage, shifted}.
  static const struct { char c; uint8_t u, s; } kPunct[] = {
    {'!', 0x1e, 1}, {'@', 0x1f, 1}, {'#', 0x20, 1}, {'$', 0x21, 1}, {'%', 0x22, 1}, {'^', 0x23, 1},
    {'&', 0x24, 1}, {'*', 0x25, 1}, {'(', 0x26, 1}, {')', 0x27, 1}, {'-', 0x2d, 0}, {'_', 0x2d, 1},
    {'=', 0x2e, 0}, {'+', 0x2e, 1}, {';', 0x33, 0}, {':', 0x33, 1}, {'\'', 0x34, 0}, {'"', 0x34, 1},
    {',', 0x36, 0}, {'.', 0x37, 0}, {'/', 0x38, 0}, {'?', 0x38, 1},
  };
  for (auto& p : kPunct)
    if (p.c == (char)c) return tap(p.u, p.s ? 0x02 : 0);
}

static bool impossible(const uint8_t* m) {
  bool same = true;
  for (int c = 0; c < TDECK_KEYMAP_COLS; c++) {
    if (m[c] & 0x80) return true;
    same = same && m[c] == m[0];
  }
  return same && m[0] != 0;
}

void TDeckKeyboard::keys() {
  if (!_ok) return;
  uint8_t m[TDECK_KEYMAP_COLS] = {};
  if (!_raw) {
    if (Wire.requestFrom(ADDR, (uint8_t)1) == 1) {
      uint8_t c = Wire.read();
      if (c) { if (!_locked) _lastKey = millis(); typed(c); }
    }
    return;
  }
  if (Wire.requestFrom(ADDR, (uint8_t)TDECK_KEYMAP_COLS) != TDECK_KEYMAP_COLS) return;
  for (auto& b : m) b = Wire.read();
  if (impossible(m)) {
    // Characters only: Alt (Esc), Sym layers and Mic won't work.
    _raw = false;
    Serial.println("keyboard: no raw mode in the C3's firmware (flash LilyGo's 250620 or later): characters only");
    if (m[0] && !(m[0] & 0x80)) typed(m[0]);
    return;
  }
  for (int c = 0; c < TDECK_KEYMAP_COLS; c++) {
    uint8_t changed = (uint8_t)((m[c] ^ _matrix[c]) & 0x7F);
    for (int r = 0; changed && r < TDECK_KEYMAP_ROWS; r++) {
      if (!(changed & (1u << r))) continue;
      bool down = m[c] & (1u << r);
      if (down && !_locked) _lastKey = millis();
      kbd_report_t rep;
      if (tdeck_keymap_event(&_st, c, r, down, &rep)) _diff.update(rep);
    }
    _matrix[c] = m[c];
  }
}

void TDeckKeyboard::ball() {
  for (int d = 0; d < 4; d++) {
    noInterrupts();
    int t = s_toggles[d];
    s_toggles[d] = 0;
    interrupts();
    if (!_ballOn) continue;
    int n = tdeck_keymap_ball(&_ball, d, t, BALL_STEP);
    if (n > 0 && !_locked) _lastKey = millis();
    while (n-- > 0) tap(tdeck_keymap_ball_usage(d), 0);
  }
}

// The click: Enter, and the unlock button, once it has stayed down for 30 ms (a
// worn switch bounces). Locked, that is all it is, so it answers at once.
// Unlocked, the Enter waits for the release: a hold of kHoldMs or more is the
// lock request instead (hal::Keyboard::lockPressed) and sends no Enter.
void TDeckKeyboard::click() {
  const uint32_t kHoldMs = 700;
  bool down = digitalRead(PIN_BALL_CLICK) == LOW;
  uint32_t now = millis();
  if (down && !_clickDown) {
    _clickDown = true;
    _clickAt = now;
  } else if (down && _clickAt && now - _clickAt >= 30) {
    _clickAt = 0;
    _lastKey = now;
    _unlock = true;
    _clickEnter = !_locked;   // sent on release, unless this turns into a hold
    if (_locked) tap(0x28, 0);
  } else if (down && _clickEnter && now - _clickFrom >= kHoldMs) {
    _clickEnter = false;
    _lastKey = now;
    _lock = true;
  } else if (!down) {
    _clickDown = false;
    _clickAt = 0;
  }
}

void TDeckKeyboard::light() {
  if (!_ok) return;
  bool on = _lightLevel && _lightSec && _lastKey && millis() - _lastKey < _lightSec * 1000u;
  uint8_t want = on ? _lightLevel : 0;
  if (want == _lit) return;
  Wire.beginTransmission(ADDR);
  Wire.write(CMD_LIGHT);
  Wire.write(want);
  if (Wire.endTransmission() == 0) _lit = want;
}

bool TDeckKeyboard::poll(hal::KeyEvent& ev) {
  if (_diff.next(ev)) return true;
  ball();
  click();
  if (_notify > want) want = _notify;
  uint32_t now = millis();
  if (now - _lastPoll >= 20) {
    _lastPoll = now;
    keys();
    light();
  }
  return _diff.next(ev);
}
