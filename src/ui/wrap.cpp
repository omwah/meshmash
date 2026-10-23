// Word wrapping on glyph advances.
#include "ui/app.h"

namespace ui {

int wrapText(const char* s, const mc_font_t* f, int w, LineFn fn, void* ctx) {
  int line = 0;
  bool mention = false;   // inside a @[mention] (a line can start inside one that began above)
  while (*s) {
    // Find the longest prefix that fits in w pixels (at least one element, so
    // a narrow column still advances), preferring a space break. Elements are
    // glyphs and whole emoji sequences, never a piece of either, and a mention's
    // brackets take no room.
    const char* p = s;
    const char* lastSpace = nullptr;
    bool mentionAtSpace = mention;
    int x = 0;
    while (*p && *p != '\n') {
      int adv;
      const bool before = mention;
      int b = Canvas::elem(p, f, &adv, &mention);
      if (x + adv > w && p > s) { mention = before; break; }
      if (*p == ' ') { lastSpace = p; mentionAtSpace = before; }
      x += adv;
      p += b;
    }
    const char* end = p;
    const char* next = p;
    if (*p == '\n') {
      next = p + 1;               // break exactly at a newline: a mention ends there
      mention = false;
    } else if (*p == ' ') {
      next = p + 1;               // or at a space
    } else if (*p && lastSpace && lastSpace > s) {
      end = lastSpace;
      next = lastSpace + 1;
      mention = mentionAtSpace;
    }
    if (fn) fn(ctx, s, (int)(end - s), line);
    line++;
    s = next;
  }
  return line ? line : 1;
}

}  // namespace ui
