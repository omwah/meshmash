#include "ui/gfx/canvas.h"
#include "ui/gfx/emoji.h"
#include "ui/theme.h"
#include <initializer_list>
#include <string.h>

namespace ui {

static uint16_t s_line[Canvas::W * Canvas::BAND];

Canvas::Canvas() {
  memset(_fb, 0, sizeof(_fb));
  memset(_rowHash, 0, sizeof(_rowHash));
  noClip();
}

void Canvas::clip(int x, int y, int w, int h) {
  _cx0 = x < 0 ? 0 : x;
  _cy0 = y < 0 ? 0 : y;
  _cx1 = x + w > W ? W : x + w;
  _cy1 = y + h > H ? H : y + h;
}

void Canvas::clear(uint8_t c) {
  memset(_fb, (c & 15) * 0x11, sizeof(_fb));
  _nspr = 0;
}

// A rectangle was painted over. Each sprite it touches keeps the largest
// rectangle of itself that is left, or goes if none is.
void Canvas::cover(int x0, int y0, int x1, int y1) {
  if (x0 < _cx0) x0 = _cx0;
  if (y0 < _cy0) y0 = _cy0;
  if (x1 > _cx1) x1 = _cx1;
  if (y1 > _cy1) y1 = _cy1;
  for (int i = 0; i < _nspr;) {
    Sprite& s = _spr[i];
    if (x0 >= s.vx1 || x1 <= s.vx0 || y0 >= s.vy1 || y1 <= s.vy0) { i++; continue; }
    // The four strips of the sprite outside the rectangle.
    const int c[4][4] = {
      {s.vx0, s.vy0, x0 > s.vx0 ? x0 : s.vx0, s.vy1},         // left
      {x1 < s.vx1 ? x1 : s.vx1, s.vy0, s.vx1, s.vy1},         // right
      {s.vx0, s.vy0, s.vx1, y0 > s.vy0 ? y0 : s.vy0},         // above
      {s.vx0, y1 < s.vy1 ? y1 : s.vy1, s.vx1, s.vy1},         // below
    };
    int best = 0, bi = -1;
    for (int k = 0; k < 4; k++) {
      int a = (c[k][2] - c[k][0]) * (c[k][3] - c[k][1]);
      if (c[k][2] > c[k][0] && c[k][3] > c[k][1] && a > best) { best = a; bi = k; }
    }
    if (bi < 0) { _spr[i] = _spr[--_nspr]; continue; }   // gone; look at what moved here
    s.vx0 = c[bi][0]; s.vy0 = c[bi][1]; s.vx1 = c[bi][2]; s.vy1 = c[bi][3];
    s.sig = (s.sig ^ (uint32_t)(s.vx0 | s.vx1 << 8 | s.vy0 << 16 | s.vy1 << 24)) * 16777619u;
    i++;
  }
}

bool Canvas::emoji(int x, int y, const mc_emoji_t* e, int glyph) {
  const int w = e->w * e->scale, h = e->h * e->scale;
  int vx0 = x < _cx0 ? _cx0 : x, vy0 = y < _cy0 ? _cy0 : y;
  int vx1 = x + w > _cx1 ? _cx1 : x + w, vy1 = y + h > _cy1 ? _cy1 : y + h;
  if (vx0 >= vx1 || vy0 >= vy1) return true;
  if (_nspr) cover(vx0, vy0, vx1, vy1);
  if (_nspr >= MC_MAX_EMOJI) return false;
  uint32_t sig = 2166136261u;
  for (uint32_t v : {(uint32_t)(uint16_t)x, (uint32_t)(uint16_t)y, (uint32_t)(uint16_t)vx0, (uint32_t)(uint16_t)vx1,
                     (uint32_t)(uint16_t)vy0, (uint32_t)(uint16_t)vy1, (uint32_t)glyph, (uint32_t)(uintptr_t)e})
    sig = (sig ^ v) * 16777619u;
  _spr[_nspr++] = {(int16_t)x, (int16_t)y, (int16_t)vx0, (int16_t)vy0, (int16_t)vx1, (int16_t)vy1,
                   (uint16_t)glyph, sig, e};
  return true;
}

void Canvas::pixel(int x, int y, uint8_t c) {
  if (x < _cx0 || y < _cy0 || x >= _cx1 || y >= _cy1) return;
  uint8_t* p = &_fb[y * (W / 2) + (x >> 1)];
  *p = (x & 1) ? ((*p & 0xF0) | (c & 15)) : ((*p & 0x0F) | (c << 4));
}

uint8_t Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= W || y >= H) return 0;
  uint8_t b = _fb[y * (W / 2) + (x >> 1)];
  return (x & 1) ? (b & 15) : (b >> 4);
}

