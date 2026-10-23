# Unicode and emoji

Text is drawn from Spleen first. A code point Spleen lacks falls through a
chain of fallback layers (`mc_font_t::next`, `src/ui/gfx/font.h`), and one that
no layer has shows a replacement box. A layer's glyph advance is its own cell
width, so layers of a different width lay out correctly. Colour emoji are
matched as whole sequences before any of that (below).

Layers are bitmaps generated at build time by `scripts/gen_fonts.py`,
`scripts/gen_unicode.py` and `scripts/gen_emoji.py` into `src/gen/` (never
committed). The firmware parses no fonts and allocates nothing.

## Choosing coverage

A *profile* says which groups of code points and which emoji each Spleen size
gets. The firmware environments pick one in `platformio.ini`:

| Environment | `custom_unicode` |
|---|---|
| `cardputer_adv*`, `sim`, `simnet*` | `min` |
| `tdeck*`, `sim_tdeck` | `std` |

Override it for one build with `MC_UNICODE`:

    MC_UNICODE=none pixi run build        # Spleen only
    MC_UNICODE=std,+math pixi run sim     # a profile plus a group
    MC_UNICODE=min,-greek pixi run sim    # ... or minus one
    MC_UNICODE=min,+0x2600-0x26ff pixi run sim   # ... or a raw range
    MC_UNICODE=min,+emoji-common pixi run sim    # ... or an emoji tier
    MC_UNICODE=std,-emoji pixi run sim           # ... or no emoji

or in `platformio.local.ini` (git-ignored):

    [env:cardputer_adv]
    custom_unicode = min,+arrows

Groups, profiles and emoji settings live in `scripts/unicode_profiles.py`; the
emoji themselves in `scripts/emoji_set.py`. Add to those rather than touching
the renderer. `pixi run fonts` regenerates with the default profile and
`python -I scripts/gen_fonts.py --report` prints what each layer costs (also
left in `src/gen/unicode_report.txt`). Code points the source font does not
have are skipped and listed in the report. About shows the profile a firmware
was built with, and `scripts/check-size.py` fails a build whose profile is not
the one its environment is configured for.

Policy: emoji come first, and rarely used writing systems and symbols are
left out, most of all on the Cardputer, whose flash is tight.

## What is covered

Groups the layers can add on top of Spleen:

| Group | Code points |
|---|---|
| `latin-ext-a` | U+0100–U+017F |
| `greek` | U+0386, U+0388–U+038A, U+038C, U+038E–U+03A1, U+03A3–U+03CE |
| `cyrillic` | U+0400–U+045F, U+0490–U+0491 |
| `punct` | U+2010–U+2015, U+2018–U+201E, U+2020–U+2022, U+2026, U+2030, U+2039–U+203A, U+2116, U+2122 |
| `currency` | U+20A9, U+20AC, U+20B9, U+20BA, U+20BD |
| `arrows` | U+2190–U+2195 |
| `math` | U+2212, U+2248, U+2260, U+2264–U+2265, U+221E |

Spleen itself draws Latin-1 (U+0020–U+007E, U+00A0–U+00FF) at every size, basic
Cyrillic (72 of the 98 code points of `cyrillic`) at 6x12 and 8x16, all of
Latin Extended-A and the arrows at 8x16, some punctuation (11 of 22) at 8x16
and 12x24, and no Greek at all. A profile's layers add the rest of each group
at each size:

| Size | `min` (Cardputer) adds | `std` (T-Deck) adds |
|---|---|---|
| 5x8 | Latin Ext-A, punct, currency | Latin Ext-A, Cyrillic, punct, currency |
| 6x12 | Latin Ext-A, Greek, punct, currency, arrows | + math |
| 8x16 | Greek, punct, currency | + math |
| 12x24 | punct | Latin Ext-A, Greek, Cyrillic, punct, currency, arrows |

So under `min` the 5x8 size has no Cyrillic and the 12x24 size has no Greek,
Cyrillic or Latin Extended-A; `std` closes those at 12x24 and 5x8.

Not covered, and shown as the replacement box: CJK, Arabic, Hebrew, the Indic
scripts, Thai, Korean, Vietnamese, Latin Extended-B and Additional, and symbol blocks (box drawing, mathematical operators, dingbats)
except the groups above. `full-cjk-common` is not implemented: it needs a CJK
source font, whose size would dwarf everything here.

