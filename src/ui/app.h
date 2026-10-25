// The UI: a stack of screens drawn into one 4-bpp canvas under a status
// header and above a key-hint footer (and, with a touch panel, a bar of
// shortcuts to the main screens). Owns input filtering and theme.
//
// Everything is statically allocated; screens are singletons that keep only
// their own cursor state and read the model when they draw.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ui/gfx/canvas.h"
#include "ui/input/input.h"
#include "ui/input/vim.h"
#include "ui/input/gesture.h"
#include "ui/theme.h"
#include "model/model.h"
#include "hal/display.h"
#include "hal/keyboard.h"
#include "hal/idle.h"
#include "hal/touch.h"
#include "hal/sound.h"
#include "hal/led.h"
#include "gen/fonts.h"

// Boards with a touch panel build the touch UI (gestures, targets, the
// shortcut bar); others leave it out.
#ifndef MC_TOUCH
#define MC_TOUCH 0
#endif

namespace ui {

class App;

struct Rect {
  int x, y, w, h;
};

// Sizes derived from the text-size setting: every screen lays out from these
// instead of fixed pixel values.
struct Metrics {
  const mc_font_t* small;    // footer hints, meta lines, small labels
  const mc_font_t* med;      // header, menus, previews
  const mc_font_t* list;     // list item names
  const mc_font_t* body;     // message text
  const mc_font_t* menu;     // menu and settings rows, text pages
  int head, foot;            // header and footer bar heights
  int tab;                   // tab strip
  int row;                   // single-line list row
  int chatRow;               // two-line conversation row
  int menuRow;               // settings/menu row
  int meta;                  // message meta line
};

struct Hint {
  const char* key;    // short key name ("ent", "tab", "<>", "^v", "esc")
  const char* label;
};

// A place on screen a touch acts on, registered while drawing (App::hit).
struct HitTarget {
  enum Kind : uint8_t {
    KEY,        // arg: the key code a tap sends (a long press on Esc: long Esc)
    ROW,        // arg: list row; tap opens it (Enter), long press shows details (Tab)
    ADJUST,     // arg: list row * 2 + (1: right, 0: left); tap changes the value
    SHORTCUT,   // arg: a shortcut bar button
  };
  Rect r;
  Kind kind;
  int16_t arg;
};

class Screen {
public:
  virtual ~Screen() {}
  virtual const char* title(App& app) = 0;
  virtual void draw(App& app, Canvas& c, const Rect& r) = 0;
  virtual void onKey(App& app, const Key& k) = 0;
  // Key hints for the footer, terminated by {nullptr, nullptr}.
  virtual const Hint* hints(App& app) { (void)app; return nullptr; }
  virtual bool textMode(App& app) { (void)app; return false; }
  virtual void onEnter(App& app) { (void)app; }
  // Called every loop; return true to request a redraw.
  virtual bool tick(App& app, uint32_t now) { (void)app; (void)now; return false; }
  // Screens that draw the full display (boot, lock) hide header and footer.
  virtual bool fullscreen() { return false; }
  // The user asked to go home (long Esc): return to a known starting state.
  virtual void onHome(App& app) { (void)app; }
  // A touch, with the target under where it began (nullptr: none). The
  // default turns it into keys: taps on targets, drags into Up/Down, sideways
  // swipes into Left/Right.
  virtual void onTouch(App& app, const Gesture& g, const HitTarget* t);
  // Put the list cursor on row `i` (a tap on it); false if it can't go there.
  virtual bool selectRow(App& app, int i) { (void)app; (void)i; return false; }
  // Pixels of drag that move a list by one row: its row height, so with "opposite" the list follows the finger.
  virtual int dragRowPx(App& app);
};

using Action = void (*)(App& app, int arg);

class App {
public:
  static constexpr int MAX_DEPTH = 8;
  static constexpr int BAR_H = 34;   // the touch shortcut bar

