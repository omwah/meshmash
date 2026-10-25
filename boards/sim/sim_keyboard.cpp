#include "sim_keyboard.h"
#include <string.h>

namespace sim {

#ifdef MC_BOARD_TDECK
// The T-Deck's keys in its keyboard controller's matrix order (column, then
// row), as in tdeck_keymap.c. Here "row" is the matrix column.
static constexpr int ROWS = 5, COLS = 7;
static const char* const kLegends[ROWS][COLS] = {
    {"q", "w", "sym", "a", "alt", "space", "mic"},
    {"e", "s", "d", "p", "x", "z", "shift"},
    {"r", "g", "t", "rshift", "v", "c", "f"},
    {"u", "h", "y", "enter", "b", "n", "j"},
    {"o", "l", "i", "bksp", "$", "m", "k"},
};
// What each key types with Sym, and with Sym and Shift.
static const char kSym[ROWS][COLS] = {
    {'#', '1', 0, '*', 0, 0, '0'},
    {'2', '4', '5', '@', '8', '7', 0},
    {'3', '/', '(', 0, '?', '9', '6'},
    {'_', ':', ')', 0, '!', ',', ';'},
    {'+', '"', '-', 0, 0, '.', '\''},
};
static const char kSymShift[ROWS][COLS] = {
    {'~', '`', 0, '|', 0, 0, 0},
    {'^', '\\', '<', '&', 0, 0, 0},
    {'%', 0, '[', 0, 0, 0, '>'},
    {'{', 0, ']', 0, 0, 0, 0},
    {'=', 0, '}', 0, 0, 0, 0},
};
static const char* const kAlias[][2] = {{"esc", "alt"}, {"ctrl", "mic"}, {"menu", "sym+enter"},
                                        {"tab", "sym+space"}, {"del", "sym+bksp"}, {"backspace", "bksp"},
                                        {"ret", "enter"}};
// The trackball, in tdeck_keymap's direction order, then its click.
static const char* const kBall[] = {"up", "down", "left", "right", "click", "hold"};
static constexpr int BALL_CLICK = 4, BALL_HOLD = 5;
#else
// The printed layout, as in cardputer_keymap.h.
static constexpr int ROWS = 4, COLS = 14;
static const char* const kLegends[ROWS][COLS] = {
    {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "bksp"},
    {"tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"},
    {"fn", "shift", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "enter"},
    {"ctrl", "opt", "alt", "z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "space"},
};
static const char kShifted[ROWS][COLS] = {
    {'~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '{', '}', '|'},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '"', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '<', '>', '?', 0},
};
// The navigation meaning of the arrow keys, and other names.
static const char* const kAlias[][2] = {{"up", ";"}, {"down", "."}, {"left", ","}, {"right", "/"},
                                        {"esc", "`"}, {"menu", "opt"}, {"backspace", "bksp"}, {"ret", "enter"}};
#endif

SimKeyboard::SimKeyboard(const uint32_t* nowMs) : _now(nowMs) {
#ifdef MC_BOARD_TDECK
  tdeck_keymap_reset(&_st);
#else
  cardputer_keymap_reset(&_st);
#endif
}

void SimKeyboard::setTextMode(bool on) {
#ifdef MC_BOARD_TDECK
  (void)on;   // no keys double as arrows
#else
  cardputer_keymap_text_mode(&_st, on);
#endif
}

static bool add(const char* legend, Chord& out) {
  if (out.n == 3) return false;
#ifdef MC_BOARD_TDECK
  for (int d = 0; d < 6; d++)
    if (!strcmp(legend, kBall[d])) {
      out.row[out.n] = Chord::BALL;
      out.col[out.n++] = (int8_t)d;
      return true;
    }
#endif
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (!strcmp(kLegends[r][c], legend)) {
        out.row[out.n] = (int8_t)r;
        out.col[out.n++] = (int8_t)c;
        return true;
      }
  return false;
}

bool SimKeyboard::find(const char* legend, Chord& out) {
  out = Chord();
  for (auto& a : kAlias)
    if (!strcmp(legend, a[0])) legend = a[1];
  // "mod+key", "mod+mod+key"
  char buf[32];
  strncpy(buf, legend, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  for (char* p = buf; p;) {
    char* plus = strchr(p, '+');
    if (plus && plus[1]) *plus = 0;
    else plus = nullptr;
    const char* part = p;
    for (auto& a : kAlias)
      if (!strcmp(part, a[0]) && !strchr(a[1], '+')) part = a[1];
    if (!add(part, out)) return false;
    p = plus ? plus + 1 : nullptr;
  }
  return out.n > 0;
}

bool SimKeyboard::findChar(char ch, Chord& out) {
  out = Chord();
  if (ch == ' ') return add("space", out);
  if (ch >= 'A' && ch <= 'Z') {
    char l[2] = {(char)(ch - 'A' + 'a'), 0};
    return add("shift", out) && add(l, out);
  }
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      auto key = [&](const char* mod, const char* mod2) {
        if (mod) add(mod, out);
        if (mod2) add(mod2, out);
        out.row[out.n] = (int8_t)r;
        out.col[out.n++] = (int8_t)c;
        return true;
      };
      if (strlen(kLegends[r][c]) == 1 && kLegends[r][c][0] == ch) return key(nullptr, nullptr);
#ifdef MC_BOARD_TDECK
      if (kSym[r][c] == ch) return key("sym", nullptr);
      if (kSymShift[r][c] == ch) return key("sym", "shift");
#else
      if (kShifted[r][c] == ch) return key("shift", nullptr);
#endif
    }
  return false;
}

void SimKeyboard::at(uint32_t ms, int row, int col, bool down) {
  if (_n == Q) return;
  // Keep the queue sorted by time (stable).
  int i = _n++;
  while (i > 0 && _q[i - 1].ms > ms) { _q[i] = _q[i - 1]; i--; }
  _q[i] = {ms, (int8_t)row, (int8_t)col, down};
  if (ms > _last) _last = ms;
}

bool SimKeyboard::poll(hal::KeyEvent& ev) {
  while (_n && _q[0].ms <= *_now) {
    Ev e = _q[0];
    memmove(_q, _q + 1, sizeof(Ev) * (--_n));
    kbd_report_t r;
#ifdef MC_BOARD_TDECK
    if (e.row == Chord::BALL) {
      // A roll is an arrow key's press and release, as the board sends it; the
      // click is an Enter, and the unlock button.
      memset(&r, 0, sizeof r);
      r.has_mods = true;
      if (e.col == BALL_CLICK) {
        if (e.down) { r.key[r.n++] = 0x28; _unlock = true; }
      } else if (e.col == BALL_HOLD) {
        if (e.down) _lock = true;   // the click, held: no Enter
      } else if (e.down) {
        r.key[r.n++] = tdeck_keymap_ball_usage(e.col);
      }
      _diff.update(r);
    } else if (tdeck_keymap_event(&_st, e.row, e.col, e.down, &r)) {
      _diff.update(r);
    }
#else
    if (cardputer_keymap_event(&_st, e.row, e.col, e.down, &r)) _diff.update(r);
#endif
  }
  return _diff.next(ev);
}

}  // namespace sim