## Colour emoji

Emoji are drawn in their own colours, from Noto Color Emoji. The framebuffer
holds only theme palette indices (all 16 are theme roles), so an emoji is a
*sprite*: `Canvas::emoji()` records it and `Canvas::flush()` composites its
pixels over the rows it belongs to. The rules that keep that correct:

- A sprite remembers the clip it was drawn under. Anything drawn over it later
  (`fillRect`, bitmaps) cuts it back to the largest part still showing, or
  removes it; `clear()` drops all of them. A frame is redrawn from `clear()`,
  so sprites never outlive it. Glyphs of text drawn straight over an emoji
  without a fill behind them do not hide it.
- A sprite is part of its rows' hash, so a row is pushed again when an emoji
  appears or goes even though the framebuffer behind it did not change.
- At most `MC_MAX_EMOJI` (48; 96 on the T-Deck) are placed per frame; the rest
  show the replacement glyph. That is 20 bytes of RAM each.

Sequences are found by longest match over a sorted table shared by all sizes
(`emojiMatch`). U+FE0F and U+FE0E are ignored wherever they appear. A bare
character the text fonts draw (©, ™) stays text; it is the emoji only with a
selector. What does not match degrades without boxes:

| Text | Shown |
|---|---|
| Variation selectors, ZWJ, zero-width marks, tag characters, skin tone modifiers | nothing (they take no room) |
| A ZWJ sequence we lack | its parts, each as an emoji if we have it |
| A skin tone we lack | the base emoji |
| A flag we lack | its two letters (🇩🇪 → `DE`) |
| A keycap we lack | its digit |
| An emoji we lack | the replacement box |
| Any emoji at 5x8 (no art that small) | one replacement box per sequence |

Each emoji is two cells wide: 12x12 art at 6x12, 16x16 at 8x16, and 24x24 at
12x24 (on the Cardputer profile the same 12x12 art at 2x, to save flash). Every
glyph is centred on its visible content in a box one pixel in from its cell
edge, scaled down if larger and never up, so a row of emoji is evenly spaced.

Art is run-length coded (a byte per run of up to 16 equal pixels, runs
crossing row ends) with one shared 15-colour palette per size, index 0
transparent. The palette is chosen by k-means weighted so greens and blues are
not lost to the yellows that fill the faces. `flush` decodes a glyph once per
band of up to 8 rows, down to the last row the band needs.

The emoji a build carries are `scripts/emoji_set.py` and the font itself: `core`
(105: everyday reactions, signs, and what a mesh radio user talks about; the
Cardputer), `common` (359 more, in order of use) and `all` (every other sequence
Noto Color Emoji draws: 5,644 sequences in all, over 4,003 distinct images, with
sequences that share art pointing at one bitmap). The T-Deck's `std` profile takes
all three, at three sizes. `bytes` in a profile can cap the uncompressed bitmaps
(none is set now). Offsets into the art are 32-bit, because one size passes 64 KB
coded (24 px: 555 KB). The full set costs about 1.1 MB of flash, which is why only the
T-Deck, with a 6.25 MB app partition, carries it; `MC_UNICODE=std,-emoji-all` goes
back to the 462 most useful (about 125 KB).

## Measurements

Flash and static RAM, from the same code built with each profile (bytes, so
the profile is the only difference):

| Build | Spleen only | `min` | `std` |
|---|---|---|---|
| `cardputer_adv` flash | 989,232 | 1,010,560 (+20.8 KB) | 2,187,344 (+1,169 KB) |
| `tdeck` flash | 996,880 | 1,018,208 (+20.8 KB) | 2,194,992 (+1,169 KB) |
| static RAM (any profile) | 118,792 (Cardputer) | | |
| static RAM (any profile) | 62,160 (T-Deck) | | |

`min` is the Cardputer's profile and `std` the T-Deck's; a Cardputer build with `std`
would not fit its flash budget.

