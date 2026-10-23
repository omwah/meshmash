// Fixed-cell bitmap font, as scripts/bdf2c.py and scripts/gen_unicode.py emit
// it. Glyphs are `height` rows of `bytes_per_row` bytes, MSB leftmost, 1bpp.
//
// Code points are listed as runs of consecutive values: run r covers
// [first, first + count) and its glyphs start at glyph number `index`, so the
// glyph for cp is number runs[r].index + (cp - runs[r].first). Runs are sorted
// by `first`. Sparse sets (Greek, a few symbols) cost a few bytes, not four
// per glyph.
//
// A font may name a fallback layer in `next`: another font with its own cell
// size (a double-width layer has width = 2 * the primary's), tried in order
// when a code point is not in this one. The advance for a glyph is the width
// of the layer it came from.
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t first;
    uint16_t count;
    uint16_t index;
} mc_run_t;

// Colour emoji, drawn as sprites (ui/gfx/emoji.h). One sequence table is shared
// by every size; each size has its own bitmaps and palette.
//
// A glyph is w x h pixels, row after row, run-length coded: each byte is a run
// of 1..16 equal pixels, the run length minus one in the high nibble and the
// pixel in the low nibble (0 transparent, 1..15 select `pal`, RGB565 with
// entry 0 unused). Runs cross row ends. Glyph i is the stream from offs[i] (32 bits: a size can pass 64 KB);
// the next glyph's offset ends it. It is drawn `scale` times larger, so the
// advance is w * scale. `bitmaps` may be null (a size too small to show
// emoji): a matched sequence then takes one replacement glyph's place.
typedef struct {
    uint16_t off;      // first code point in `cps`
    uint8_t len;
    uint16_t glyph;    // index into the bitmaps
} mc_seq_t;

typedef struct {
    uint16_t nseqs;
    uint8_t maxlen;    // longest sequence, in code points
    const mc_seq_t *seqs;   // sorted by code points, lexicographically
    const uint32_t *cps;
} mc_emoji_seqs_t;

typedef struct {
    uint8_t w, h, scale;
    const uint16_t *pal;
    const uint8_t *bitmaps;
    const uint32_t *offs;
    const mc_emoji_seqs_t *seqs;
} mc_emoji_t;

typedef struct mc_font {
    const char *name;
    uint8_t width, height, bytes_per_row, max_cols;
    uint16_t nruns;
    const mc_run_t *runs;
    const uint8_t *bitmaps;
    const struct mc_font *next;
    const mc_emoji_t *emoji;   // null: this font draws no emoji
} mc_font_t;
