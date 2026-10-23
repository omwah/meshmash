"""Which Unicode coverage a build carries, chosen at build time.

A *profile* names, for each of the four Spleen sizes, the groups of code points
to add on top of Spleen from the fallback layers (Noto Sans Mono now; emoji
and more later). The firmware environment picks one with `custom_unicode` in
platformio.ini; `MC_UNICODE` in the environment overrides that, and a bare
`pixi run fonts` uses DEFAULT.

Spec syntax (the value of MC_UNICODE / custom_unicode):

    min                     a profile
    std,+math               a profile, plus a group at every size
    min,-greek              ... or minus one
    min,+0x2600-0x26ff      ... plus a raw code point range (also U+2600-U+26FF)
    min,+emoji-common       ... plus an emoji tier (scripts/emoji_set.py)
    std,-emoji              ... or without any emoji
    none                    Spleen only, exactly as before the layers existed

Policy: emoji coverage comes first and rarely used writing systems and symbols
are left out, most of all on the Cardputer, whose flash is tight. Add to a
profile here (or with a `+` term) rather than changing the renderer; the
generator only emits code points the source font really has and Spleen lacks,
and prints what each layer costs.
"""
import re

DEFAULT = "std"

# Named sets of code points (inclusive ranges).
GROUPS = {
    # European names beyond Latin-1 (Polish, Czech, Turkish, Hungarian, ...).
    "latin-ext-a": "0x100-0x17f",
    # Modern Greek with tonos, and the Cyrillic letters Spleen's small sizes
    # lack plus the Ukrainian ones.
    "greek": "0x386,0x388-0x38a,0x38c,0x38e-0x3a1,0x3a3-0x3ce",
    "cyrillic": "0x400-0x45f,0x490-0x491",
    # Typographic punctuation, dashes and quotes people paste in.
    "punct": "0x2010-0x2015,0x2018-0x201e,0x2020-0x2022,0x2026,0x2030,0x2039-0x203a,0x2116,0x2122",
    "currency": "0x20a9,0x20ac,0x20b9,0x20ba,0x20bd",
    "arrows": "0x2190-0x2195",
    # The handful of math signs that turn up in chat.
    "math": "0x2212,0x2248,0x2260,0x2264-0x2265,0x221e",
}

# Per Spleen font symbol: the groups its layer carries.
# (font_xlarge costs 48 bytes a glyph, so it gets far less than the others.)
_MIN = {
    # 5x8 only shows sender names at the smallest text size: just European names.
    "font_small": ["latin-ext-a", "punct", "currency"],
    "font_medium": ["latin-ext-a", "greek", "punct", "currency", "arrows"],
    "font_large": ["greek", "punct", "currency", "arrows"],
    "font_xlarge": ["punct"],
}
_STD = {
    "font_small": ["latin-ext-a", "cyrillic", "punct", "currency"],
    "font_medium": ["latin-ext-a", "greek", "punct", "currency", "arrows", "math"],
    "font_large": ["greek", "punct", "currency", "arrows", "math"],
    "font_xlarge": ["latin-ext-a", "greek", "cyrillic", "punct", "currency", "arrows"],
}

# Colour emoji per profile. `sizes` maps each Spleen font to the bitmap size its
# emoji are drawn from and the integer scale they are drawn at, or None for a
# size too small to show them (a matched emoji then takes one replacement
# glyph's place). `bytes` caps the bitmaps of all native sizes together, counted
# before run-length coding (the emoji of a tier are taken in order until it is
# spent); None for no cap. Coding roughly halves what is stored.
# The 24-row font reuses the 12 px art at 2x on the Cardputer: its native art
# would cost more than the other sizes together.
_SIZES_SCALED = {"font_small": None, "font_medium": (12, 1), "font_large": (16, 1), "font_xlarge": (12, 2)}
_SIZES_NATIVE = {"font_small": None, "font_medium": (12, 1), "font_large": (16, 1), "font_xlarge": (24, 1)}
EMOJI = {
    "none": None,
    "min": {"tiers": ["core"], "sizes": _SIZES_SCALED, "bytes": None},
    "std": {"tiers": ["core", "common", "all"], "sizes": _SIZES_NATIVE, "bytes": None},
}

PROFILES = {
    "none": {s: [] for s in _MIN},
    "min": _MIN,
    "std": _STD,
}

_RANGE = re.compile(r"^(?:0x|U\+)([0-9a-fA-F]+)(?:-(?:0x|U\+)?([0-9a-fA-F]+))?$")


def _group_ranges(name):
    """A group name or a raw range -> 'lo-hi,lo-hi' text, or None."""
    if name in GROUPS:
        return GROUPS[name]
    m = _RANGE.match(name)
    if m:
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        if hi < lo or hi > 0x10FFFF:
            raise ValueError(f"bad range {name!r}")
        return f"0x{lo:x}-0x{hi:x}"
    return None


def resolve(spec=""):
    """Spec -> (normalized spec, {font symbol: [group or range names]}, emoji
    settings or None)."""
    terms = [t.strip() for t in (spec or DEFAULT).split(",") if t.strip()]
    # A raw range contains no comma, so splitting on commas is safe, but a
    # profile name comes first and is optional.
    base = DEFAULT
    if terms and not terms[0][0] in "+-":
        base = terms.pop(0)
    if base not in PROFILES:
        raise ValueError(f"unknown Unicode profile {base!r}; have {', '.join(sorted(PROFILES))}")
    groups = {s: list(g) for s, g in PROFILES[base].items()}
    emoji = None
    if EMOJI[base]:
        emoji = {"tiers": list(EMOJI[base]["tiers"]), "sizes": dict(EMOJI[base]["sizes"]),
                 "bytes": EMOJI[base]["bytes"]}
    for t in terms:
        if t[0] not in "+-":
            raise ValueError(f"expected +group or -group, got {t!r}")
        name = t[1:]
        if name == "emoji" or name.startswith("emoji-"):
            import emoji_set
            tier = name[6:]
            if tier and tier not in emoji_set.TIERS:
                raise ValueError(f"unknown emoji tier {tier!r}; have {', '.join(sorted(emoji_set.TIERS))}")
            if t[0] == "-":
                if tier and emoji:
                    emoji["tiers"] = [x for x in emoji["tiers"] if x != tier]
                    if not emoji["tiers"]:
                        emoji = None
                else:
                    emoji = None
            else:
                if emoji is None:
                    emoji = {"tiers": [], "sizes": dict(EMOJI["std"]["sizes"]), "bytes": EMOJI["std"]["bytes"]}
                for x in ([tier] if tier else EMOJI["std"]["tiers"]):
                    if x not in emoji["tiers"]:
                        emoji["tiers"].append(x)
            continue
        if _group_ranges(name) is None:
            raise ValueError(f"unknown group {name!r}; have {', '.join(sorted(GROUPS))}")
        for g in groups.values():
            if t[0] == "+" and name not in g:
                g.append(name)
            elif t[0] == "-" and name in g:
                g.remove(name)
    return ",".join([base] + terms), groups, emoji


def ranges_for(names):
    """Group/range names -> a sorted list of inclusive (lo, hi) pairs."""
    out = []
    for n in names:
        for part in _group_ranges(n).split(","):
            lo, _, hi = part.partition("-")
            out.append((int(lo, 0), int(hi or lo, 0)))
    return sorted(out)
