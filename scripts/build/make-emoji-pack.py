#!/usr/bin/env python3
# Builds emoji/emoji-pack.wep: every single-codepoint Noto colour emoji (1440 at
# v2.051), for the emoji the firmware doesn't carry in flash. The app slot is
# nearly full on most boards, so the full set lives as a file instead: on the
# internal store, copied from the SD card, or downloaded over Wi-Fi from the
# firmware's own release (EMOJI.md). emojiGlyphLookup() tries the baked emoji
# first and this pack after them.
#
#   git clone --depth 1 --branch v2.051 --filter=blob:none --sparse \
#       https://github.com/googlefonts/noto-emoji.git noto && \
#       git -C noto sparse-checkout set png/128 third_party/region-flags/png
#       git -C noto sparse-checkout set png/128 third_party/region-flags/png
#   python3 scripts/build/make-emoji-pack.py noto/png/128 noto/third_party/region-flags/png
#
# Flags: Noto's png/128 has none; its region-flags/png has one per ISO code (US.png).
# Each is stored under FlagText.h's key for its two regional indicators (past the
# last Unicode codepoint), which is the letter LVGL reads a flag as.
#
# Images are made exactly as add-emoji.py makes the baked ones (16 px, RGB565 +
# alpha) and packed as pack-emoji.py packs them (alpha plane, then the visible
# pixels' RGB565, each through the same LZ), so the firmware unpacks both alike.
#
# File (little-endian):
#   0   "WEP1"
#   4   u16 version (1)    6  u16 count
#   8   u32 data offset (from the start of the file)
#   12  count x 16-byte entries, sorted by codepoint:
#         u32 cp, u32 offset (from the data offset), u16 zlen, u16 alen, u8 w, u8 h, u16 0
#       w = h = 0: drawn as nothing (a skin-tone modifier, a keycap or tag
#       character): the emoji before it shows on its own instead of a box.
#   ... data
#
# Also writes test/emoji_pack_file_expected.h for test/test_emoji_pack_file.cpp.
import os, re, struct, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "emoji", "emoji-pack.wep")
OUTT = os.path.join(ROOT, "test", "emoji_pack_file_expected.h")
PX = 16

# Drawn as nothing: skin tones (the emoji shows in its default colour), the keycap
# combiner, the tag characters of subdivision flags.
ZERO = list(range(0x1F3FB, 0x1F400)) + [0x20E3] + list(range(0xE0020, 0xE0080))
# Checked by the test: a few the chat showed as boxes, and the edges.
FLAG_BASE = 0x110000   # FlagText.h key(a, b) = FLAG_BASE + a * 26 + b (letters 0..25)


def flag_key(iso):
    return FLAG_BASE + (ord(iso[0]) - 65) * 26 + (ord(iso[1]) - 65)


SAMPLES = [0x1F420, 0x1F41F, 0x2615, 0x1F947, 0x1F3C0, 0x1F3B5, 0x1F600, 0x1FAF6,
           flag_key("US"), flag_key("LS"), flag_key("CA"), flag_key("JP")]


