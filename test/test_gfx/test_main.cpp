// Canvas, dirty-row flushing and text layout.
#include <unity.h>
#include <string.h>
#include "ui/app.h"
#include "ui/gfx/emoji.h"
#include "gen/fonts.h"
#include "gen/unicode_cfg.h"

using namespace ui;

class CountingDisplay : public hal::Display {
public:
  int rows = 0, calls = 0;
  uint16_t first = 0;
  bool begin() override { return true; }
  int width() const override { return Canvas::W; }
  int height() const override { return Canvas::H; }
  bool bigEndian() const override { return true; }
  void pushRows(int, int h, const uint16_t* px) override { rows += h; calls++; first = px[0]; }
  void setBacklight(uint8_t) override {}
};

// Keeps every pixel pushed, to look at what a flush composes.
class CapturingDisplay : public hal::Display {
public:
  uint16_t px[Canvas::W * Canvas::H] = {};
  int rows = 0;
  bool begin() override { return true; }
  int width() const override { return Canvas::W; }
  int height() const override { return Canvas::H; }
  bool bigEndian() const override { return false; }
  void pushRows(int y, int h, const uint16_t* p) override {
    memcpy(&px[y * Canvas::W], p, (size_t)h * Canvas::W * 2);
    rows += h;
  }
  void setBacklight(uint8_t) override {}
};
static CapturingDisplay s_cap;

static Canvas s_c;
static const uint16_t kPal[16] = {0x0000, 0x1234};

void test_flush_pushes_only_changed_rows() {
  CountingDisplay d;
  s_c.clear(0);
  s_c.flush(d, kPal, true);
  TEST_ASSERT_EQUAL(Canvas::H, d.rows);
  d.rows = d.calls = 0;
  s_c.flush(d, kPal, false);
  TEST_ASSERT_EQUAL(0, d.rows);
  s_c.fillRect(10, 20, 5, 3, 1);
  s_c.flush(d, kPal, false);
  TEST_ASSERT_EQUAL(3, d.rows);
  TEST_ASSERT_EQUAL(1, d.calls);
}

void test_flush_byte_swaps_for_big_endian_panels() {
  CountingDisplay d;
  s_c.clear(1);
  s_c.flush(d, kPal, true);
  TEST_ASSERT_EQUAL_HEX16(0x3412, d.first);
}

void test_fill_rect_odd_edges_and_clip() {
  s_c.clear(0);
  s_c.fillRect(3, 0, 4, 1, 5);   // x 3..6
  TEST_ASSERT_EQUAL(0, s_c.get(2, 0));
  TEST_ASSERT_EQUAL(5, s_c.get(3, 0));
  TEST_ASSERT_EQUAL(5, s_c.get(6, 0));
  TEST_ASSERT_EQUAL(0, s_c.get(7, 0));
  s_c.clip(0, 0, 4, 4);
  s_c.fillRect(0, 1, 100, 1, 7);
  TEST_ASSERT_EQUAL(7, s_c.get(3, 1));
  TEST_ASSERT_EQUAL(0, s_c.get(4, 1));
  s_c.noClip();
}

void test_utf8_width_counts_code_points() {
  TEST_ASSERT_EQUAL(4 * 8, Canvas::textWidth("caf\xc3\xa9", &font_large));
  TEST_ASSERT_EQUAL(3, Canvas::fitBytes("caf\xc3\xa9", &font_large, 31));   // 'é' doesn't fit
  TEST_ASSERT_EQUAL(5, Canvas::fitBytes("caf\xc3\xa9", &font_large, 32));
}


// --- UTF-8 decoding ------------------------------------------------------

// Decode all of `s` (n bytes, bounded) into code points; returns the count.
static int decodeAll(const char* s, int n, uint32_t* out, int cap) {
  int k = 0;
  for (const char* p = s; p < s + n && k < cap;) {
    int used = utf8Decode(p, s + n, &out[k++]);
    TEST_ASSERT_TRUE(used >= 1 && used <= s + n - p);
    p += used;
  }
  return k;
}

#define DECODES_TO(bytes, expect)                                     \
  do {                                                                \
    uint32_t cp = 0;                                                  \
    int n = utf8Decode(bytes, bytes + sizeof(bytes) - 1, &cp);        \
    TEST_ASSERT_EQUAL_INT((int)sizeof(bytes) - 1, n);                 \
    TEST_ASSERT_EQUAL_HEX32(expect, cp);                              \
  } while (0)

