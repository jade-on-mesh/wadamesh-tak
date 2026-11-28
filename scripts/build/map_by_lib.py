#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Bytes per library (archive) and output region from a GNU ld map file, for comparing
# builds (IDF5_MIGRATION.md). Usage: map_by_lib.py firmware.map [top N]
import re
import sys
from collections import defaultdict

REGIONS = {"flash": re.compile(r"^\.flash\.(text|rodata)"), "iram": re.compile(r"^\.iram0\.text"),
           "dram": re.compile(r"^\.dram0\.(data|bss)")}
# "  .text.foo   0x42001234   0x56 /path/libbar.a(obj.o)"  (name may sit on its own line)
LINE = re.compile(r"^\s+(\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$")


def lib_of(path):
    m = re.search(r"([^/\\]+\.a)\(", path)
    if m:
        return m.group(1)
    if path.endswith(".o"):
        return "(objects) " + path.split("/")[-1]
    return path.split("/")[-1]


def main():
    totals = defaultdict(lambda: defaultdict(int))
    region = None
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            if line and not line[0].isspace():
                name = line.split()[0] if line.split() else ""
                region = next((r for r, rx in REGIONS.items() if rx.match(name)), None)
                continue
            if not region:
                continue
            m = LINE.match(line.rstrip("\n"))
            if not m or m.group(1) in ("*fill*",):
                continue
            size = int(m.group(3), 16)
            if size == 0 or not int(m.group(2), 16):
                continue
            totals[region][lib_of(m.group(4))] += size
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    for r in ("flash", "iram", "dram"):
        items = sorted(totals[r].items(), key=lambda kv: -kv[1])
        print("%s total %d" % (r, sum(v for _, v in items)))
        for name, v in items[:top]:
            print("  %8d %s" % (v, name))


if __name__ == "__main__":
    main()
