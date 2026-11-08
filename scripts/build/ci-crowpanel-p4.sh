#!/usr/bin/env bash
# Wadamesh-tak: build the CrowPanel Advanced 10.1" P4 (crowpanel_p4_101/, an ESP-IDF
# project like the T-Display P4's) in CI, in the espressif/idf:v5.5.1 image, so a change
# that breaks it shows on the pull request rather than at release time
# (scripts/build/p4-release.sh builds the released images). Same setup as ci-p4.sh.
#
#   docker run --rm -v "$PWD:/w" -w /w espressif/idf:v5.5.1 \
#     bash scripts/build/ci-crowpanel-p4.sh <tag> <outdir> [1.0|1.1|1.2]
set -euo pipefail
TAG="${1:?tag}"; OUT="${2:?outdir}"; REV="${3:-1.2}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
git config --global --add safe.directory '*'
mkdir -p /tmp/sedshim
cat > /tmp/sedshim/sed <<'SH'
#!/bin/bash
a=()
while [ $# -gt 0 ]; do
  if [ "$1" = "-i" ] && [ $# -ge 2 ] && [ -z "$2" ]; then a+=("-i"); shift 2; continue; fi
  a+=("$1"); shift
done
exec /bin/sed "${a[@]}"
SH
chmod +x /tmp/sedshim/sed
export PATH="/tmp/sedshim:$PATH"
: "${IDF_PATH:?not in the espressif/idf image}"
: "${IDF_TOOLS_PATH:=/opt/esp}"
[ -e tanmatsu/esp-idf ] || ln -s "$IDF_PATH" tanmatsu/esp-idf
[ -e tanmatsu/esp-idf-tools ] || ln -s "$IDF_TOOLS_PATH" tanmatsu/esp-idf-tools
[ -d tdisplay_p4/components/lvgl/upstream/src ] || bash tdisplay_p4/fetch-deps.sh
unset WADA_P4_LCD WADA_P4_LR2021 WADA_P4_LEGACY_AT
cd crowpanel_p4_101
WADA_CP_REV="$REV" ./build.sh fullclean >/dev/null 2>&1 || true
WADA_CP_REV="$REV" WADA_FW_TAG="$TAG" ./build.sh build
APP="build/crowpanel_p4_101_v${REV/./}/application.bin"
[ -f "$APP" ] || { echo "::error::no $APP"; exit 1; }
mkdir -p "$ROOT/$OUT"
NAME="wadamesh-crowpanel-p4-101-v${REV/./}"
B="build/crowpanel_p4_101_v${REV/./}"
cp "$APP" "$ROOT/$OUT/$NAME.bin"
# The USB image: bootloader, partition table, OTA data and the app (as p4-release.sh).
python3 -m esptool --chip esp32p4 merge_bin --flash_mode dio --flash_size 16MB --flash_freq 80m \
  -o "$ROOT/$OUT/$NAME-merged.bin" \
  0x2000 "$B/bootloader/bootloader.bin" \
  0x8000 "$B/partition_table/partition-table.bin" \
  0xf000 "$B/ota_data_initial.bin" \
  0x20000 "$APP"
echo "built $(stat -c %s "$APP") bytes (5.5 MB slot)"