void test_utf8_valid_sequences() {
  DECODES_TO("A", 0x41);
  DECODES_TO("\xc3\xa9", 0xE9);               // 2 bytes
  DECODES_TO("\xc2\x80", 0x80);               // smallest 2-byte
  DECODES_TO("\xdf\xbf", 0x7FF);
  DECODES_TO("\xe2\x82\xac", 0x20AC);         // 3 bytes
  DECODES_TO("\xe0\xa0\x80", 0x800);
  DECODES_TO("\xed\x9f\xbf", 0xD7FF);         // just below the surrogates
  DECODES_TO("\xee\x80\x80", 0xE000);         // just above them
  DECODES_TO("\xef\xbf\xbd", 0xFFFD);
  DECODES_TO("\xf0\x9f\x98\x80", 0x1F600);    // 4 bytes
  DECODES_TO("\xf0\x90\x80\x80", 0x10000);
  DECODES_TO("\xf4\x8f\xbf\xbf", 0x10FFFF);   // the last code point
}

// Each of these must give U+FFFD for exactly `used` bytes, never more.
#define REJECTS(bytes, used)                                                \
  do {                                                                      \
    uint32_t cp = 0;                                                        \
    int n = utf8Decode(bytes, bytes + sizeof(bytes) - 1, &cp);              \
    TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp);                                    \
    TEST_ASSERT_EQUAL_INT(used, n);                                         \
  } while (0)

void test_utf8_rejects_malformed() {
  REJECTS("\x80", 1);                // stray continuation
  REJECTS("\xbf", 1);
  REJECTS("\xff", 1);
  REJECTS("\xfe", 1);
  REJECTS("\xc0\x80", 1);            // overlong NUL
  REJECTS("\xc1\xbf", 1);            // overlong 0x7F
  REJECTS("\xe0\x80\x80", 1);        // overlong 3-byte
  REJECTS("\xe0\x9f\xbf", 1);        // overlong, top of range
  REJECTS("\xf0\x80\x80\x80", 1);    // overlong 4-byte
  REJECTS("\xf0\x8f\xbf\xbf", 1);
  REJECTS("\xed\xa0\x80", 1);        // U+D800, surrogate
  REJECTS("\xed\xbf\xbf", 1);        // U+DFFF
  REJECTS("\xf4\x90\x80\x80", 1);    // U+110000, out of range
  REJECTS("\xf5\x80\x80\x80", 1);    // lead byte never valid
  REJECTS("\xf8\x88\x80\x80\x80", 1);// 5-byte form
  REJECTS("\xe2\x82", 2);            // truncated 3-byte, at the end
  REJECTS("\xf0\x9f\x98", 3);        // truncated 4-byte
  REJECTS("\xc3", 1);                // lead byte alone
}

void test_utf8_resynchronizes_after_damage() {
  uint32_t cp[8];
  // 'a', a 3-byte cut short before 'b', a 4-byte cut short before 'c', then é.
  const char s[] = "a\xe2\x82" "b" "\xf0\x9f" "c" "\xc3\xa9";
  int n = decodeAll(s, sizeof s - 1, cp, 8);
  TEST_ASSERT_EQUAL(6, n);
  TEST_ASSERT_EQUAL_HEX32('a', cp[0]);
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp[1]);
  TEST_ASSERT_EQUAL_HEX32('b', cp[2]);
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp[3]);
  TEST_ASSERT_EQUAL_HEX32('c', cp[4]);
  TEST_ASSERT_EQUAL_HEX32(0xE9, cp[5]);
  // A run of continuation bytes is one replacement each, not a stall.
  const char run[] = "\x80\x80\x80x";
  n = decodeAll(run, sizeof run - 1, cp, 8);
  TEST_ASSERT_EQUAL(4, n);
  TEST_ASSERT_EQUAL_HEX32('x', cp[3]);
}

void test_utf8_never_reads_past_end() {
  // Heap-sized exactly, so ASan flags any read beyond `n` bytes.
  const char* full = "\xf0\x9f\x98\x80";
  for (int n = 0; n <= 4; n++) {
    char* buf = new char[n ? n : 1];
    memcpy(buf, full, n);
    uint32_t cp = 0;
    int used = utf8Decode(buf, buf + n, &cp);
    TEST_ASSERT_EQUAL_INT(n, used);                      // all of it, or nothing
    if (n == 0) TEST_ASSERT_EQUAL_HEX32(0, cp);          // untouched
    else if (n < 4) TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp);
    else TEST_ASSERT_EQUAL_HEX32(0x1F600, cp);
    delete[] buf;
  }
  // An embedded NUL is a character in a bounded buffer, and cuts a sequence.
  uint32_t cp;
  TEST_ASSERT_EQUAL_INT(1, utf8Decode("\0z", "\0z" + 2, &cp));
  TEST_ASSERT_EQUAL_HEX32(0, cp);
  TEST_ASSERT_EQUAL_INT(1, utf8Decode("\xe2\0\0", "\xe2\0\0" + 3, &cp));
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, cp);
}

