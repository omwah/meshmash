// 4-bit palettized framebuffer (two pixels per byte, high nibble left).
//
// The whole UI draws into this every frame; flush() compares each row with
// the previous frame by hash and pushes only rows that changed, expanding
// them through the active palette into a small line buffer. 240x135 costs
// 16.2 KB here plus 3.8 KB of line buffer, instead of 64.8 KB for RGB565.
// The board sets the size with MC_SCREEN_W/H (the Cardputer's by default).
#pragma once
#include <stdint.h>
#include "ui/gfx/font.h"
#include "ui/gfx/utf8.h"
#include "hal/display.h"

#ifndef MC_SCREEN_W
#define MC_SCREEN_W 240
#endif
#ifndef MC_SCREEN_H
#define MC_SCREEN_H 135
#endif
// Colour emoji on screen at once (each costs 20 bytes). A screenful of text
// seldom holds more than a dozen; the T-Deck's larger screen gets more.
#ifndef MC_MAX_EMOJI
#define MC_MAX_EMOJI (MC_SCREEN_W * MC_SCREEN_H > 50000 ? 96 : 48)
#endif

namespace ui {

class Canvas {
public:
  static constexpr int W = MC_SCREEN_W;
  static constexpr int H = MC_SCREEN_H;
  static constexpr int BAND = 8;              // rows per pushRows call

  Canvas();

  void clear(uint8_t c);
  void pixel(int x, int y, uint8_t c);
  uint8_t get(int x, int y) const;
  void fillRect(int x, int y, int w, int h, uint8_t c);
  void hline(int x, int y, int w, uint8_t c) { fillRect(x, y, w, 1, c); }
  void vline(int x, int y, int h, uint8_t c) { fillRect(x, y, 1, h, c); }
  void rect(int x, int y, int w, int h, uint8_t c);
  // Rectangle with its four corner pixels cut, for pills and badges.
  void pill(int x, int y, int w, int h, uint8_t c);
  // 1bpp bitmap, `w` <= 8 per row byte, MSB left; transparent background.
  // `scale` draws each bit as a scale x scale block.
  void bitmap(int x, int y, const uint8_t* rows, int w, int h, uint8_t c, int scale = 1);
  // Same for any width: ceil(w/8) bytes per row, MSB left.
  void bitmapWide(int x, int y, const uint8_t* rows, int w, int h, uint8_t c);

  // UTF-8 text. Returns the x just past the last glyph drawn. bg < 0 is
  // transparent. maxw > 0 clips with an ellipsis when the text is wider.
  int text(int x, int y, const char* s, const mc_font_t* f, uint8_t fg, int bg = -1, int maxw = 0,
           bool* mention = nullptr);
  // `mention`: s starts inside a @[mention] (a wrapped line that continues one); text() takes
  // and updates it the same way. A mention shows as @Name on a highlight, without the brackets.
  static int textWidth(const char* s, const mc_font_t* f, bool mention = false);
  // Bytes of `s` that fit in `maxw` pixels without breaking a UTF-8 sequence.
  static int fitBytes(const char* s, const mc_font_t* f, int maxw, bool mention = false);

  // Text shifted left by `offset` pixels and cut to [x, x+maxw): one frame of
  // a marquee.
  void textScrolled(int x, int y, const char* s, const mc_font_t* f, uint8_t fg, int maxw, int offset);

  // Colour emoji. The framebuffer holds theme palette indices, so a colour
  // glyph is a sprite recorded here and composited over its pixels by flush().
  // Anything drawn over it afterwards covers it, and clear() drops them all, so
  // draw it where the rest of the text goes. False when MC_MAX_EMOJI are
  // already placed. A glyph clipped away entirely counts as drawn.
  bool emoji(int x, int y, const mc_emoji_t* e, int glyph);

  // The text element at the start of s: a glyph, an emoji sequence, or
  // something that takes no room (a variation selector). Returns its bytes
  // (0 at the terminator) and its advance in pixels.
  static int elem(const char* s, const mc_font_t* f, int* adv, bool* mention = nullptr);

  // Limit drawing to a rectangle (for scrolling regions).
  void clip(int x, int y, int w, int h);
  void noClip() { clip(0, 0, W, H); }

  // Push changed rows (or every row when `force`) through `palette`.
  void flush(hal::Display& d, const uint16_t palette[16], bool force);

  const uint8_t* raw() const { return _fb; }

private:
  struct Sprite {
    int16_t x, y;                       // top left, unclipped
    int16_t vx0, vy0, vx1, vy1;         // the part still showing
    uint16_t glyph;
    uint32_t sig;                       // changes whenever the sprite does
    const mc_emoji_t* e;
  };
  void cover(int x0, int y0, int x1, int y1);   // something was drawn over this
  void composeEmoji(bool bigEndian, int y0, int n);

  Sprite _spr[MC_MAX_EMOJI];
  int _nspr = 0;
  uint8_t _fb[W / 2 * H];
  uint32_t _rowHash[H];
  int _cx0, _cy0, _cx1, _cy1;                 // clip, exclusive max
};

}  // namespace ui
