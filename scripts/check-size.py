#!/usr/bin/env python3
"""
Check the firmware images against size budgets (flash = app image, RAM =
static .data + .bss). Fails when a build grows past its budget, so growth is
always a deliberate decision. Run after `pixi run build`.
"""

import os
import subprocess
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-size")

# env: (max app bytes, max static RAM bytes)
BUDGETS = {
    # Bluetooth over NimBLE (2026-10-11): 1296 -> 915 KB and 127 -> 113 KB
    # static RAM against stock's Bluedroid. Room left for BLE client mode.
    # Colour emoji and Unicode layers (2026-10-23): the `min` profile's 103
    # run-length coded emoji, its text layers and the sprite code are ~21 KB,
    # and 48 on-screen sprites ~1 KB of RAM. Flash caps rose by 25 KB for it;
    # see docs/unicode.md.
    # +2 KB (2026-10-25): the 12/24-hour clock, receive times and sender offsets, the version label.
    # +3 KB (2026-10-25): the thread graph (scoring, components, weak links) is shared code.
    "cardputer_adv": (1_030_000, 120_000),       # stock BLE companion is ~1.25 MB
    # +2 KB (2026-10-25): chat drafts, the message info and thread screens, history reads.
    "cardputer_adv_usb": (771_000, 109_000),     # MeshCore 1.18 adds dynamic prefs/CLI (~7 KB over old cap)
    # T-Deck: a 6.25 MB app partition, so flash is no concern; internal RAM is
    # (the node and the UI are made in PSRAM at boot). Its `std` profile carries the
    # whole Noto emoji set (5,644 sequences, 4,003 images, three sizes: ~1.1 MB coded),
    # so the caps rose by ~1.05 MB; the app partition is 6.25 MB.
    "tdeck": (2_250_000, 80_000),
    "tdeck_usb": (2_000_000, 70_000),
}


def sizes(env):
    elf = os.path.join(ROOT, ".pio", "build", env, "firmware.elf")
    out = subprocess.check_output([TOOL, "-A", elf], text=True)
    sec = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("."):
            sec[parts[0]] = int(parts[1])
    ram = sum(v for k, v in sec.items() if k in (".dram0.data", ".dram0.bss", ".noinit"))
    flash = os.path.getsize(os.path.join(ROOT, ".pio", "build", env, "firmware.bin"))
    return flash, ram


def configured_profile(env):
    """The Unicode profile the environment should have been built with: the
    MC_UNICODE override, else custom_unicode from platformio.ini (following
    `extends`), normalized the way gen_fonts.py does."""
    import configparser
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import unicode_profiles
    spec = os.environ.get("MC_UNICODE", "").strip()
    if not spec:
        ini = configparser.ConfigParser(interpolation=None, strict=False, inline_comment_prefixes=(";",))
        ini.read(os.path.join(ROOT, "platformio.ini"))
        name = "env:" + env
        while name and not spec:
            sec = name if ini.has_section(name) else "env:" + name if ini.has_section("env:" + name) else None
            if sec is None:
                break
            spec = ini.get(sec, "custom_unicode", fallback="").strip()
            name = ini.get(sec, "extends", fallback="").strip()
    return unicode_profiles.resolve(spec)[0]


def built_profile(env):
    """The profile recorded in the firmware (the About text), or None."""
    with open(os.path.join(ROOT, ".pio", "build", env, "firmware.bin"), "rb") as f:
        m = re.search(rb"Unicode: ([^\n\0]+)", f.read())
    return m.group(1).decode() if m else None


def main():
    bad = 0
    for env, (max_flash, max_ram) in BUDGETS.items():
        try:
            flash, ram = sizes(env)
        except (OSError, subprocess.CalledProcessError) as e:
            print(f"{env}: not built ({e})")
            bad += 1
            continue
        ok = flash <= max_flash and ram <= max_ram
        want, got = configured_profile(env), built_profile(env)
        if want != got:                          # a firmware from another profile proves nothing
            print(f"WRONG {env:20} built with Unicode profile {got!r}, configured {want!r}")
            bad += 1
        bad += not ok
        print(f"{'ok  ' if ok else 'OVER'} {env:20} flash {flash/1024:7.1f} KB / {max_flash/1024:.0f}"
              f"   static RAM {ram/1024:6.1f} KB / {max_ram/1024:.0f}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