void test_utf8_next_walks_a_string() {
  const char* s = "h\xc3\xa9\xf0\x9f\x98\x80\xff!";
  const char* p = s;
  TEST_ASSERT_EQUAL_HEX32('h', utf8Next(&p));
  TEST_ASSERT_EQUAL_HEX32(0xE9, utf8Next(&p));
  TEST_ASSERT_EQUAL_HEX32(0x1F600, utf8Next(&p));
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, utf8Next(&p));
  TEST_ASSERT_EQUAL_HEX32('!', utf8Next(&p));
  TEST_ASSERT_EQUAL_INT(0, *p);
  // A sequence cut short by the terminator stops there instead of eating it.
  const char* t = "\xe2\x82";
  TEST_ASSERT_EQUAL_HEX32(0xFFFD, utf8Next(&t));
  TEST_ASSERT_EQUAL_INT(0, *t);
}

void test_utf8_width_with_invalid_input() {
  // One cell per replacement: the truncated 3-byte sequence is one, the
  // rejected surrogate is three (the lead byte and each stray continuation).
  TEST_ASSERT_EQUAL(3 * 8, Canvas::textWidth("a\xe2\x82" "b", &font_large));
  TEST_ASSERT_EQUAL(4 * 8, Canvas::textWidth("\xed\xa0\x80z", &font_large));
}


// --- Font layers ---------------------------------------------------------

// What drawing `text` at (0,0) leaves on an empty canvas: one byte per pixel
// of the first `h` rows and `w` columns.
static void render(const char* text, const mc_font_t* f, uint8_t* out, int w, int h) {
  s_c.noClip();
  s_c.fillRect(0, 0, Canvas::W, Canvas::H, 0);
  s_c.text(0, 0, text, f, 1);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) out[y * w + x] = s_c.get(x, y);
}
static int lit(const uint8_t* px, int n) { int c = 0; for (int i = 0; i < n; i++) c += px[i] != 0; return c; }

// A primary font (4x2 cells) with a double-width, two-byte-per-row layer.
static const mc_run_t kWideRuns[] = {{0x57, 1, 0}, {0xFFFD, 1, 1}};          // 'W', replacement
static const uint8_t kWideBits[] = {
  0x80, 0x10,  0x00, 0x00,    // 'W': pixel 0 and pixel 11 of row 0
  0xFF, 0xF0,  0xFF, 0xF0,    // replacement: a solid 12x2 block
};
static const mc_font_t kWide = {"wide", 12, 2, 2, 0, 2, kWideRuns, kWideBits, nullptr};
static const mc_run_t kNarrowRuns[] = {{0x61, 2, 0}};                      // 'a', 'b'
static const uint8_t kNarrowBits[] = {0x80, 0x00,  0x40, 0x00};
static const mc_font_t kNarrow = {"narrow", 4, 2, 1, 0, 1, kNarrowRuns, kNarrowBits, &kWide};

void test_layer_advance_comes_from_the_layer() {
  // 'a' 4 + 'W' 12 + 'b' 4; an unknown code point takes the replacement, 12 wide.
  TEST_ASSERT_EQUAL(4 + 12 + 4, Canvas::textWidth("aWb", &kNarrow));
  TEST_ASSERT_EQUAL(4 + 12, Canvas::textWidth("a?", &kNarrow));
  TEST_ASSERT_EQUAL(1, Canvas::fitBytes("aWb", &kNarrow, 15));   // 'W' needs 12 of the 11 left
  TEST_ASSERT_EQUAL(2, Canvas::fitBytes("aWb", &kNarrow, 16));
}

void test_layer_glyphs_draw_across_bytes() {
  uint8_t px[16 * 2];
  render("W", &kNarrow, px, 16, 2);
  TEST_ASSERT_EQUAL(1, px[0]);
  TEST_ASSERT_EQUAL(1, px[11]);        // second byte of the row
  TEST_ASSERT_EQUAL(2, lit(px, 16 * 2));
  render("\xef\xbf\xbd", &kNarrow, px, 16, 2);   // U+FFFD directly
  TEST_ASSERT_EQUAL(24, lit(px, 16 * 2));
}

void test_layer_text_clips_and_scrolls_by_layer_width() {
  uint8_t px[16 * 2];
  s_c.noClip();
  s_c.fillRect(0, 0, Canvas::W, Canvas::H, 0);
  s_c.textScrolled(0, 0, "aW", &kNarrow, 1, 16, 4);   // scrolled past the 'a'
  for (int y = 0; y < 2; y++) for (int x = 0; x < 16; x++) px[y * 16 + x] = s_c.get(x, y);
  TEST_ASSERT_EQUAL(1, px[0]);                          // 'W' starts at x=0 now
  TEST_ASSERT_EQUAL(1, px[11]);
}

