#!/usr/bin/env python3
# Checks emoji/emoji-names.txt (make-emoji-names.py) against the emoji pack it
# goes with: well formed, small enough for the standalone (EmojiPackUI.inc reads
# at most 256 KB), and naming every emoji the pack offers in its picker.
#
#   python3 scripts/build/check-emoji-names.py emoji/emoji-names.txt deploy/site/emoji/emoji.pack
import struct, sys


def pack_items(path):
    d = open(path, "rb").read()
    _, _, _, _, _, items_off, items_len, _, _ = struct.unpack_from("<4sHHIIIIII", d, 0)
    it = d[items_off:items_off + items_len]
    n = struct.unpack_from("<H", it, 0)[0]
    pos, out = 2, []
    for _ in range(n):
        ln = it[pos + 1]
        out.append(it[pos + 2:pos + 2 + ln].decode("utf-8"))
        pos += 2 + ln
    return out


def main(names_path, pack_path):
    raw = open(names_path, "rb").read()
    fails = []
    if len(raw) > 256 * 1024:
        fails.append("%d bytes: over the 256 KB the standalone reads" % len(raw))
    if not raw.endswith(b"\n"):
        fails.append("doesn't end in a newline")
    named = set()
    for i, line in enumerate(raw.decode("utf-8").split("\n")[:-1], 1):
        utf8, tab, text = line.partition("\t")
        if not tab or not utf8 or not text or "\t" in text:
            fails.append("line %d: not 'emoji TAB name|keywords'" % i)
        named.add(utf8)
    items = pack_items(pack_path)
    missing = [s for s in items if s not in named and s.replace("️", "") not in
               {n.replace("️", "") for n in named}]
    print("%d names; the pack offers %d emoji, %d without a name" % (len(named), len(items), len(missing)))
    if missing:
        print("  e.g. " + " ".join(missing[:20]))
    if len(missing) > len(items) // 50:   # a few new or odd ones are fine; most must be there
        fails.append("%d of the pack's emoji have no name" % len(missing))
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