void Canvas::fillRect(int x, int y, int w, int h, uint8_t c) {
  int x0 = x < _cx0 ? _cx0 : x, y0 = y < _cy0 ? _cy0 : y;
  int x1 = x + w > _cx1 ? _cx1 : x + w, y1 = y + h > _cy1 ? _cy1 : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  if (_nspr) cover(x0, y0, x1, y1);
  c &= 15;
  for (int yy = y0; yy < y1; yy++) {
    int xx = x0;
    if (xx & 1) pixel(xx++, yy, c);
    int pairs = (x1 - xx) >> 1;
    if (pairs > 0) {
      memset(&_fb[yy * (W / 2) + (xx >> 1)], c * 0x11, pairs);
      xx += pairs * 2;
    }
    if (xx < x1) pixel(xx, yy, c);
  }
}

void Canvas::rect(int x, int y, int w, int h, uint8_t c) {
  hline(x, y, w, c);
  hline(x, y + h - 1, w, c);
  vline(x, y, h, c);
  vline(x + w - 1, y, h, c);
}

void Canvas::pill(int x, int y, int w, int h, uint8_t c) {
  fillRect(x + 1, y, w - 2, h, c);
  vline(x, y + 1, h - 2, c);
  vline(x + w - 1, y + 1, h - 2, c);
}

void Canvas::bitmap(int x, int y, const uint8_t* rows, int w, int h, uint8_t c, int scale) {
  if (_nspr) cover(x, y, x + w * scale, y + h * scale);
  for (int r = 0; r < h; r++)
    for (int i = 0; i < w; i++)
      if (rows[r] & (0x80 >> i)) {
        if (scale == 1) pixel(x + i, y + r, c);
        else fillRect(x + i * scale, y + r * scale, scale, scale, c);
      }
}

void Canvas::bitmapWide(int x, int y, const uint8_t* rows, int w, int h, uint8_t c) {
  if (_nspr) cover(x, y, x + w, y + h);
  const int stride = (w + 7) / 8;
  for (int r = 0; r < h; r++)
    for (int i = 0; i < w; i++)
      if (rows[r * stride + i / 8] & (0x80 >> (i & 7))) pixel(x + i, y + r, c);
}

// The glyph for cp within one font layer: binary search over its runs.
static const uint8_t* findGlyph(const mc_font_t* f, uint32_t cp) {
  int lo = 0, hi = (int)f->nruns - 1;
  const int stride = f->height * f->bytes_per_row;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    const mc_run_t& r = f->runs[mid];
    if (cp < r.first) hi = mid - 1;
    else if (cp - r.first >= r.count) lo = mid + 1;
    else return f->bitmaps + (r.index + (cp - r.first)) * stride;
  }
  return nullptr;
}

struct Glyph {
  const uint8_t* bits;
  const mc_font_t* layer;   // the font (primary or fallback) it came from
};

// The font itself first, then each fallback layer in turn. A code point none
// of them has shows the first layer's replacement glyph (U+FFFD), or '?' from
// the primary font when no layer carries one.
static Glyph resolve(const mc_font_t* f, uint32_t cp) {
  for (const mc_font_t* l = f; l; l = l->next)
    if (const uint8_t* g = findGlyph(l, cp)) return {g, l};
  for (const mc_font_t* l = f->next; l; l = l->next)
    if (const uint8_t* g = findGlyph(l, REPLACEMENT)) return {g, l};
  return {findGlyph(f, '?'), f};
}

// What the text at one position turns into.
struct Elem {
  int bytes = 0;                       // 0 at the terminator
  int adv = 0;                         // pixels
  Glyph glyph = {nullptr, nullptr};    // a monochrome glyph, or
  const mc_emoji_t* emoji = nullptr;   // a colour emoji
  int emojiGlyph = 0;
  bool pill = false;                   // part of a @[mention]: drawn on a highlight
};

// Whether the font chain itself (not its replacement glyph) draws cp.
static bool hasText(const mc_font_t* f, uint32_t cp) {
  for (const mc_font_t* l = f; l; l = l->next)
    if (findGlyph(l, cp)) return true;
  return false;
}