void test_spleen_ascii_is_unchanged_by_layers() {
  // A layered font draws ASCII from Spleen itself, exactly as a bare one.
  mc_font_t bare = font_large;
  bare.next = nullptr;
  uint8_t a[8 * 16 * 4], b[8 * 16 * 4];
  render("Hamburgefonstiv 0123", &font_large, a, 8 * 4, 16);
  render("Hamburgefonstiv 0123", &bare, b, 8 * 4, 16);
  TEST_ASSERT_EQUAL_MEMORY(a, b, 8 * 4 * 16);
  TEST_ASSERT_TRUE(lit(a, 8 * 4 * 16) > 40);
  TEST_ASSERT_EQUAL(20 * 8, Canvas::textWidth("Hamburgefonstiv 0123", &font_large));
}

void test_generated_fallback_draws_greek_and_marks_unknown() {
#if MC_UNICODE_LAYERS
  TEST_ASSERT_NOT_NULL(font_large.next);
  uint8_t alpha[8 * 16], rep[8 * 16], cjk[8 * 16], q[8 * 16];
  render("\xce\xb1", &font_large, alpha, 8, 16);        // α: Greek comes from the layer
  render("\xef\xbf\xbd", &font_large, rep, 8, 16);       // the replacement glyph
  render("\xe4\xb8\xad", &font_large, cjk, 8, 16);       // 中: in no layer
  render("?", &font_large, q, 8, 16);
  TEST_ASSERT_TRUE(lit(alpha, 8 * 16) > 8);
  TEST_ASSERT_TRUE(memcmp(alpha, rep, sizeof alpha) != 0);
  TEST_ASSERT_TRUE(memcmp(alpha, q, sizeof alpha) != 0);
  TEST_ASSERT_EQUAL_MEMORY(rep, cjk, sizeof rep);        // unknown shows the replacement,
  TEST_ASSERT_TRUE(memcmp(q, cjk, sizeof q) != 0);       // not a question mark
  TEST_ASSERT_EQUAL(8, Canvas::textWidth("\xce\xb1", &font_large));
#else
  // Spleen only: unknown code points show '?' as they always did.
  uint8_t cjk[8 * 16], q[8 * 16];
  render("\xe4\xb8\xad", &font_large, cjk, 8, 16);
  render("?", &font_large, q, 8, 16);
  TEST_ASSERT_EQUAL_MEMORY(q, cjk, sizeof q);
#endif
}

void test_layers_cover_every_size() {
#if MC_UNICODE_LAYERS
  const mc_font_t* fs[] = {&font_small, &font_medium, &font_large, &font_xlarge};
  for (const mc_font_t* f : fs) {
    TEST_ASSERT_NOT_NULL(f->next);
    TEST_ASSERT_EQUAL(f->width, f->next->width);       // text layers keep the cell
    TEST_ASSERT_EQUAL(f->height, f->next->height);
    // Latin-1 stays in Spleen: no fallback needed, so same advance either way.
    TEST_ASSERT_EQUAL(4 * f->width, Canvas::textWidth("caf\xc3\xa9", f));
    // U+0151 (ő, Latin Extended-A) is drawn, not replaced, at the sizes whose
    // profile carries it; otherwise it is the replacement. Neither may crash
    // or change the advance.
    TEST_ASSERT_EQUAL(f->width, Canvas::textWidth("\xc5\x91", f));
  }
#endif
}

static int s_lines;
static char s_line[8][64];
static void collect(void*, const char* s, int len, int line) {
  if (line < 8) { memcpy(s_line[line], s, len); s_line[line][len] = 0; }
  s_lines = line + 1;
}


// --- Colour emoji --------------------------------------------------------
#if MC_UNICODE_EMOJI

static const mc_font_t* const kEmojiFont = &font_large;   // 8x16: 16 px emoji

static int match(const char* s, int* glyph = nullptr) {
  int g = -1;
  int n = emojiMatch(kEmojiFont->emoji, s, &g);
  if (glyph) *glyph = g;
  return n;
}

