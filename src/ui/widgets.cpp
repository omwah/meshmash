#include "ui/widgets.h"
#include "ui/icons.h"
#include <stdio.h>
#include <string.h>

namespace ui {

// ---------------------------------------------------------------- ListNav

void ListNav::clamp(int count, int visible) {
  if (sel >= count) sel = count - 1;
  if (sel < 0) sel = 0;
  if (sel < top) top = sel;
  if (sel >= top + visible) top = sel - visible + 1;
  if (top > count - visible) top = count - visible;
  if (top < 0) top = 0;
}

bool ListNav::key(const Key& k, int count, int visible) {
  if (count <= 0) return false;
  int old = sel;
  switch (k.code) {
    // Lists stop at their ends instead of wrapping: one press too many must
    // never throw the cursor to the opposite end of a menu.
    case K_UP:
      if (sel > 0) sel--;
      break;
    case K_DOWN:
      if (sel < count - 1) sel++;
      break;
    case K_PGUP: sel -= visible; break;
    case K_PGDN: sel += visible; break;
    case K_HALF_UP: sel -= visible > 1 ? visible / 2 : 1; break;
    case K_HALF_DN: sel += visible > 1 ? visible / 2 : 1; break;
    case K_HOME: sel = 0; break;
    case K_END: sel = count - 1; break;
    case K_GOTO: sel = k.count - 1; break;
    default: return false;
  }
  (void)old;
  clamp(count, visible);
  return true;
}

// ---------------------------------------------------------------- MenuScreen

void MenuScreen::onEnter(App& app) {
  if (!_fresh) return;   // returning from a child screen: keep the cursor
  _fresh = false;
  // Land on the first row that can be used.
  int n = count(app);
  for (int i = 0; i < n; i++) {
    MenuItem it;
    memset(&it, 0, sizeof it);
    it.enabled = true;
    item(app, i, it);
    if (!it.header && it.enabled) { _nav.sel = i; return; }
  }
}

void MenuScreen::draw(App& app, Canvas& c, const Rect& r0) {
  Rect r = r0;
  int hh = drawHead(app, c, r);
  r.y += hh;
  r.h -= hh;
  const int RH = app.mx().menuRow;
  int n = count(app);
  int vis = r.h / RH;
  if (vis < 1) vis = 1;
  _vis = vis;
  _nav.clamp(n, vis);
  const mc_font_t* f = app.fMenu();
  const mc_font_t* sm = app.fSmall();
  const int ty = (RH - f->height) / 2, iy = (RH - 8) / 2;
  for (int row = 0; row < vis && _nav.top + row < n; row++) {
    int i = _nav.top + row;
    MenuItem it;
    memset(&it, 0, sizeof it);
    it.enabled = true;
    it.valueCol = C_ACCENT;
    item(app, i, it);
    int y = r.y + row * RH;
    if (it.header) {
      int lw = Canvas::textWidth(it.label, sm);
      c.text(r.x + 4, y + (RH - sm->height) / 2, it.label, sm, C_ACCENT);
      c.hline(r.x + 6 + lw, y + RH / 2, r.w - 12 - lw, C_DIM);
      continue;
    }
    bool sel = i == _nav.sel;
    if (sel) {
      c.fillRect(r.x, y, r.w - 3, RH, C_SEL);
      c.fillRect(r.x, y, 2, RH, C_ACCENT);
    } else if (row & 1) {
      c.fillRect(r.x, y, r.w - 3, RH, C_ZEBRA);
    }
    uint8_t fg = it.enabled ? C_TEXT : C_DIM;
    // If label and value don't both fit (Large text), the value drops to the
    // header font. The label keeps at least a third of the row; a value that
    // still doesn't fit is cut (or scrolls when selected).
    int lw = Canvas::textWidth(it.label, f);
    const mc_font_t* vf = f;
    if (it.value[0] && lw + Canvas::textWidth(it.value, f) + 28 > r.w && app.fMed() != f) vf = app.fMed();
    int vmax = r.w - 26 - (lw < r.w / 3 ? lw : r.w / 3);
    int vw = it.value[0] ? Canvas::textWidth(it.value, vf) : 0;
    if (vw > vmax) vw = vmax;
    app.fitText(c, r.x + 6, y + ty, it.label, f, fg, r.w - 18 - vw - (vw ? 10 : 0), sel);
    app.hit({r.x, y, r.w, RH}, HitTarget::ROW, i);
    if (vw) {
      // With touch, a choice reads "< value >" and either side changes it.
      bool touchChoice = app.hasTouch() && it.enabled && it.choice;
      int vx = r.x + r.w - 8 - vw - (touchChoice ? 12 : 0);
      if (touchChoice) {
        c.bitmap(vx - 11, y + iy, icon::arrowL, 8, 8, sel ? C_ACCENT : C_DIM);
        c.bitmap(r.x + r.w - 14, y + iy, icon::arrowR, 8, 8, sel ? C_ACCENT : C_DIM);
        int ax = vx - 16, mid = (ax + r.x + r.w) / 2;
        app.hit({ax, y, mid - ax, RH}, HitTarget::ADJUST, i * 2);
        app.hit({mid, y, r.x + r.w - mid, RH}, HitTarget::ADJUST, i * 2 + 1);
      } else if (sel && it.enabled && it.choice) {
        // Show that Left/Right change this value.
        c.bitmap(vx - 9, y + iy, icon::arrowL, 8, 8, C_DIM);
      }
      app.fitText(c, vx, y + (RH - vf->height) / 2, it.value, vf, it.enabled ? it.valueCol : C_DIM, vw, sel);
    }
  }
  drawScrollbar(c, r, n, _nav.top, vis);
}

bool MenuScreen::selectRow(App& app, int i) {
  if (i < 0 || i >= count(app)) return false;
  MenuItem it;
  memset(&it, 0, sizeof it);
  it.enabled = true;
  item(app, i, it);
  if (it.header || !it.enabled) return false;
  _nav.sel = i;
  _nav.clamp(count(app), _vis);
  return true;
}

void MenuScreen::onKey(App& app, const Key& k) {
  int n = count(app);
  int vis = _vis;
  if (k.code == K_ESC) { app.pop(); return; }
  int old = _nav.sel;
  if (_nav.key(k, n, vis)) {
    // Never rest on a header or a disabled row: keep going the way the
    // cursor moved; if there is nothing usable that way, go back to where
    // the cursor started.
    int dir = (k.code == K_END || _nav.sel < old) ? -1 : 1;
    int start = old;
    for (int guard = 0; guard < 2 * n; guard++) {
      MenuItem it;
      memset(&it, 0, sizeof it);
      it.enabled = true;
      item(app, _nav.sel, it);
      if (!it.header && it.enabled) break;
      if (_nav.sel + dir < 0 || _nav.sel + dir >= n) { _nav.sel = start; break; }
      _nav.sel += dir;
    }
    _nav.clamp(n, vis);
    return;
  }
  MenuItem it;
  memset(&it, 0, sizeof it);
  it.enabled = true;
  item(app, _nav.sel, it);
  if (!it.enabled || it.header) return;
  if (k.code == K_ENTER) activate(app, _nav.sel);
  else if (k.code == K_LEFT) adjust(app, _nav.sel, -1);
  else if (k.code == K_RIGHT) adjust(app, _nav.sel, +1);
}

const Hint* MenuScreen::hints(App&) {
  static const Hint h[] = {{"^v", "move"}, {"ent", "select"}, {"<>", "change"}, {"esc", "back"}, {nullptr, nullptr}};
  return h;
}

// ---------------------------------------------------------------- LineEditor

static bool isCont(char c) { return (c & 0xC0) == 0x80; }

bool LineEditor::key(const Key& k) {
  int len = length();
  if (k.code == K_LEFT) {
    if (_cur > 0) { do _cur--; while (_cur > 0 && isCont(_buf[_cur])); }
    return true;
  }
  if (k.code == K_RIGHT) {
    if (_cur < len) { do _cur++; while (_cur < len && isCont(_buf[_cur])); }
    return true;
  }
  if (k.code == K_HOME) { _cur = 0; return true; }
  if (k.code == K_END) { _cur = len; return true; }
  if (k.code == K_BKSP) {
    if (_cur == 0) return true;
    int s = _cur;
    do s--; while (s > 0 && isCont(_buf[s]));
    memmove(_buf + s, _buf + _cur, len - _cur + 1);
    _cur = s;
    return true;
  }
  if (k.code == K_DEL) {
    if (_cur >= len) return true;
    int e = _cur;
    do e++; while (e < len && isCont(_buf[e]));
    memmove(_buf + _cur, _buf + e, len - e + 1);
    return true;
  }
  if (k.isChar()) {
    if (len + 1 >= _cap) return true;
    memmove(_buf + _cur + 1, _buf + _cur, len - _cur + 1);
    _buf[_cur++] = (char)k.code;
    return true;
  }
  return false;
}

bool LineEditor::replaceBefore(int from, const char* s) {
  int len = length(), n = (int)strlen(s);
  if (from < 0 || from > _cur || len - (_cur - from) + n + 1 > _cap) return false;
  memmove(_buf + from + n, _buf + _cur, len - _cur + 1);
  memcpy(_buf + from, s, n);
  _cur = from + n;
  return true;
}

void LineEditor::draw(App& app, Canvas& c, int x, int y, int w, const mc_font_t* f, uint8_t fg, bool secret) {
  // Count code points before the cursor and in total.
  int before = 0, total = 0;
  for (const char* p = _buf; *p;) {
    if (p - _buf < _cur) before++;
    utf8Next(&p);
    total++;
  }
  int cols = w / f->width - 1;
  int first = before > cols ? before - cols : 0;
  const char* p = _buf;
  for (int i = 0; i < first; i++) utf8Next(&p);
  int cx = x;
  for (int i = first; i < total && cx + f->width <= x + w; i++) {
    const char* q = p;
    utf8Next(&q);
    char g[5] = {0};
    if (secret) g[0] = '*';
    else memcpy(g, p, q - p);
    c.text(cx, y, g, f, fg);
    cx += f->width;
    p = q;
  }
  // Blinking caret.
  if ((app.nowMs() / 500) & 1) return;
  int caretX = x + (before - first) * f->width;
  c.fillRect(caretX, y, 1, f->height, C_ACCENT);
}

// Where the line starting at byte `start` ends: at most `cols` characters,
// broken after the last space if a word would be cut. `*n` gets its length
// in characters.
static int wrapLine(const char* buf, int start, int cols, int* n) {
  const char* p = buf + start;
  int count = 0, spaceEnd = -1, spaceCount = 0;
  while (*p && count < cols) {
    if (*p == ' ') { spaceEnd = (int)(p - buf) + 1; spaceCount = count + 1; }
    utf8Next(&p);
    count++;
  }
  int end = (int)(p - buf);
  if (*p == ' ') end++;   // a space right at the edge ends this line, not starts the next
  else if (*p && spaceEnd > start) { end = spaceEnd; count = spaceCount; }
  *n = count;
  return end;
}

void LineEditor::drawWrapped(App& app, Canvas& c, const Rect& r, const mc_font_t* f, uint8_t fg) {
  int cols = r.w / f->width;
  int rows = r.h / f->height;
  if (cols < 2 || rows < 1) return;
  // Word wrap; the caret is placed on the line that holds its byte (at the
  // end of a full line it starts the next one, where typing will go).
  int len = length();
  int lines = 0, caretLine = 0, caretCol = 0;
  for (int s = 0;;) {
    int n, e = wrapLine(_buf, s, cols, &n);
    if (_cur >= s && (_cur < e || (_cur == e && (e == len) && n < cols))) {
      caretLine = lines;
      caretCol = 0;
      for (const char* p = _buf + s; p < _buf + _cur; utf8Next(&p)) caretCol++;
    }
    lines++;
    if (e >= len) {
      if (n == cols && _cur == len) { caretLine = lines; caretCol = 0; lines++; }
      break;
    }
    s = e;
  }
  int firstLine = lines > rows ? lines - rows : 0;
  if (caretLine < firstLine) firstLine = caretLine;
  if (caretLine >= firstLine + rows) firstLine = caretLine - rows + 1;
  int line = 0;
  for (int s = 0; s < len && line < firstLine + rows; line++) {
    int n, e = wrapLine(_buf, s, cols, &n);
    if (line >= firstLine) {
      const char* p = _buf + s;
      for (int col = 0; p < _buf + e; col++) {
        const char* q = p;
        utf8Next(&q);
        char g[5] = {0};
        memcpy(g, p, q - p);
        c.text(r.x + col * f->width, r.y + (line - firstLine) * f->height, g, f, fg);
        p = q;
      }
    }
    s = e;
  }
  if ((app.nowMs() / 500) & 1) return;
  int cl = caretLine - firstLine;
  if (cl >= 0 && cl < rows)
    c.fillRect(r.x + caretCol * f->width, r.y + cl * f->height, 1, f->height, C_ACCENT);
}

}  // namespace ui