// Characters that take no room and draw nothing: variation selectors, joiners,
// zero-width marks, the combining keycap (an unsupported keycap shows its
// digit), skin tone modifiers and the tags of subdivision flags.
// Without this an unsupported sequence would show a box for each of them.
static bool invisible(uint32_t cp) {
  return cp == 0xFE0E || cp == 0xFE0F || (cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF || cp == 0x20E3 ||
         (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F);
}

static Elem nextPlain(const mc_font_t* f, const char* s) {
  Elem el;
  uint32_t cp;
  el.bytes = utf8Decode(s, nullptr, &cp);
  if (cp == 0) { el.bytes = 0; return el; }
  const bool layered = f->emoji || f->next;
  if (f->emoji) {
    // Digits, # and * only start an emoji as a keycap, which has a selector or
    // U+20E3 right after; non-ASCII may start any.
    const bool maybe = cp >= 0x80 || ((cp == '#' || cp == '*' || (cp >= '0' && cp <= '9')) &&
                                      ((uint8_t)s[1] == 0xEF || (uint8_t)s[1] == 0xE2));
    int g, n;
    if (maybe && (n = emojiMatch(f->emoji, s, &g)) > 0 && (n > el.bytes || !hasText(f, cp))) {
      // A bare character the text fonts draw (©, ™) stays text; an emoji
      // selector or a longer sequence asks for the emoji.
      el.bytes = n;
      if (f->emoji->bitmaps) {
        el.emoji = f->emoji;
        el.emojiGlyph = g;
        el.adv = f->emoji->w * f->emoji->scale;
      } else {                         // this size has no emoji art: one replacement glyph
        el.glyph = resolve(f, REPLACEMENT);
        el.adv = el.glyph.layer->width;
      }
      return el;
    }
  }
  if (layered && cp >= 0x80) {          // nothing below U+0080 is invisible or a flag letter
    if (invisible(cp)) return el;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) cp = 'A' + (cp - 0x1F1E6);   // a flag we lack shows its letters
  }
  el.glyph = resolve(f, cp);
  el.adv = el.glyph.layer->width;
  return el;
}

// The text at s, given whether it is inside a @[mention] (updated). A mention is
// written @[Name]; it shows as @Name on a highlight, the brackets not at all. A "@["
// with no "]" before the end of the line (a preview cut short) is a mention to the end.
static Elem nextElem(const mc_font_t* f, const char* s, bool& mention) {
  if (s[0] == '@' && s[1] == '[' && !mention) {
    mention = true;
    Elem el;
    el.bytes = 2;
    el.glyph = resolve(f, '@');
    el.adv = el.glyph.layer->width;
    el.pill = true;
    return el;
  }
  if (mention && s[0] == ']') {          // the closing bracket takes no room
    mention = false;
    Elem el;
    el.bytes = 1;
    return el;
  }
  if (mention && s[0] == '\n') mention = false;
  Elem el = nextPlain(f, s);
  el.pill = mention && el.bytes;
  return el;
}

int Canvas::elem(const char* s, const mc_font_t* f, int* adv, bool* mention) {
  bool m = mention ? *mention : false;
  Elem el = nextElem(f, s, m);
  if (mention) *mention = m;
  *adv = el.adv;
  return el.bytes;
}

static void drawGlyph(Canvas& c, int x, int y, const Glyph& g, uint8_t fg) {
  if (!g.bits) return;
  const mc_font_t* l = g.layer;
  for (int r = 0; r < l->height; r++) {
    const uint8_t* row = g.bits + r * l->bytes_per_row;
    for (int b = 0; b < l->bytes_per_row; b++)
      for (uint8_t v = row[b], i = 0; v; i++, v <<= 1)
        if (v & 0x80) c.pixel(x + b * 8 + i, y + r, fg);
  }
}

// Draw an element with its top left at (x, y).
static void drawElem(Canvas& c, int x, int y, const Elem& el, const mc_font_t* f, uint8_t fg) {
  if (el.pill) {                       // a mention: a quiet band behind the text, which keeps its colour
    if (el.adv) c.fillRect(x, y, el.adv, f->height, C_SEL);
    if (fg == C_DIM) fg = C_TEXT;      // dim text would sink into the band
  }
  if (el.emoji) {
    if (c.emoji(x, y, el.emoji, el.emojiGlyph)) return;
    drawGlyph(c, x, y, resolve(f, REPLACEMENT), fg);   // too many emoji on screen
  } else {
    drawGlyph(c, x, y, el.glyph, fg);
  }
}

int Canvas::textWidth(const char* s, const mc_font_t* f, bool mention) {
  int w = 0;
  for (;;) {
    Elem el = nextElem(f, s, mention);
    if (!el.bytes) return w;
    w += el.adv;
    s += el.bytes;
  }
}

int Canvas::fitBytes(const char* s, const mc_font_t* f, int maxw, bool mention) {
  const char* p = s;
  int w = 0;
  for (;;) {
    Elem el = nextElem(f, p, mention);
    if (!el.bytes || w + el.adv > maxw) break;
    w += el.adv;
    p += el.bytes;
  }
  return (int)(p - s);
}