void test_emoji_match_sequences() {
  TEST_ASSERT_EQUAL(4, match("\xf0\x9f\x98\x80"));                          // 😀
  TEST_ASSERT_EQUAL(6, match("\xe2\x9d\xa4\xef\xb8\x8f"));                  // ❤️ with its selector
  TEST_ASSERT_EQUAL(3, match("\xe2\x9d\xa4"));                              // ❤ without
  TEST_ASSERT_EQUAL(8, match("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd"));          // 👍🏽, a tone
  TEST_ASSERT_EQUAL(8, match("\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"));          // 🇺🇸
  TEST_ASSERT_EQUAL(4 + 3 + 4 + 3 + 4, match("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"));   // 👨‍👩‍👧
  TEST_ASSERT_EQUAL(4, match("\xf0\x9f\xaa\xb6"));                          // 🪶 feather
  TEST_ASSERT_EQUAL(4, match("\xf0\x9f\xa7\xbf"));                          // 🧿 nazar amulet
  TEST_ASSERT_EQUAL(0, match("a"));
  TEST_ASSERT_EQUAL(0, match("\xef\xb8\x8f"));                              // a selector alone
  TEST_ASSERT_EQUAL(0, match(""));
}

void test_emoji_match_is_longest_and_glyph_is_distinct() {
  int plain, tone;
  match("\xf0\x9f\x91\x8d", &plain);                                        // 👍
  int n = match("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", &tone);                 // 👍🏽 is its own glyph
  TEST_ASSERT_EQUAL(8, n);
  TEST_ASSERT_TRUE(plain != tone);
  // A sequence we lack matches its longest known prefix: 👍 then an unknown tone modifier-less rest.
  TEST_ASSERT_EQUAL(4, match("\xf0\x9f\x91\x8d" "x"));
  // Keycaps need their U+20E3.
  TEST_ASSERT_EQUAL(0, match("1"));
}

void test_emoji_bare_text_characters_stay_text() {
  // (c) is in Spleen, so without a selector it is text; with FE0F it is the emoji.
  int adv;
  TEST_ASSERT_EQUAL(2, Canvas::elem("\xc2\xa9", kEmojiFont, &adv));
  TEST_ASSERT_EQUAL(8, adv);
}

void test_emoji_elements_have_double_width_and_invisibles_none() {
  int adv;
  TEST_ASSERT_EQUAL(4, Canvas::elem("\xf0\x9f\x98\x80", kEmojiFont, &adv));
  TEST_ASSERT_EQUAL(16, adv);
  TEST_ASSERT_EQUAL(3, Canvas::elem("\xe2\x80\x8d", kEmojiFont, &adv));      // ZWJ
  TEST_ASSERT_EQUAL(0, adv);
  TEST_ASSERT_EQUAL(3, Canvas::elem("\xef\xb8\x8f", kEmojiFont, &adv));      // FE0F
  TEST_ASSERT_EQUAL(0, adv);
  TEST_ASSERT_EQUAL(4, Canvas::elem("\xf0\x9f\x8f\xbd", kEmojiFont, &adv));  // a lone tone modifier
  TEST_ASSERT_EQUAL(0, adv);
  // A flag we lack shows its two letters.
  const char* de = "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa";
  TEST_ASSERT_EQUAL(2 * 8, Canvas::textWidth(de, kEmojiFont));
  // A keycap is an emoji where the profile has it and otherwise just its digit.
  const char* seven = "7\xef\xb8\x8f\xe2\x83\xa3";
  TEST_ASSERT_EQUAL(match(seven) ? 16 : 8, Canvas::textWidth(seven, kEmojiFont));
  TEST_ASSERT_EQUAL(4 * 16 + 4 * 8, Canvas::textWidth("\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80 abc", kEmojiFont));
}

// The colour of the pixel the last flush put at (x, y).
static uint16_t shown(int x, int y) { return s_cap.px[y * Canvas::W + x]; }

void test_emoji_flush_composites_colour_over_the_framebuffer() {
  s_c.noClip();
  s_c.clear(0);
  s_cap = CapturingDisplay();
  s_c.text(0, 0, "\xf0\x9f\x98\x80", kEmojiFont, 1);                       // 😀, 16x16
  s_c.flush(s_cap, kPal, true);
  // Somewhere on the face is yellow (R high, G high, B low), a colour no palette slot holds.
  bool yellow = false, theme = false;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      uint16_t c = shown(x, y);
      int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
      if (r > 24 && g > 40 && b < 12) yellow = true;
      if (c != kPal[0]) theme = theme || (c == kPal[1]);
    }
  TEST_ASSERT_TRUE(yellow);
  TEST_ASSERT_FALSE(theme);
  TEST_ASSERT_EQUAL_HEX16(kPal[0], shown(20, 0));                           // beside it: background
  TEST_ASSERT_EQUAL_HEX16(kPal[0], shown(0, 0));                            // the corner is transparent
}

