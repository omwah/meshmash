#!/usr/bin/env python3
"""Turn a device message backup into a corpus that is safe to commit.

Reads the ring file and the SD archive files of a MeshMash backup (209-byte records, the layout
before routes got a field of their own) and writes tab-separated lines for the thread tests and the
simulator:

    <channel> <seq> <offset s> <sender> <text, newlines as \\n>

Nothing from the source survives except its structure:
  * senders become stable pseudonyms (the same person is always the same pseudonym), and so do the
    names inside @[...] mentions;
  * every word becomes a deterministic made-up word of the same length and case, so topics still
    overlap between messages but say nothing; digits are replaced too; URLs and anything outside
    ASCII (emoji, decorations) are dropped;
  * times become gaps from the previous message (kept when plausible, else 45 s), summed into
    offsets from the start of the channel, so nothing real survives but the rhythm.
The script then checks its own output: no source sender or word of four letters or more may appear
verbatim, or it exits with an error and writes nothing.

Usage: python -I scripts/anonymise-corpus.py --ring RING.bin --history DIR --out OUT.txt
"""
import argparse
import hashlib
import os
import re
import struct
import sys

REC = 209
NAMES = """Alder Birch Cedar Dune Ember Fern Grove Heath Iris Juniper Kestrel Larch Moss Nettle Otter Pine
Quill Rowan Sage Thistle Umber Vale Willow Yarrow Zephyr Aspen Basil Clover Dahlia Elm Flint Gale Hazel
Ivy Jasper Kelp Linden Maple Nimbus Opal Poppy Quartz Reed Sorrel Tansy Urchin Violet Wren Xenia Yew Zinnia""".split()
ONSET = ["b", "d", "f", "g", "k", "l", "m", "n", "p", "r", "s", "t", "v", "z", "br", "dr", "fl", "gr", "pl", "st", "tr"]
VOWEL = ["a", "e", "i", "o", "u", "ai", "ea", "ou"]


def records(data):
    for i in range(0, len(data) - REC + 1, REC):
        r = data[i:i + REC]
        seq = int.from_bytes(r[0:4], "little")
        if not seq:
            continue
        yield {
            "seq": seq,
            "kind": r[4],
            "chan": r[5],
            "flags": r[11],
            "ts": int.from_bytes(r[17:21], "little"),
            "sender": r[25:49].split(b"\0")[0].decode("utf8", "replace"),
            "text": r[49:209].split(b"\0")[0].decode("utf8", "replace"),
        }


def norm(name):
    """A name as a key: ASCII letters and digits only, lower case."""
    return re.sub(r"[^a-z0-9]", "", name.encode("ascii", "ignore").decode().lower())


class Pseudo:
    def __init__(self, source_words, source_names):
        self.source = source_words
        # Pseudonyms that are real words or names in the source would look like leaks.
        self.pool = [n for n in NAMES if n.lower() not in source_words and n.lower() not in source_names]
        self.words = {}
        self.used = set()
        self.names = {}

    def name(self, raw):
        k = norm(raw)
        if not k:
            return None
        if k not in self.names:
            n = len(self.names)
            base = self.pool[n % len(self.pool)]
            self.names[k] = base if n < len(self.pool) else "%s%d" % (base, n // len(self.pool) + 1)
        return self.names[k]

    def word(self, w):
        key = w.lower()
        if key not in self.words:
            for salt in range(1000):
                h = hashlib.sha256(("%s/%d" % (key, salt)).encode()).digest()
                out, i = "", 0
                while len(out) < len(key):
                    out += ONSET[h[i % 32] % len(ONSET)] + VOWEL[h[(i + 1) % 32] % len(VOWEL)]
                    i += 2
                out = out[:len(key)]
                if out not in self.source and out not in self.used and out != key:
                    break
            self.used.add(out)
            self.words[key] = out
        out = self.words[key]
        return out.upper() if w.isupper() and len(w) > 1 else out.capitalize() if w[0].isupper() else out

    def number(self, d):
        h = hashlib.sha256(("n/" + d).encode()).digest()
        return "".join(str(h[i % 32] % 10) for i in range(len(d)))


def scramble(text, ps):
    """The text with its words, numbers and mention names replaced."""
    text = re.sub(r"https?://\S+", "", text)
    text = text.encode("ascii", "ignore").decode()
    out = []
    pos = 0
    for m in re.finditer(r"@\[([^\]]*)\]", text):
        out.append(_plain(text[pos:m.start()], ps))
        n = ps.name(m.group(1))
        out.append("@[%s]" % n if n else "")
        pos = m.end()
    out.append(_plain(text[pos:], ps))
    return "".join(out).replace("\r", "").replace("\n", "\\n").replace("\t", " ").strip()


def _plain(s, ps):
    def sub(m):
        t = m.group(0)
        return ps.number(t) if t.isdigit() else ps.word(t)
    return re.sub(r"[A-Za-z]+|\d+", sub, s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ring", required=True)
    ap.add_argument("--history", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    recs = {}
    with open(a.ring, "rb") as f:
        for r in records(f.read()):
            recs[r["seq"]] = r
    for name in sorted(os.listdir(a.history)):
        if name.endswith(".bin"):
            with open(os.path.join(a.history, name), "rb") as f:
                for r in records(f.read()):
                    recs.setdefault(r["seq"], r)
    chans = {}
    for r in recs.values():
        if r["kind"] == 1:
            chans.setdefault(r["chan"], []).append(r)
    chans = {c: sorted(v, key=lambda r: r["seq"]) for c, v in chans.items() if len(v) >= 20}

    source_words = set()
    source_names = set()
    for v in chans.values():
        for r in v:
            source_words.update(w.lower() for w in re.findall(r"[A-Za-z]{4,}", r["text"]))
            source_names.add(norm(r["sender"]))
            for m in re.findall(r"@\[([^\]]*)\]", r["text"]):
                source_names.add(norm(m))
    ps = Pseudo(source_words, source_names)
    lines = []
    for c in sorted(chans):
        last = 0
        prev_ts = None
        for r in chans[c]:
            if r["flags"] & 1:
                who = "me"
            else:
                who = ps.name(r["sender"]) or "Anon"
            # The gap to the previous message, kept when it is plausible (the stored times mix this
            # device's clock with senders' clocks, which are often wrong).
            d = r["ts"] - prev_ts if prev_ts is not None else 0
            if d < 0 or d > 7200:
                d = 45
            prev_ts = r["ts"]
            last += max(d, 1)
            lines.append("%d\t%d\t%d\t%s\t%s" % (c, r["seq"], last, who, scramble(r["text"], ps)))
    out = "\n".join(lines) + "\n"

    # The check: nothing of the source may be left.
    out_words = set(w.lower() for w in re.findall(r"[A-Za-z]{4,}", out))
    leaked = sorted((out_words & source_words) - {"self"})
    name_leaks = sorted(n for n in source_names if len(n) >= 4 and n in {norm(x) for x in re.findall(r"[A-Za-z0-9]+", out)})
    if leaked or name_leaks:
        sys.exit("refusing to write: source words left in the output: %s %s" % (leaked[:10], name_leaks[:10]))
    with open(a.out, "w") as f:
        f.write(out)
    print("%d messages, %d channels, %d people, %d distinct words" % (len(lines), len(chans), len(ps.names), len(ps.words)))


if __name__ == "__main__":
    main()