  App(model::MeshModel& m, hal::Display& d, hal::Keyboard& k, hal::Sound* sound = nullptr);

  void begin(Screen* root);
  // Run one iteration: read keys, update, redraw if needed. Returns the
  // milliseconds until the next iteration needs to run (for idle sleep).
  uint32_t loop(uint32_t nowMs);

  // Navigation. Every change flushes pending input.
  void push(Screen* s);
  void pop();
  void replace(Screen* s);
  void popToRoot();
  Screen* top() { return _stack[_depth - 1]; }
  int depth() const { return _depth; }
  Screen* screenAt(int i) const { return _stack[i]; }

  // Transient message over the footer.
  void toast(const char* text, uint8_t col = C_ACCENT);
  // Modal yes/no; `yes` runs with `arg` when confirmed.
  void confirm(const char* question, Action yes, int arg = 0);
  // Modal single-line text entry; `done` runs with the text in promptText().
  void prompt(const char* label, const char* initial, int maxLen, Action done, int arg = 0, bool secret = false);
  const char* promptText() const;

  void invalidate() { _dirty = true; }
  // Draw text in `maxw` pixels: as is if it fits, cut with an ellipsis if it
  // doesn't, or, with `scroll` (the selected row, titles), as a marquee that
  // pauses, slides to show the end, pauses and starts again.
  int fitText(Canvas& c, int x, int y, const char* s, const mc_font_t* f, uint8_t fg, int maxw, bool scroll);
  bool asleep() const { return _asleep; }
  // Locked: the screen timed out on a board with an unlock button, and keys
  // and touch are ignored until it is pressed. The screen can still light for
  // a message or a pairing, and stays locked.
  bool locked() const { return _locked; }
  void wake();
  // The status light, if the board has one (set before begin()).
  void setLed(hal::Led* led) { _led = led; }
  void setIdleHook(hal::IdleHook* h) { _idleHook = h; }
#if MC_TOUCH
  // The touch panel, if the board has one (set before begin()).
  void setTouch(hal::Touch* t) { _touch = t; }
  bool hasTouch() const { return _touch != nullptr; }
  // While drawing: make `r` act on touch (see HitTarget).
  void hit(const Rect& r, HitTarget::Kind kind, int arg);
#else
  bool hasTouch() const { return false; }
  void hit(const Rect&, HitTarget::Kind, int) {}
#endif
  // Act on a key as if it were typed (touch targets send keys this way).
  void key(uint16_t code);
  // Turn a drag's pixels into whole steps of `step` pixels (the rest is kept
  // for the next drag event); the sign is the finger's direction.
  int dragSteps(int dy, int step);
  // A drag's vertical pixels as the direction to move (positive: down, toward later items and
  // newer messages). Both lists and conversations use this, so they always agree: with
  // Settings -> Touch Scrolling Direction "same" the selection or view moves the way the finger does;
  // with "opposite" the content follows the finger, so they move the other way.
  int scrollDy(int dy) const { return _m.prefs().touchScrollNatural ? -dy : dy; }
  // Play the new-message colours once (a preview, or an alert).
  void flashLed();
  void applyPrefs();               // theme, lockout, brightness from model prefs
  void previewTheme(uint8_t idx);  // apply a theme without saving it

  model::MeshModel& m() { return _m; }
  Canvas& canvas() { return _c; }
  uint32_t nowMs() const { return _now; }
  const Theme& theme() const { return kThemes[_theme]; }

  const Metrics& mx() const { return *_mx; }
  const mc_font_t* fList() const { return _mx->list; }
  const mc_font_t* fBody() const { return _mx->body; }
  const mc_font_t* fSmall() const { return _mx->small; }
  const mc_font_t* fMed() const { return _mx->med; }
  const mc_font_t* fMenu() const { return _mx->menu; }

