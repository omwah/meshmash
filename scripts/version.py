#!/usr/bin/env python3
"""The firmware's version label, from git.

On a tagged commit with a clean tree it is the tag alone ("1.2.0"). Anywhere else it says how
far from the tag it is, and whether the tree has uncommitted changes:

    1.2.0+19.9bf8937          19 commits after tag 1.2.0, at commit 9bf8937
    1.2.0+19.9bf8937.dirty    the same with uncommitted changes to tracked files
    1.2.0+0.89ecde1.dirty     on the tag, with changes

Usage:
    python -I scripts/version.py                  print the label
    python -I scripts/version.py --header PATH    write PATH (#define MC_UI_VERSION "...") if it changed
    python -I scripts/version.py --self-test
Without git (a source tarball) the label is the `version_fallback` in platformio.ini.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def label(describe, fallback):
    """The label for `git describe --tags --long --dirty --always` output."""
    d = describe.strip()
    m = re.fullmatch(r"(?P<tag>.+)-(?P<n>\d+)-g(?P<sha>[0-9a-f]+)(?P<dirty>-dirty)?", d)
    if m:
        tag = m["tag"][1:] if re.match(r"v\d", m["tag"]) else m["tag"]
        if m["n"] == "0" and not m["dirty"]:
            return tag
        return f"{tag}+{int(m['n'])}.{m['sha']}" + (".dirty" if m["dirty"] else "")
    # No tag in reach: --always gives a bare commit hash.
    dirty = d.endswith("-dirty")
    sha = d[:-6] if dirty else d
    if not re.fullmatch(r"[0-9a-f]{4,40}", sha):
        return fallback
    return f"{fallback}+{sha}" + (".dirty" if dirty else "")


def fallback_version():
    try:
        with open(os.path.join(ROOT, "platformio.ini")) as f:
            m = re.search(r"(?m)^version_fallback\s*=\s*(\S+)", f.read())
        return m.group(1) if m else "0.0.0"
    except OSError:
        return "0.0.0"


def version():
    fb = fallback_version()
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--long", "--dirty", "--always", "--match", "[0-9]*", "--match", "v[0-9]*"],
            cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return fb
    return label(out, fb)


def write_header(path, v):
    text = '#pragma once\n// Written by scripts/version.py at build time.\n#define MC_UI_VERSION "%s"\n' % v
    try:
        with open(path) as f:
            if f.read() == text:
                return   # unchanged: nothing rebuilds
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def self_test():
    cases = [
        ("1.2.0-0-g89ecde1\n", "1.2.0"),
        ("1.2.0-0-g89ecde1-dirty\n", "1.2.0+0.89ecde1.dirty"),
        ("1.2.0-19-g9bf8937\n", "1.2.0+19.9bf8937"),
        ("1.2.0-19-g9bf8937-dirty\n", "1.2.0+19.9bf8937.dirty"),
        ("v2.0.1-3-gabcdef0\n", "2.0.1+3.abcdef0"),
        ("v2.0.1-0-gabcdef0\n", "2.0.1"),
        ("1.2.0-rc1-4-g1234567\n", "1.2.0-rc1+4.1234567"),
        ("9bf8937\n", "0.9.0+9bf8937"),
        ("9bf8937-dirty\n", "0.9.0+9bf8937.dirty"),
        ("", "0.9.0"),
    ]
    bad = 0
    for given, want in cases:
        got = label(given, "0.9.0")
        if got != want:
            print("FAIL %r: %r, wanted %r" % (given, got, want))
            bad += 1
    if bad:
        sys.exit(1)
    print("version: %d cases ok" % len(cases))


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
    elif "--header" in sys.argv:
        write_header(sys.argv[sys.argv.index("--header") + 1], version())
    else:
        print(version())