void test_emoji_decode_run_length_streams() {
  // 4x2: [1 1 1 2 / 0 0 3 3] as runs (3,1) (1,2) (2,0) (2,3), then a second
  // glyph 4x2 [5 5 5 5 / 5 5 5 5] as one run of eight crossing the row end
  // (two runs of at most 16: here 8 = (8,5)).
  static const uint8_t rle[] = {0x21, 0x02, 0x10, 0x13,   0x75};
  static const uint32_t offs[] = {0, 4, 5};
  static const uint16_t pal[16] = {};
  static const mc_emoji_t e = {4, 2, 1, pal, rle, offs, nullptr};
  uint8_t px[8];
  emojiDecode(&e, 0, 2, px);
  static const uint8_t want0[] = {1, 1, 1, 2, 0, 0, 3, 3};
  TEST_ASSERT_EQUAL_MEMORY(want0, px, 8);
  emojiDecode(&e, 1, 2, px);
  static const uint8_t want1[] = {5, 5, 5, 5, 5, 5, 5, 5};
  TEST_ASSERT_EQUAL_MEMORY(want1, px, 8);
  memset(px, 9, sizeof px);
  emojiDecode(&e, 0, 1, px);                               // only the first row is written
  TEST_ASSERT_EQUAL_MEMORY(want0, px, 4);
  TEST_ASSERT_EQUAL(9, px[4]);
  // A run longer than the rows asked for is cut there, not past the buffer.
  memset(px, 9, sizeof px);
  emojiDecode(&e, 1, 1, px);
  TEST_ASSERT_EQUAL(5, px[3]);
  TEST_ASSERT_EQUAL(9, px[4]);
}

void test_emoji_are_centred_with_a_margin() {
  // Faces fill their cell, hands and symbols are small, flags are wide: each
  // must still sit in the middle of its 16 px cell with room on every side, or
  // a row of emoji looks unevenly spaced.
  const char* const set[] = {"\xf0\x9f\x98\x80", "\xf0\x9f\x91\x8d", "\xe2\x9a\xa1", "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8",
                             "\xf0\x9f\x94\x8b", "\xf0\x9f\xa5\xb0", "\xf0\x9f\xa4\xaf", "\xe2\x9c\x8c\xef\xb8\x8f"};
  for (const char* e : set) {
    s_c.noClip();
    s_c.clear(0);
    s_cap = CapturingDisplay();
    s_c.text(0, 0, e, kEmojiFont, 1);
    s_c.flush(s_cap, kPal, true);
    int x0 = 99, y0 = 99, x1 = -1, y1 = -1;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++)
        if (shown(x, y) != kPal[0]) {
          x0 = x < x0 ? x : x0; y0 = y < y0 ? y : y0;
          x1 = x > x1 ? x : x1; y1 = y > y1 ? y : y1;
        }
    TEST_ASSERT_TRUE_MESSAGE(x1 >= 0, e);
    const int l = x0, r = 15 - x1, t = y0, b = 15 - y1;
    TEST_ASSERT_TRUE_MESSAGE(l >= 1 && r >= 1 && t >= 1 && b >= 1, e);       // never touching the cell edge
    TEST_ASSERT_TRUE_MESSAGE(l - r <= 1 && r - l <= 1, e);                   // centred, to a pixel
    TEST_ASSERT_TRUE_MESSAGE(t - b <= 1 && b - t <= 1, e);
  }
}

void test_emoji_is_covered_by_later_drawing() {
  s_c.noClip();
  s_c.clear(0);
  s_cap = CapturingDisplay();
  s_c.text(0, 0, "\xf0\x9f\x98\x80", kEmojiFont, 1);
  s_c.fillRect(0, 0, 16, 16, 1);                                            // a panel over all of it
  s_c.flush(s_cap, kPal, true);
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) TEST_ASSERT_EQUAL_HEX16(kPal[1], shown(x, y));
  // Over half of it: the other half stays.
  s_c.clear(0);
  s_c.text(0, 0, "\xf0\x9f\x98\x80", kEmojiFont, 1);
  s_c.fillRect(8, 0, 8, 16, 1);
  s_c.flush(s_cap, kPal, true);
  bool left = false;
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 8; x++) left = left || shown(x, y) != kPal[0];
    for (int x = 8; x < 16; x++) TEST_ASSERT_EQUAL_HEX16(kPal[1], shown(x, y));
  }
  TEST_ASSERT_TRUE(left);
}

void test_emoji_respects_the_clip() {
  s_c.noClip();
  s_c.clear(0);
  s_cap = CapturingDisplay();
  s_c.clip(0, 0, 8, 8);
  s_c.text(0, 0, "\xf0\x9f\x98\x80", kEmojiFont, 1);
  s_c.noClip();
  s_c.flush(s_cap, kPal, true);
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      if (x >= 8 || y >= 8) TEST_ASSERT_EQUAL_HEX16(kPal[0], shown(x, y));
}

