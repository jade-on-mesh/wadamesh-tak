#!/usr/bin/env bash
# Pinned third-party sources for the radio emulator tests, into emu/third_party
# (gitignored): RadioLib (the version the PlatformIO builds resolve ^7.6.0 to)
# and the meshcomod core at the tag platformio.ini pins.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tp="$here/third_party"
mkdir -p "$tp"
fetch() {   # dir url ref
  if [ -d "$tp/$1/.git" ] && [ "$(git -C "$tp/$1" describe --tags --exact-match 2>/dev/null)" = "$3" ]; then return; fi
  rm -rf "${tp:?}/$1"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$tp/$1"
}
fetch RadioLib https://github.com/jgromes/RadioLib.git 7.8.1
fetch meshcomod https://github.com/ALLFATHER-BV/meshcomod.git core-v1.18.0
