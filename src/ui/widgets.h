// Reusable pieces: list cursor/scroll state, a menu of settings-like rows,
// and a single-line UTF-8 text editor.
#pragma once
#include <stdint.h>
#include "ui/app.h"

namespace ui {

// Cursor and scroll position for a vertical list.
struct ListNav {
  int sel = 0, top = 0;
  // Handle movement keys; true if the key was consumed.
  bool key(const Key& k, int count, int visible);
  void clamp(int count, int visible);
};

// A row in a MenuScreen.
struct MenuItem {
  const char* label;
  char value[28];        // shown right-aligned; empty for plain actions
  uint8_t valueCol;      // palette slot for the value
  bool enabled;
  bool header;           // non-selectable section header
  bool choice;           // Left/Right change the value (shows an arrow)
};

// A list of label/value rows: settings pages, action lists. Subclasses fill
// rows and act on Enter (activate) and Left/Right (adjust).
class MenuScreen : public Screen {
public:
  void draw(App& app, Canvas& c, const Rect& r) override;
  void onKey(App& app, const Key& k) override;
  const Hint* hints(App& app) override;
  void onEnter(App& app) override;
  bool selectRow(App& app, int i) override;
  // Start at the top: call when opening the menu fresh (not when a child
  // screen returns to it, which also runs onEnter).
  void resetNav() { _nav = ListNav(); _fresh = true; }

protected:
  virtual int count(App& app) = 0;
  virtual void item(App& app, int i, MenuItem& out) = 0;
  virtual void activate(App& app, int i) = 0;
  virtual void adjust(App& app, int i, int dir) { (void)app; (void)i; (void)dir; }
  // Optional block drawn above the rows (e.g. contact details); returns height.
  virtual int drawHead(App& app, Canvas& c, const Rect& r) { (void)app; (void)c; (void)r; return 0; }
  ListNav _nav;
  int _vis = 6;          // rows that fit, as of the last draw
  bool _fresh = true;    // place the cursor on the first usable row
};

// Single-line editor over a caller-owned buffer.
class LineEditor {
public:
  void attach(char* buf, int cap) { _buf = buf; _cap = cap; _cur = (int)strlen_(buf); }
  // true if consumed.
  bool key(const Key& k);
  // Draws the text with a cursor, scrolled horizontally to keep it visible.
  void draw(App& app, Canvas& c, int x, int y, int w, const mc_font_t* f, uint8_t fg, bool secret);
  // Multi-line variant: wraps, shows the last lines that fit in h.
  void drawWrapped(App& app, Canvas& c, const Rect& r, const mc_font_t* f, uint8_t fg);
  int length() const { return (int)strlen_(_buf); }
  int cursor() const { return _cur; }
  const char* text() const { return _buf; }
  // Replace the text from byte `from` up to the cursor with `s` (the cursor ends after it);
  // false, changing nothing, if it would not fit.
  bool replaceBefore(int from, const char* s);

private:
  static int strlen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
  char* _buf = nullptr;
  int _cap = 0, _cur = 0;
};

}  // namespace ui