void test_emoji_change_repushes_rows_even_if_the_framebuffer_did_not() {
  CountingDisplay d;
  s_c.noClip();
  s_c.clear(0);
  s_c.flush(d, kPal, true);                                                 // everything pushed once
  s_c.clear(0);
  s_c.text(0, 0, "\xf0\x9f\x98\x80", kEmojiFont, 1);
  d.rows = 0;
  s_c.flush(d, kPal, false);
  TEST_ASSERT_TRUE(d.rows >= 16);                                           // the emoji appeared
  s_c.clear(0);
  d.rows = 0;
  s_c.flush(d, kPal, false);
  TEST_ASSERT_TRUE(d.rows >= 16);                                           // and went again
  d.rows = 0;
  s_c.flush(d, kPal, false);
  TEST_ASSERT_EQUAL(0, d.rows);                                             // nothing changed
}

void test_emoji_wrap_never_splits_a_sequence() {
  memset(s_line, 0, sizeof s_line);
  // Six 16 px emoji in 80 px: five, then one.
  const char* six = "\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80";
  int n = wrapText(six, kEmojiFont, 80, collect, nullptr);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL(20, (int)strlen(s_line[0]));
  TEST_ASSERT_EQUAL(4, (int)strlen(s_line[1]));
  // A family is one element: it stays whole on the next line.
  memset(s_line, 0, sizeof s_line);
  const char* fam = "ab \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7";
  n = wrapText(fam, kEmojiFont, 3 * 8 + 8, collect, nullptr);   // room for "ab " plus half the family
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("ab", s_line[0]);
  TEST_ASSERT_EQUAL(18, (int)strlen(s_line[1]));
}

void test_smallest_size_shows_one_replacement_per_emoji() {
  // 5x8 has no emoji art: a matched sequence takes one cell, not one per code point.
  TEST_ASSERT_EQUAL(5, Canvas::textWidth("\xe2\x9d\xa4\xef\xb8\x8f", &font_small));
  TEST_ASSERT_EQUAL(5, Canvas::textWidth("\xf0\x9f\x98\x80", &font_small));
}

void test_emoji_scale_doubles_the_art() {
  // The 24 row font draws 12 px art at 2x on the Cardputer profile and 24 px art on std;
  // either way an emoji is two 12 px cells.
  int adv;
  Canvas::elem("\xf0\x9f\x98\x80", &font_xlarge, &adv);
  TEST_ASSERT_EQUAL(24, adv);
}

#endif  // MC_UNICODE_EMOJI


// --- @[mentions] ---------------------------------------------------------

void test_mention_brackets_take_no_room() {
  // "@[Nick]" shows as "@Nick": five cells, not seven.
  TEST_ASSERT_EQUAL(5 * 8, Canvas::textWidth("@[Nick]", &font_large));
  TEST_ASSERT_EQUAL((5 + 1 + 2) * 8, Canvas::textWidth("@[Nick] hi", &font_large));
  // Ordinary brackets and a lone @ stay as typed.
  TEST_ASSERT_EQUAL(10 * 8, Canvas::textWidth("[brackets]", &font_large));
  TEST_ASSERT_EQUAL(5 * 8, Canvas::textWidth("a@b.c", &font_large));
  // Cut short (a preview): a mention to the end, still without the opening "@[".
  TEST_ASSERT_EQUAL(4 * 8, Canvas::textWidth("@[Ali", &font_large));
}

void test_mention_is_drawn_on_the_selection_colour() {
  s_c.noClip();
  s_c.clear(0);
  s_c.text(0, 0, "@[Ab] c", &font_large, C_TEXT);
  // Inside the three-cell mention every pixel is the selection colour or the accent text;
  // the cells after it are the plain background.
  bool accent = false;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 3 * 8; x++) {
      uint8_t p = s_c.get(x, y);
      TEST_ASSERT_TRUE(p == C_SEL || p == C_ACCENT);
      accent = accent || p == C_ACCENT;
    }
  TEST_ASSERT_TRUE(accent);
  for (int y = 0; y < 16; y++) TEST_ASSERT_EQUAL(C_BG, s_c.get(3 * 8 + 1, y));   // the gap after it
}