Before any of this work (the CI run after the navigation icons, which changed
almost nothing), `cardputer_adv` was 965.9 KB and 114.8 KB of static RAM, and
`tdeck` 973.5 KB and 60.6 KB. A Spleen-only build is now 964.8 KB, a little
smaller, because font tables list runs of code points instead of one `uint32`
each. The Cardputer's default build is 985.0 KB (+19 KB) and 115.9 KB RAM
(+1.1 KB: the 48 sprites). The T-Deck keeps the sprites in PSRAM with the rest
of the UI, so its internal RAM is unchanged. The Cardputer budgets in
`scripts/check-size.py` rose by 25 KB for this; the T-Deck's did not need to.

Nothing is allocated on the heap. The stack grows by a 576-byte decode buffer
in `flush` and under 100 bytes in `emojiMatch`.

Compression of the emoji art, measured before choosing it (bytes; `RLE` is
the format above with a 2-byte offset per glyph):

| Set and size | Raw 4bpp | RLE | zlib per glyph | zlib whole size |
|---|---|---|---|---|
| `min`, 12x12 | 7,416 | 4,991 (67%) | 5,793 (78%) | 2,996 |
| `min`, 16x16 | 13,184 | 7,883 (60%) | 8,116 (62%) | 4,715 |
| 307 emoji, 24x24 | 88,416 | 41,618 (47%) | 37,478 (42%) | 26,196 |

RLE saves a third to a half and decodes with a loop that costs nothing
noticeable; zlib per glyph saves no more and needs an inflater, and zlib over a
whole size could only be used by unpacking it into RAM, which this avoids.

Text cost on the host (`scripts/bench-text.sh`, optimised, no sanitizers;
nanoseconds per element, 8x16, ±20% run to run). The ESP32-S3 is slower by a
roughly constant factor, so read these as relative cost:

| Text | Spleen only | `min` | `std` |
|---|---|---|---|
| ASCII, draw | 124 | 122 | 125 |
| ASCII, measure | 11 | 10 | 10 |
| Latin-1 accents (Spleen), measure | 12 | 16 | 17 |
| Greek (layer), draw | 127 (shows `?`) | 183 | 191 |
| Emoji, draw (single code point) | | 170 | 212 |
| Emoji, measure | | 113 | 157 |
| `wrapText` of a 137-byte message, total | 1,635 | 2,512 | 3,450 |
| Full-screen flush, 48 emoji on screen | +1% | +113% (43 → 91 µs) | +119% (43 → 94 µs) |

ASCII costs the same whether layers are on or not (it takes a fast path before
any emoji or invisible-character test). Emoji cost about ten times an ASCII
character to measure, which only matters on screens full of them; a flush that
composites 48 emoji costs about twice a plain one. These were measured on the
host only: nothing here has been timed on a device.

## Sources and licences

| Layer | Source | Licence |
|---|---|---|
| Spleen | `fonts/spleen-*.bdf` | BSD 2-Clause |
| Text fallback | Noto Sans Mono 2.014 (Regular; Bold at 8x16) | SIL OFL 1.1 |
| Colour emoji | Noto Color Emoji 2.057 (CBDT bitmaps) | SIL OFL 1.1 |
| Navigation and splash icons | Material Symbols Rounded | Apache 2.0 |

Upstream files are pinned by SHA-256 in `scripts/assets.lock`, fetched into the
git-ignored `.cache/assets/` by `scripts/fetch_assets.py`, and not shipped. The
compiled bitmaps are derived from them: see `NOTICE` and `licenses/`. The OFL
texts are vendored byte for byte, and `scripts/check-licenses.py` (part of
`pixi run ci`) fails if an asset has no allowed licence on file, no attribution
in `NOTICE`, a licence text that differs from what upstream publishes, or any
sign of GNU Unifont or another GPL glyph source in the tree. The upstream
notices declare no Reserved Font Name; the generators ship no font and rename
nothing. That is a statement of what is done here, not legal advice.

Noto Sans Mono's glyphs are drawn hinted and thresholded to one bit at the
Spleen cell size and baseline; Bold is used at 8x16 so strokes match Spleen's
two-pixel stems.

## Limits

No text shaping, no right-to-left layout, and combining marks are not
composed. Only the groups and emoji in a profile are drawn; everything else
shows the replacement box. Text glyphs are one-bit in the theme's colours;
only emoji carry their own. A screen with more emoji than `MC_MAX_EMOJI` shows
boxes for the extras. The line editor counts a character per code point, so an
emoji typed or pasted there is not laid out as two cells. Nothing has been
tested on a device: the simulator and the unit tests are the evidence.