int Canvas::text(int x, int y, const char* s, const mc_font_t* f, uint8_t fg, int bg, int maxw, bool* mention) {
  bool m = mention ? *mention : false;
  const bool m0 = m;
  int limit = 1 << 30;
  bool ell = false;
  if (maxw > 0 && textWidth(s, f, m0) > maxw) {
    limit = maxw - f->width;               // room for the ellipsis
    ell = true;
  }
  if (bg >= 0) {
    int w = ell ? maxw : textWidth(s, f, m0);
    fillRect(x, y, w, f->height, (uint8_t)bg);
  }
  int cx = x;
  for (;;) {
    Elem el = nextElem(f, s, m);
    if (!el.bytes) break;
    s += el.bytes;
    if (cx + el.adv - x > limit) break;
    drawElem(*this, cx, y, el, f, fg);
    cx += el.adv;
  }
  if (mention) *mention = m;
  if (ell) {
    // Three dots on the baseline (Spleen's small fonts have no U+2026).
    int by = y + f->height - 3;
    for (int i = 0; i < 3; i++) pixel(cx + i * 2 + (f->width >= 8 ? 1 : 0), by, fg);
    cx += f->width;
  }
  return cx;
}

void Canvas::textScrolled(int x, int y, const char* s, const mc_font_t* f, uint8_t fg, int maxw, int offset) {
  int c0 = _cx0, c1 = _cx1, r0 = _cy0, r1 = _cy1;
  // Intersect with the current clip, draw, restore.
  _cx0 = x > c0 ? x : c0;
  _cx1 = x + maxw < c1 ? x + maxw : c1;
  int cx = x - offset;
  bool m = false;
  for (;;) {
    Elem el = nextElem(f, s, m);
    if (!el.bytes) break;
    s += el.bytes;
    if (cx + el.adv > _cx0 && cx < _cx1) drawElem(*this, cx, y, el, f, fg);
    cx += el.adv;
    if (cx >= _cx1) break;
  }
  _cx0 = c0; _cx1 = c1; _cy0 = r0; _cy1 = r1;
}

// Lay the sprites over rows [y0, y0 + n) of the line buffer. A glyph is run
// length coded, so it is decoded once for the whole band, down to the last row
// the band needs, not once per row.
void Canvas::composeEmoji(bool bigEndian, int y0, int n) {
  uint8_t px[24 * 24];                               // the largest glyph, one byte a pixel
  for (int i = 0; i < _nspr; i++) {
    const Sprite& sp = _spr[i];
    const int ya = sp.vy0 > y0 ? sp.vy0 : y0, yb = sp.vy1 < y0 + n ? sp.vy1 : y0 + n;
    if (ya >= yb) continue;
    const mc_emoji_t* e = sp.e;
    const int rows = (yb - 1 - sp.y) / e->scale + 1;   // glyph rows the band reaches
    emojiDecode(e, sp.glyph, rows, px);
    for (int y = ya; y < yb; y++) {
      const uint8_t* src = &px[((y - sp.y) / e->scale) * e->w];
      uint16_t* out = &s_line[(y - y0) * W];
      for (int x = sp.vx0; x < sp.vx1; x++) {
        const uint8_t v = src[(x - sp.x) / e->scale];
        if (v) out[x] = bigEndian ? (uint16_t)((e->pal[v] >> 8) | (e->pal[v] << 8)) : e->pal[v];
      }
    }
  }
}

void Canvas::flush(hal::Display& d, const uint16_t palette[16], bool force) {
  uint16_t pal[16];
  for (int i = 0; i < 16; i++)
    pal[i] = d.bigEndian() ? (uint16_t)((palette[i] >> 8) | (palette[i] << 8)) : palette[i];

  int bandStart = -1, n = 0;
  for (int y = 0; y <= H; y++) {
    bool dirty = false;
    if (y < H) {
      // FNV-1a over the packed row.
      uint32_t h = 2166136261u;
      const uint8_t* row = &_fb[y * (W / 2)];
      for (int i = 0; i < W / 2; i++) h = (h ^ row[i]) * 16777619u;
      for (int i = 0; i < _nspr; i++)             // a sprite is part of its rows' picture
        if (y >= _spr[i].vy0 && y < _spr[i].vy1) h = (h ^ _spr[i].sig) * 16777619u;
      dirty = force || h != _rowHash[y];
      _rowHash[y] = h;
    }
    if (dirty) {
      if (bandStart < 0) bandStart = y;
      const uint8_t* row = &_fb[y * (W / 2)];
      uint16_t* out = &s_line[n * W];
      for (int i = 0; i < W / 2; i++) {
        out[2 * i] = pal[row[i] >> 4];
        out[2 * i + 1] = pal[row[i] & 15];
      }
      n++;
    }
    if (bandStart >= 0 && (!dirty || n == BAND || y == H - 1)) {
      composeEmoji(d.bigEndian(), bandStart, n);
      d.pushRows(bandStart, n, s_line);
      bandStart = -1;
      n = 0;
    }
  }
}

}  // namespace ui