void test_mention_continues_over_a_wrapped_line() {
  // A name too long for the line: the second line starts inside the mention, so it is
  // still highlighted and its closing bracket still takes no room.
  memset(s_line, 0, sizeof s_line);
  int n = wrapText("@[aaa bbb ccc] ok", &font_large, 8 * 9, collect, nullptr);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("@[aaa bbb", s_line[0]);
  TEST_ASSERT_EQUAL_STRING("ccc] ok", s_line[1]);
  bool m = true;                                   // as drawLine passes it on
  TEST_ASSERT_EQUAL(3 * 8 + 8 + 2 * 8, Canvas::textWidth("ccc] ok", &font_large, true));
  s_c.noClip();
  s_c.clear(0);
  s_c.text(0, 0, "@[aaa bbb", &font_large, C_TEXT, -1, 0, &m);
  TEST_ASSERT_TRUE(m);                             // the first line ended inside it
  s_c.text(0, 0, "ccc] ok", &font_large, C_TEXT, -1, 0, &m);
  TEST_ASSERT_FALSE(m);                            // and the second closed it
  bool sel = false;                                // the second line's cells are on the highlight
  for (int x = 0; x < 3 * 8; x++) sel = sel || s_c.get(x, 1) == C_SEL;
  TEST_ASSERT_TRUE(sel);
}

void test_mention_ends_at_a_newline() {
  memset(s_line, 0, sizeof s_line);
  wrapText("@[open\nnext line", &font_large, 8 * 20, collect, nullptr);
  TEST_ASSERT_EQUAL_STRING("@[open", s_line[0]);
  TEST_ASSERT_EQUAL_STRING("next line", s_line[1]);
  TEST_ASSERT_EQUAL(9 * 8, Canvas::textWidth("next line", &font_large));         // plain: nothing carried over
}

void test_wrap_breaks_at_spaces() {
  memset(s_line, 0, sizeof s_line);
  int n = wrapText("hello world again", &font_large, 8 * 11, collect, nullptr);  // 11 cols
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_STRING("hello world", s_line[0]);
  TEST_ASSERT_EQUAL_STRING("again", s_line[1]);
}

void test_wrap_hard_breaks_long_words() {
  int n = wrapText("abcdefghijkl", &font_large, 8 * 5, collect, nullptr);
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL_STRING("abcde", s_line[0]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_flush_pushes_only_changed_rows);
  RUN_TEST(test_flush_byte_swaps_for_big_endian_panels);
  RUN_TEST(test_fill_rect_odd_edges_and_clip);
  RUN_TEST(test_utf8_width_counts_code_points);
  RUN_TEST(test_utf8_valid_sequences);
  RUN_TEST(test_utf8_rejects_malformed);
  RUN_TEST(test_utf8_resynchronizes_after_damage);
  RUN_TEST(test_utf8_never_reads_past_end);
  RUN_TEST(test_utf8_next_walks_a_string);
  RUN_TEST(test_utf8_width_with_invalid_input);
  RUN_TEST(test_layer_advance_comes_from_the_layer);
  RUN_TEST(test_layer_glyphs_draw_across_bytes);
  RUN_TEST(test_layer_text_clips_and_scrolls_by_layer_width);
  RUN_TEST(test_spleen_ascii_is_unchanged_by_layers);
  RUN_TEST(test_generated_fallback_draws_greek_and_marks_unknown);
  RUN_TEST(test_layers_cover_every_size);
#if MC_UNICODE_EMOJI
  RUN_TEST(test_emoji_match_sequences);
  RUN_TEST(test_emoji_match_is_longest_and_glyph_is_distinct);
  RUN_TEST(test_emoji_bare_text_characters_stay_text);
  RUN_TEST(test_emoji_elements_have_double_width_and_invisibles_none);
  RUN_TEST(test_emoji_flush_composites_colour_over_the_framebuffer);
  RUN_TEST(test_emoji_decode_run_length_streams);
  RUN_TEST(test_emoji_are_centred_with_a_margin);
  RUN_TEST(test_emoji_is_covered_by_later_drawing);
  RUN_TEST(test_emoji_respects_the_clip);
  RUN_TEST(test_emoji_change_repushes_rows_even_if_the_framebuffer_did_not);
  RUN_TEST(test_emoji_wrap_never_splits_a_sequence);
  RUN_TEST(test_smallest_size_shows_one_replacement_per_emoji);
  RUN_TEST(test_emoji_scale_doubles_the_art);
#endif
  RUN_TEST(test_mention_brackets_take_no_room);
  RUN_TEST(test_mention_is_drawn_on_the_selection_colour);
  RUN_TEST(test_mention_continues_over_a_wrapped_line);
  RUN_TEST(test_mention_ends_at_a_newline);
  RUN_TEST(test_wrap_breaks_at_spaces);
  RUN_TEST(test_wrap_hard_breaks_long_words);
  return UNITY_END();
}