  Rect content() const;
  // The shortcut bar is showing (touch, and the screen isn't taking text).
  bool showBar();
  // Vim keys are on and the screen on top isn't taking text.
  bool vimActive();

private:
  void dispatch(const Key& k);
  void handleKey(Key k, uint32_t now);
  void handleGesture(const Gesture& g);
  bool keyboardText();
  void drawShortcutBar();
  void render();
  void drawHeader();
  void drawFooter();
  void drawPairing();
  void entered();
  uint32_t ledTick(uint32_t now);   // ms until the next colour, 0: idle

  model::MeshModel& _m;
  hal::Display& _d;
  hal::Keyboard& _k;
  hal::Sound* _sound;
  Canvas _c;
  Input _in;
  Vim _vim;
  Screen* _stack[MAX_DEPTH] = {};
  int _depth = 0;
  uint8_t _theme = 0;
  const Metrics* _mx;
  bool _dirty = true, _forceFlush = true;
  uint32_t _now = 0, _lastRev = 0, _lastMinute = 0;
  char _toast[48] = {};
  uint8_t _toastCol = C_ACCENT;
  uint32_t _toastUntil = 0;
  uint32_t _lastInput = 0;
  uint16_t _lastUnread = 0;
  hal::Led* _led = nullptr;
  hal::IdleHook* _idleHook = nullptr;
  bool _idleTold = false;        // what the hook was last told (the screen asleep)
  bool _dimmed = false;          // the backlight is down before the screen goes off
#if MC_TOUCH
  hal::Touch* _touch = nullptr;
  GestureReader _gest;
  static constexpr int MAX_HITS = 40;
  HitTarget _hits[MAX_HITS];
  int _nHits = 0;
  int _dragRest = 0;             // drag pixels short of a whole step
#endif
  uint32_t _ledStart = 0;        // when the burst started
  bool _ledOn = false;           // a burst is playing
  int _ledStep = -1;             // the colour showing (-1: off)
  bool _asleep = false;
  bool _locked = false;
  void unlock(uint32_t now);
  void lockNow();
  uint32_t _marqueeStart = 0;   // restarts on every key, so a new selection starts at its beginning
  bool _animating = false;       // a marquee is moving: keep redrawing
  bool _pairing = false;         // the PIN overlay is showing
  bool _pairingHidden = false;   // dismissed with Esc for this attempt
};

// Shared drawing helpers.
void drawSignal(Canvas& c, int x, int y, int8_t snr_x4);       // 4 bars, 9x8
void drawBattery(Canvas& c, int x, int y, uint8_t pct, bool charging, uint8_t fg);  // 14x7
void drawScrollbar(Canvas& c, const Rect& r, int total, int first, int visible);
// Unread-count pill ending at xRight; returns its left edge.
int drawBadge(App& a, int xRight, int y, unsigned n, uint8_t col = C_BADGE);
uint8_t typeColor(model::NodeType t);
const uint8_t* typeIcon(model::NodeType t);
void fmtAgo(char* out, size_t n, uint32_t now, uint32_t ts);    // "3m", "2h", "5d"
void fmtAge(char* out, size_t n, uint32_t secs);                // same, from seconds
void fmtTime(char* out, size_t n, uint32_t ts);                 // local "HH:MM"
// Local "HH:MM" for today, "Oct 9 14:05" for other days.
void fmtStamp(char* out, size_t n, uint32_t now, uint32_t ts);
void fmtDur(char* out, size_t n, uint32_t secs);                // "3m05s", "2h10m", "4d03h"
void fmtSnr(char* out, size_t n, int8_t snr_x4);                 // "-7.5"
uint8_t snrColor(int8_t snr_x4);
// Word-wrap UTF-8 text to `w` pixels. Calls back per line; returns line count.
using LineFn = void (*)(void* ctx, const char* s, int len, int line);
int wrapText(const char* s, const mc_font_t* f, int w, LineFn fn, void* ctx);

}  // namespace ui