def to_rgb565a8(img):
    # IDENTICAL to add-emoji.py / gen-emoji-font.py: autocrop the transparent
    # margin, scale into 16x16, bottom-align, [lo, hi, alpha] per pixel.
    img = img.convert("RGBA")
    bbox = img.split()[3].getbbox()
    if bbox:
        img = img.crop(bbox)
    w, h = img.size
    scale = min(PX / w, PX / h)
    nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
    content = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGBA", (PX, PX), (0, 0, 0, 0))
    canvas.paste(content, ((PX - nw) // 2, PX - nh))
    out = bytearray()
    for (r, g, b, a) in canvas.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        if a == 0:
            v = 0   # a fully transparent pixel's colour never shows (pack-emoji.py)
        out += bytes((v & 0xFF, (v >> 8) & 0xFF, a))
    return bytes(out)


# pack-emoji.py's LZ stream, found with a hash chain instead of its full window
# scan (1440 images): t < 0x80 is t+1 literals; t >= 0x80 copies (t&0x7F)+3 bytes
# from off = 1 + next two bytes (LE) back.
WINDOW, MAXRUN = 1024, 130


def lz(b):
    out, lit = bytearray(), bytearray()
    heads = {}
    i, n = 0, len(b)

    def flush():
        nonlocal lit
        while lit:
            k = lit[:128]
            out.append(len(k) - 1)
            out.extend(k)
            lit = lit[128:]

    def add(p):
        if p + 3 <= n:
            heads.setdefault(b[p:p + 3], []).append(p)

    while i < n:
        best = bo = 0
        if i + 3 <= n:
            for j in reversed(heads.get(b[i:i + 3], [])[-64:]):
                if i - j > WINDOW:
                    break
                l = 0
                while i + l < n and b[j + l] == b[i + l] and l < MAXRUN:
                    l += 1
                if l > best:
                    best, bo = l, i - j
        if best >= 4:
            flush()
            out.append(0x80 | (best - 3))
            out += bytes(((bo - 1) & 0xFF, (bo - 1) >> 8))
            for p in range(i, i + best):
                add(p)
            i += best
        else:
            lit.append(b[i])
            add(i)
            i += 1
    flush()
    return bytes(out)


def unlz(c, n):
    o, i = bytearray(), 0
    while i < len(c):
        t = c[i]
        i += 1
        if t < 0x80:
            o += c[i:i + t + 1]
            i += t + 1
        else:
            l, off = (t & 0x7F) + 3, (c[i] | (c[i + 1] << 8)) + 1
            i += 2
            for _ in range(l):
                o.append(o[-off])
    if len(o) != n:
        raise ValueError("bad stream")
    return bytes(o)


def pack(px):
    alpha = bytes(px[k + 2] for k in range(0, len(px), 3))
    rgb = bytearray()
    for k in range(0, len(px), 3):
        if px[k + 2]:
            rgb += px[k:k + 2]
    za, zr = lz(alpha), lz(bytes(rgb))
    return za + zr, len(za)


def unpack(z, alen, n):
    alpha = unlz(z[:alen], n)
    rgb = unlz(z[alen:], 2 * sum(1 for a in alpha if a))
    out, r = bytearray(3 * n), 0
    for p in range(n):
        if alpha[p]:
            out[3 * p], out[3 * p + 1] = rgb[r], rgb[r + 1]
            r += 2
        out[3 * p + 2] = alpha[p]
    return bytes(out)


def fnv(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make-emoji-pack.py <noto-emoji/png/128> <noto-emoji/third_party/region-flags/png>")
    src, flags = sys.argv[1], sys.argv[2]
    imgs = {}
    for fn in sorted(os.listdir(src)):
        m = re.fullmatch(r"emoji_u([0-9a-f]+)\.png", fn)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if cp < 0x80 or cp in ZERO:   # ASCII is the text font's; ZERO draw as nothing
            continue
        imgs[cp] = os.path.join(src, fn)
    if len(imgs) < 1000:
        sys.exit("make-emoji-pack: only %d images in %s" % (len(imgs), src))
    nflags = 0
    for fn in sorted(os.listdir(flags)):
        m = re.fullmatch(r"([A-Z]{2})\.png", fn)
        if m:
            imgs[flag_key(m.group(1))] = os.path.join(flags, fn)
            nflags += 1
    if nflags < 200:
        sys.exit("make-emoji-pack: only %d flags in %s" % (nflags, flags))

    entries, data, hashes = [], bytearray(), {}
    for cp in sorted(set(imgs) | set(ZERO)):
        if cp in ZERO:
            entries.append((cp, 0, 0, 0, 0, 0))
            continue
        px = to_rgb565a8(Image.open(imgs[cp]))
        z, alen = pack(px)
        if unpack(z, alen, PX * PX) != px:
            sys.exit("make-emoji-pack: U+%X doesn't round-trip" % cp)
        if len(z) > 0xFFFF:
            sys.exit("make-emoji-pack: U+%X too big" % cp)
        entries.append((cp, len(data), len(z), alen, PX, PX))
        data += z
        hashes[cp] = fnv(px)

    head = struct.pack("<4sHHI", b"WEP1", 1, len(entries), 12 + 16 * len(entries))
    table = b"".join(struct.pack("<IIHHBBH", cp, off, zl, al, w, h, 0) for cp, off, zl, al, w, h in entries)
    blob = head + table + bytes(data)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)

    drawn = sum(1 for e in entries if e[4])
    with open(OUTT, "w") as f:
        f.write("// AUTO-GENERATED by scripts/build/make-emoji-pack.py — do not edit.\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("static const uint32_t kPackBytes = %du;\n" % len(blob))
        f.write("static const uint16_t kPackCount = %d;\n" % len(entries))
        f.write("static const uint16_t kPackDrawn = %d;\n" % drawn)
        f.write("static const struct { uint32_t cp, fnv; } kPackSamples[] = {\n")
        for cp in SAMPLES:
            f.write("  { 0x%Xu, 0x%08Xu },\n" % (cp, hashes[cp]))
        f.write("};\n")
        f.write("static const uint16_t kPackFlags = %d;\n" % nflags)
    print("emoji-pack.wep: %d entries (%d images, %d flags), %d bytes" % (len(entries), drawn, nflags, len(blob)))


if __name__ == "__main__":
    main()
