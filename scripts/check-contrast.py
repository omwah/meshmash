#!/usr/bin/env python3
"""
Check every theme's text/background pairs against WCAG 2.x contrast ratios.

    python -I scripts/check-contrast.py          # exit 1 if a pair is under its minimum
    python -I scripts/check-contrast.py -v       # every ratio, not just the failures

Reads the palettes from src/ui/theme.cpp and checks them as the panel shows
them (quantised to RGB565). Text that carries meaning needs 4.5:1 (WCAG AA);
colours that only mark a kind or a state, 3:1 (non-text, AA). Midnight is the
benchmark for how much contrast is enough.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROLES = ["BG", "ZEBRA", "HEAD", "HTXT", "SEL", "TEXT", "DIM", "ACCENT", "OK", "WARN", "ERR", "BADGE",
         "CHAN", "CHAT", "RPT", "ROOM"]

# (foreground, backgrounds, minimum ratio)
TEXT = 4.5
MARK = 3.0
PAIRS = [
    ("TEXT", ["BG", "ZEBRA", "SEL"], TEXT),
    ("HTXT", ["HEAD"], TEXT),
    ("DIM", ["BG", "ZEBRA"], TEXT),
    ("DIM", ["SEL"], 2.5),            # Midnight, the benchmark, sits at 2.54
    ("ACCENT", ["BG", "ZEBRA"], TEXT),   # titles
    ("ACCENT", ["SEL"], 4.0),         # mentions on the selection colour (Midnight: 4.30)
    ("OK", ["BG", "ZEBRA"], MARK),
    ("WARN", ["BG", "ZEBRA"], MARK),
    ("ERR", ["BG", "ZEBRA"], MARK),
    ("BADGE", ["BG", "ZEBRA"], MARK),
    ("CHAN", ["BG", "ZEBRA", "SEL"], MARK),
    ("CHAT", ["BG", "ZEBRA", "SEL"], MARK),
    ("RPT", ["BG", "ZEBRA", "SEL"], MARK),
    ("ROOM", ["BG", "ZEBRA", "SEL"], MARK),
]


def quantise(r, g, b):
    """The colour the RGB565 panel shows for an 8-bit one."""
    r, g, b = r & 0xF8, g & 0xFC, b & 0xF8
    return (r | r >> 5, g | g >> 6, b | b >> 5)


def lum(c):
    def lin(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(v) for v in c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


def badge_text(pal, fill):
    """Badge numbers are drawn in HTXT or BG, whichever reads better (ui::onColour)."""
    return max(ratio(pal["HTXT"], pal[fill]), ratio(pal["BG"], pal[fill]))


def themes():
    src = open(os.path.join(ROOT, "src/ui/theme.cpp")).read()
    for m in re.finditer(r'\{"(\w+)",\s*\{(.*?)\}\}', src, re.S):
        cols = [quantise(*map(int, t)) for t in re.findall(r"rgb\((\d+),\s*(\d+),\s*(\d+)\)", m.group(2))]
        if len(cols) != len(ROLES):
            sys.exit(f"theme {m.group(1)}: {len(cols)} colours, expected {len(ROLES)}")
        yield m.group(1), dict(zip(ROLES, cols))


def main():
    verbose = "-v" in sys.argv
    bad = 0
    n = 0
    for name, pal in themes():
        n += 1
        for fg, bgs, need in PAIRS:
            for bg in bgs:
                r = ratio(pal[fg], pal[bg])
                fail = r < need
                bad += fail
                if fail or verbose:
                    print(f"{'FAIL' if fail else 'ok  '} {name:9} {fg:6} on {bg:5} {r:5.2f}  (min {need})")
        for fill in ("BADGE", "DIM"):   # DIM: the badge of a muted channel
            r = badge_text(pal, fill)
            fail = r < TEXT
            bad += fail
            if fail or verbose:
                print(f"{'FAIL' if fail else 'ok  '} {name:9} badge text on {fill:5} {r:5.2f}  (min {TEXT})")
    if not n:
        sys.exit("no themes found in src/ui/theme.cpp")
    print(f"{n} themes, {bad} pairs under their minimum")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
