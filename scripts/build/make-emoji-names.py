#!/usr/bin/env python3
# Builds emoji/emoji-names.txt: the names the emoji picker's search (🔍) finds
# emoji by. Upstream's emoji pack (deploy/site/emoji/emoji.pack, gen-emoji-pack.py)
# carries only images, so the names travel beside it: kept, copied from the SD
# card or downloaded with the pack (EmojiPackUI.inc, EMOJI.md).
#
#   curl -LO https://raw.githubusercontent.com/unicode-org/unicodetools/main/unicodetools/data/emoji/15.1/emoji-test.txt
#   curl -L -o annotations.json https://raw.githubusercontent.com/unicode-org/cldr-json/main/cldr-json/cldr-annotations-full/annotations/en/annotations.json
#   curl -L -o derived.json https://raw.githubusercontent.com/unicode-org/cldr-json/main/cldr-json/cldr-annotations-derived-full/annotationsDerived/en/annotations.json
#   python3 scripts/build/make-emoji-names.py emoji-test.txt annotations.json derived.json
#
# One line per emoji, in Unicode's order:
#   <the emoji to insert, UTF-8>  TAB  <name>|<CLDR English keywords>  LF
# e.g. "🐠\ttropical fish|animal fish fishes tropical". The text to insert is
# Unicode's fully-qualified form (U+2764 U+FE0F for ❤️). A flag's keywords also
# hold its ISO code. Skin-tone variants are left out, as the pack leaves them out;
# the standalone shows only the ones it can draw.
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "emoji", "emoji-names.txt")
TONES = range(0x1F3FB, 0x1F400)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: make-emoji-names.py <emoji-test.txt> <annotations.json> <derived annotations.json>")
    ann = json.load(open(sys.argv[2], encoding="utf-8"))["annotations"]["annotations"]
    der = json.load(open(sys.argv[3], encoding="utf-8"))["annotationsDerived"]["annotations"]
    lines = []
    for line in open(sys.argv[1], encoding="utf-8"):
        m = re.match(r"([0-9A-F ]+?)\s*;\s*fully-qualified\s*#\s*\S+\s+E[\d.]+\s+(.*)$", line)
        if not m:
            continue
        cps = [int(x, 16) for x in m.group(1).split()]
        if any(c in TONES for c in cps):
            continue
        name = m.group(2).strip().lower()
        extra = []
        if len(cps) == 2 and all(0x1F1E6 <= c <= 0x1F1FF for c in cps):
            extra = ["".join(chr(97 + c - 0x1F1E6) for c in cps)]
        bare = "".join(chr(c) for c in cps if c != 0xFE0F)
        a = ann.get(bare) or der.get(bare) or ann.get("".join(map(chr, cps))) or {}
        kws = [k.lower() for k in a.get("default", []) if k.lower() != name] + extra
        text = (name + "|" + " ".join(kws)).replace("\t", " ").replace("\n", " ")
        lines.append("".join(chr(c) for c in cps) + "\t" + text)
    data = ("\n".join(lines) + "\n").encode("utf-8")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(data)
    print("emoji-names.txt: %d emoji, %d bytes" % (len(lines), len(data)))


if __name__ == "__main__":
    main()
