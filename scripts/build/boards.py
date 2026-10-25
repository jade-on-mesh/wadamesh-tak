#!/usr/bin/env python3
"""The fork's released boards, from boards.json at the repo root.

  boards.py check               PR CI: boards.json is consistent, every env exists,
                                and flasher/index.html carries the current list
  boards.py flasher [FILE]      write the list into the flasher (default
                                flasher/index.html), between its BOARDS markers
  boards.py matrix              fork-release's plan step: GITHUB_OUTPUT lines
                                fw=<PlatformIO matrix>, idf=<P4 matrix>,
                                notes=<board list for the release notes>

Adding a finished board is one entry in boards.json: fork-release builds it, the
release notes name it and the MediaMesh Flasher offers it.
"""
import itertools
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
FLASHER = os.path.join(ROOT, "flasher", "index.html")
BEGIN, END = "/*BOARDS:BEGIN*/", "/*BOARDS:END*/"
FLASHER_KEYS = ("id", "name", "chip", "desc", "boot", "base", "options")


def load():
    with open(os.path.join(ROOT, "boards.json"), encoding="utf-8") as f:
        return json.load(f)["boards"]


def combos(board):
    """Every file name the flasher can ask for: base + each chosen option's suffix."""
    opts = board.get("options") or []
    for pick in itertools.product(*[o["choices"] for o in opts]):
        yield board["base"] + "".join(c[2] if len(c) > 2 else "" for c in pick)


def pio_envs():
    envs = set()
    for dirpath, _, files in os.walk(ROOT):
        if "/.pio" in dirpath or "/.git" in dirpath or "/emu/third_party" in dirpath:
            continue
        for fn in files:
            if fn == "platformio.ini":
                with open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace") as f:
                    envs.update(re.findall(r"^\[env:([^\]]+)\]", f.read(), re.M))
    return envs


def flasher_block(boards):
    lines = [json.dumps({k: b[k] for k in FLASHER_KEYS if k in b}, ensure_ascii=False) for b in boards]
    return BEGIN + "[\n    " + ",\n    ".join(lines) + ",\n  ]" + END


def problems(boards, html):
    out = []
    ids, slugs = set(), set()
    envs = pio_envs()
    for b in boards:
        where = "board %r" % b.get("id")
        for k in ("id", "name", "chip", "desc", "boot", "notes", "base", "builds"):
            if not b.get(k) and k != "options":
                out.append("%s: no %s" % (where, k))
        if b.get("id") in ids:
            out.append("%s: id used twice" % where)
        ids.add(b.get("id"))
        for o in b.get("options") or []:
            for c in o.get("choices", []):
                if len(c) != 3:
                    out.append("%s: option %r choice %r must be [value, label, suffix]" % (where, o.get("key"), c))
        builds = b.get("builds") or {}
        reachable = set(combos(b))
        for s in sorted(reachable - set(builds)):
            out.append("%s: the flasher can ask for %s, which is not in its builds" % (where, s))
        for s in sorted(set(builds) - reachable):
            out.append("%s: %s is built but no choice of options leads the flasher to it" % (where, s))
        for s, how in builds.items():
            if s in slugs:
                out.append("%s: %s is built by two boards" % (where, s))
            slugs.add(s)
            if not re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", s):
                out.append("%s: %r is not a file-name-safe slug" % (where, s))
            if ("env" in how) == ("idf" in how):
                out.append("%s: %s needs exactly one of env (PlatformIO) or idf (P4 switches)" % (where, s))
            elif "env" in how and how["env"] not in envs:
                out.append("%s: %s's env %s is in no platformio.ini" % (where, s, how["env"]))
            elif "idf" in how and not s.startswith("tdisplay-p4"):
                out.append("%s: %s: idf builds are only set up for the T-Display P4 (scripts/build/ci-p4.sh)" % (where, s))
    if html is not None:
        m = re.search(re.escape(BEGIN) + r".*?" + re.escape(END), html, re.S)
        if not m:
            out.append("flasher/index.html has no %s ... %s block" % (BEGIN, END))
        elif m.group(0) != flasher_block(boards):
            out.append("flasher/index.html's board list is out of date: run python3 scripts/build/boards.py flasher")
    return out


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "check"
    boards = load()
    if cmd == "check":
        with open(FLASHER, encoding="utf-8") as f:
            html = f.read()
        bad = problems(boards, html)
        for p in bad:
            print("::error::boards.json: %s" % p)
        if bad:
            return 1
        n = sum(len(b["builds"]) for b in boards)
        print("boards.json: %d boards, %d images, all reachable from the flasher" % (len(boards), n))
        return 0
    if cmd == "flasher":
        path = argv[2] if len(argv) > 2 else FLASHER
        bad = problems(boards, None)
        if bad:
            for p in bad:
                print("boards.json: %s" % p, file=sys.stderr)
            return 1
        with open(path, encoding="utf-8") as f:
            html = f.read()
        new, n = re.subn(re.escape(BEGIN) + r".*?" + re.escape(END), lambda _: flasher_block(boards), html, flags=re.S)
        if n != 1:
            print("%s: expected one %s ... %s block, found %d" % (path, BEGIN, END, n), file=sys.stderr)
            return 1
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
        return 0
    if cmd == "matrix":
        fw, idf = [], []
        for b in boards:
            for s, how in b["builds"].items():
                if "env" in how:
                    fw.append({"env": how["env"], "board": s})
                else:
                    idf.append({"board": s, "envs": how["idf"]})
        notes = ", ".join(b["notes"] for b in boards)
        print("fw=" + json.dumps({"include": fw}))
        print("idf=" + json.dumps({"include": idf}))
        print("notes=" + notes)
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
