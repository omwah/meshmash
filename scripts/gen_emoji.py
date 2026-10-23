"""Rasterize colour emoji layers for the firmware (build-time only).

Reads Noto Color Emoji (a CBDT bitmap font, SIL OFL 1.1), finds each sequence
of the chosen tiers (scripts/emoji_set.py) through its cmap and `ccmp`
ligatures, scales the bitmap to the emoji size of each Spleen size, quantizes
all of a size's glyphs to one shared 15-colour palette (index 0 is
transparent) and writes run-length coded tables (ui/gfx/font.h). Needs Pillow and
fontTools, which the pixi environment has.
"""
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

FE0F = 0xFE0F


class EmojiFont:
    def __init__(self, path):
        from fontTools.ttLib import TTFont
        f = TTFont(path)
        cmap = f.getBestCmap()
        inv = {}
        for cp, g in cmap.items():
            inv.setdefault(g, cp)
        self.seq = {}                       # (code points without FE0F) -> glyph
        for cp, g in cmap.items():
            if cp != FE0F:
                self.seq.setdefault((cp,), g)
        for lookup in f["GSUB"].table.LookupList.Lookup:
            for st in lookup.SubTable:
                st = getattr(st, "ExtSubTable", st)
                if st.LookupType != 4:
                    continue
                for first, ligs in st.ligatures.items():
                    for lig in ligs:
                        comps = [first] + list(lig.Component)
                        if all(c in inv for c in comps):
                            key = tuple(inv[c] for c in comps if inv[c] != FE0F)
                            self.seq[key] = lig.LigGlyph
        self.strike = f["CBDT"].strikeData[0]
        self.version = f["name"].getDebugName(5)

    def glyph(self, key):
        return self.seq.get(key)

    def png(self, glyph):
        return self.strike[glyph].imageData


SRC_CROP = (4, 0, 132, 128)   # the square that holds every glyph's content


MARGIN = 1   # empty pixels kept on every side of a glyph, so neighbours never touch


