#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds tuned Arduino 3.3.11 libraries for the ESP32-S3 with esp32-arduino-lib-builder
# (IDF5_MIGRATION.md, phase 3) and packs them as a PlatformIO package that
# platformio_idf5.ini's LilyGo_TDeck_idf5_tuned env names in platform_packages.
#
#   scripts/build/ci-idf5-libs.sh <stock dependencies.lock> <out dir>
#
# Pinned to what pioarduino 55.03.311's own libraries were built from (their
# versions.txt): lib-builder ee57070, ESP-IDF v5.5.5, arduino-esp32 0d7a8614b.
set -euo pipefail

LOCK="$(realpath "$1")"
OUT="$(realpath -m "$2")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${IDF5_LIBS_WORK:-$HOME/lib-builder}"
LB_REF=ee57070
IDF_REF=v5.5.5
AR_REF=0d7a8614b

# lib-builder clones "<owner>/arduino-esp32" for GITHUB_REPOSITORY_OWNER, with any
# GITHUB_TOKEN: point it at Espressif's, without our token.
export GITHUB_REPOSITORY_OWNER=espressif
unset GITHUB_TOKEN GITHUB_HEAD_REF || true

if [ ! -d "$WORK/.git" ]; then
  git clone -q https://github.com/espressif/esp32-arduino-lib-builder "$WORK"
fi
cd "$WORK"
git fetch -q origin
git checkout -q -f "$LB_REF"
git clean -qfd configs tools main

# Our settings on top of the S3 defaults, and the stock component versions.
{ echo; echo "# ---- Wadamesh TAK (arch/esp32/idf5_libs_esp32s3.defconfig) ----"; cat "$ROOT/arch/esp32/idf5_libs_esp32s3.defconfig"; } >> configs/defconfig.esp32s3
cp -f "$LOCK" dependencies.lock

# Pin arduino-esp32 to the commit the stock libraries used: install-arduino.sh only
# takes a branch, so check the commit out right after it.
sed -i "s|^if \[ \$? -ne 0 \]; then exit 1; fi|if [ \$? -ne 0 ]; then exit 1; fi\ngit -C \"\$AR_COMPS/arduino\" checkout -q $AR_REF \|\| exit 1|" tools/install-arduino.sh
grep -q "checkout -q $AR_REF" tools/install-arduino.sh

lb_build() { ./build.sh -t esp32s3 -I "$IDF_REF" -A master -D default; }
lb_diag() {
  # idf.py keeps the details out of the console: show what CMake and Kconfig said, and
  # every component manifest rule that depends on a Kconfig option.
  for f in build/log/idf_py_stderr_output_*; do
    [ -f "$f" ] && { echo "==== $f"; grep -v "unknown kconfig symbol" "$f" | tail -60; }
  done
  echo "==== manifest rules on Kconfig options"
  find . esp-idf/components -name idf_component.yml -not -path "./esp-idf/*/test*" 2>/dev/null \
    | xargs grep -n 'CONFIG' 2>/dev/null | head -60
}
if ! lb_build; then
  lb_diag
  # The stock component lock is what makes this build match pioarduino's libraries; if
  # the manager rejects it, try once with fresh resolution so there is still a result.
  echo "==== retrying without the stock dependencies.lock"
  rm -f dependencies.lock
  lb_build || { lb_diag; exit 1; }
  echo "NOTE: built with freshly resolved components, not the stock lock"
fi

SRC="out/tools/esp32-arduino-libs"
[ -f "$SRC/esp32s3/lib/libfreertos.a" ] || { echo "no esp32s3 libraries in $SRC" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
cp -a "$SRC/esp32s3" "$OUT/"
[ -f "$SRC/versions.txt" ] && cp -a "$SRC/versions.txt" "$OUT/" || true
cat > "$OUT/package.json" <<JSON
{
  "name": "framework-arduinoespressif32-libs",
  "description": "Arduino 3.3.11 ESP32-S3 libraries rebuilt with Wadamesh TAK's settings (IDF5_MIGRATION.md)",
  "version": "5.5.5+wadamesh.$(sha256sum "$ROOT/arch/esp32/idf5_libs_esp32s3.defconfig" | cut -c1-8)"
}
JSON
echo "tuned libraries: $OUT"
grep -E "FREERTOS_PLACE|RINGBUF_PLACE|HEAP_PLACE|BT_CTRL_RUN_IN_FLASH|NIMBLE_MEM_ALLOC_MODE_EXTERNAL|ALLOW_BSS_SEG" "$OUT/esp32s3/sdkconfig" || true