def scaled(png, n):
    """The glyph as an n x n RGBA image, alpha already thresholded.

    Noto places each glyph by its own design, so left to the font a face fills
    its cell while a hand or a symbol floats in it, and a row of emoji looks
    unevenly spaced. Each glyph is instead centred on its visible content in a
    box MARGIN pixels in from the cell edge: scaled down if it is larger than
    that box, never scaled up (a small glyph stays small).
    """
    from PIL import Image
    src = Image.open(io.BytesIO(png)).convert("RGBA").crop(SRC_CROP)
    bb = src.getchannel("A").point(lambda v: 255 if v >= 128 else 0).getbbox()
    src = src.crop(bb)
    bw, bh = src.size
    inner = n - 2 * MARGIN
    nat = n / (SRC_CROP[2] - SRC_CROP[0])              # the whole crop at n pixels
    k = min(1.0, inner / (max(bw, bh) * nat)) * nat    # pixels per source pixel
    w, h = max(1, round(bw * k)), max(1, round(bh * k))
    w, h = min(w + ((n - w) & 1), inner), min(h + ((n - h) & 1), inner)   # even margins: truly centred
    if (n - w) & 1:
        w -= 1
    if (n - h) & 1:
        h -= 1
    im = src.convert("RGBa").resize((w, h), Image.LANCZOS).convert("RGBA")
    out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    out.paste(im, ((n - w) // 2, (n - h) // 2))
    px = out.load()
    for y in range(n):
        for x in range(n):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
    # Thresholding trims faint edge pixels, so centre once more on what is left
    # (a margin sum that is odd leaves one pixel of difference, no more).
    bb = out.getchannel("A").getbbox()
    dx, dy = ((n - bb[2]) - bb[0]) // 2, ((n - bb[3]) - bb[1]) // 2
    if dx or dy:
        moved = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        moved.paste(out, (dx, dy))
        out = moved
    return out


def shared_palette(images, colors=15):
    """One palette for every image of a size: k-means over the distinct colours.

    Median cut hands most entries to the yellows that fill the faces and leaves
    greens and blues muddy, so each colour counts by the square root of its
    pixels and the seeds are spread by distance (k-means++, deterministic).
    """
    from collections import Counter
    count = Counter(p[:3] for im in images for p in im.getdata() if p[3])
    pts = sorted(count)                                   # sorted: reproducible
    wts = [count[p] ** 0.5 for p in pts]

    def d2(a, b):
        return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2

    # Seeds: the heaviest colour, then repeatedly the one farthest from the seeds
    # (weighted), so rare hues such as green and blue get an entry.
    cent = [pts[max(range(len(pts)), key=lambda i: wts[i])]]
    near = [d2(p, cent[0]) for p in pts]
    while len(cent) < min(colors, len(pts)):
        i = max(range(len(pts)), key=lambda i: near[i] * wts[i])
        cent.append(pts[i])
        near = [min(near[j], d2(pts[j], pts[i])) for j in range(len(pts))]
    for _ in range(24):
        sums = [[0.0, 0.0, 0.0, 0.0] for _ in cent]
        for p, w in zip(pts, wts):
            k = min(range(len(cent)), key=lambda k: d2(p, cent[k]))
            sm = sums[k]
            sm[0] += p[0] * w; sm[1] += p[1] * w; sm[2] += p[2] * w; sm[3] += w
        new = [(round(sm[0] / sm[3]), round(sm[1] / sm[3]), round(sm[2] / sm[3])) if sm[3] else c
               for sm, c in zip(sums, cent)]
        if new == cent:
            break
        cent = new
    return cent


def nearest(pal, rgb):
    r, g, b = rgb
    best, bi = None, 0
    for i, (pr, pg, pb) in enumerate(pal):
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi + 1                                   # 0 is transparent


def indexed(im, pal):
    """n x n list of rows of palette indices (0 transparent, 1..len(pal))."""
    px = im.load()
    n = im.size[0]
    cache = {}
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            r, g, b, a = px[x, y]
            if not a:
                row.append(0)
                continue
            k = (r, g, b)
            if k not in cache:
                cache[k] = nearest(pal, k)
            row.append(cache[k])
        rows.append(row)
    return rows


def bitmap_bytes(rows):
    """Rows of indices (0..15) -> run-length bytes: (run - 1) << 4 | pixel, runs of
    1..16 that cross row ends. See ui/gfx/font.h."""
    flat = [v for r in rows for v in r]
    out, i = [], 0
    while i < len(flat):
        n = 1
        while i + n < len(flat) and flat[i + n] == flat[i] and n < 16:
            n += 1
        out.append(((n - 1) << 4) | flat[i])
        i += n
    return out


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def generate(emoji, font_path, out):
    """Write `out` (C source) for the emoji settings from unicode_profiles.
    Returns report lines."""
    import emoji_set
    ef = EmojiFont(font_path)
    sizes = emoji["sizes"]
    natives = sorted({px for v in sizes.values() if v for px in [v[0]]})
    per = sum((px * px + 1) // 2 for px in natives)       # raw bytes a glyph would cost at every native size
    wanted = emoji_set.tokens(emoji["tiers"])
    if "all" in emoji["tiers"]:
        # Everything else the font draws: real emoji only (not its letters, digits, spaces,
        # private-use codes or joiners alone), shortest sequences first.
        seen = set(wanted)
        rest = [k for k in ef.seq if k not in seen and ef.seq[k] in ef.strike and not (
            len(k) == 1 and (k[0] < 0x80 and chr(k[0]) not in "#*0123456789" or 0xE000 <= k[0] <= 0xF8FF
                              or k[0] in (0xA0, 0x200D, 0xFE0F, 0x20E3)))]
        wanted += sorted(rest, key=lambda k: (len(k), k))
    toks = [t for t in wanted if ef.glyph(t) and ef.glyph(t) in ef.strike]
    dropped = len(wanted) - len(toks)
    if emoji["bytes"]:
        toks = toks[:emoji["bytes"] // per]
    # One bitmap per distinct glyph: sequences that share art (a plain and a
    # selector form, or aliases) point at the same one.
    glyph_names, glyph_of = [], []
    for t in toks:
        gname = ef.glyph(t)
        if gname not in glyph_names:
            glyph_names.append(gname)
        glyph_of.append(glyph_names.index(gname))
    pngs = [ef.png(n) for n in glyph_names]

    lines = [f"emoji: {len(toks)} sequences, {len(glyph_names)} distinct images, from tiers {','.join(emoji['tiers'])}"
             f"{' (cap %d bytes of bitmaps)' % emoji['bytes'] if emoji['bytes'] else ''}"
             f" (Noto Color Emoji {ef.version.split(';')[0]})"]
    if dropped:
        lines.append(f"  not in Noto Color Emoji: {dropped} sequences")
    total = 0
    body = ['/* Generated by scripts/gen_emoji.py from Noto Color Emoji; do not edit. */',
            '#include "ui/gfx/font.h"', ""]
    order = sorted(range(len(toks)), key=lambda i: toks[i])
    cps, tab = [], []
    for i in order:
        tab.append((len(cps), len(toks[i]), glyph_of[i]))
        cps.extend(toks[i])
    if len(cps) > 0xFFFF or len(tab) > 0xFFFF or len(pngs) > 0xFFFF:
        raise SystemExit("gen_emoji: more emoji than the 16-bit tables in font.h reach")
    body.append("static const uint32_t emoji_cps[] = {")
    for i in range(0, len(cps), 10):
        body.append("    " + " ".join(f"0x{c:x}," for c in cps[i:i + 10]))
    body.append("};")
    body.append("static const mc_seq_t emoji_tab[] = {")
    for i in range(0, len(tab), 6):
        body.append("    " + " ".join("{%d,%d,%d}," % t for t in tab[i:i + 6]))
    body.append("};")
    body.append(f"static const mc_emoji_seqs_t emoji_seqs = {{{len(tab)}, {max(len(t) for t in toks)}, "
                f"emoji_tab, emoji_cps}};")
    body.append("")
    for px in natives:
        ims = [scaled(p, px) for p in pngs]
        pal = shared_palette(ims)
        bits, offs = [], []
        for im in ims:
            offs.append(len(bits))
            bits.extend(bitmap_bytes(indexed(im, pal)))
        offs.append(len(bits))
        total += len(bits) + 4 * len(offs) + 32
        body.append(f"static const uint16_t emoji_pal_{px}[16] = {{0, "
                    + ", ".join("0x%04x" % rgb565(c) for c in pal) + "};")
        body.append(f"static const uint8_t emoji_bits_{px}[] = {{")
        for i in range(0, len(bits), 24):
            body.append("    " + ",".join("0x%02x" % b for b in bits[i:i + 24]) + ",")
        body.append("};")
        body.append(f"static const uint32_t emoji_offs_{px}[] = {{")
        for i in range(0, len(offs), 16):
            body.append("    " + ",".join(str(o) for o in offs[i:i + 16]) + ",")
        body.append("};")
        lines.append(f"  {px}x{px}: {len(bits)} bytes of run-length bitmaps (+{4 * len(offs)} of offsets), "
                     f"{len(pal)}-colour palette")
    body.append("")
    for sym, v in sizes.items():
        if v:
            px, sc = v
            body.append(f"const mc_emoji_t {sym}_emoji = {{{px}, {px}, {sc}, emoji_pal_{px}, "
                        f"emoji_bits_{px}, emoji_offs_{px}, &emoji_seqs}};")
        else:
            body.append(f"const mc_emoji_t {sym}_emoji = {{0, 0, 1, 0, 0, 0, &emoji_seqs}};")
    with open(out, "w") as f:
        f.write("\n".join(body) + "\n")
    total += len(cps) * 4 + len(tab) * 6 + 16
    lines.append(f"  emoji layers total: {total} bytes of flash")
    return lines, total
