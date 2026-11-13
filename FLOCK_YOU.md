# Flock You

A Flock Safety ALPR detector and a DeFlock-style camera map for Wadamesh. App
drawer > **Flock You**. On every board with Wi-Fi and NimBLE (T-Deck, ThinkNode
M9, the CrowPanels...); not on the Pager yet (`CAP_FLOCK_YOU` in
`src/ui-touch/device_caps.h`).

**T-Deck (Plus): one USB install.** With Flock You the image outgrew the
T-Deck's 3.875 MB app slots, so its env now uses
`variants/lilygo_tdeck/partitions_tdeck_touch_big_app.csv`: 4.375 MB slots, the
map tile partition down from 4.75 to 3.75 MB, user data (spiffs) untouched. Since
upstream's beta_103 the slots are 4.5 MB and the tiles 3.5 MB (a second USB install).
Install the full merged image once over USB (the web flasher does this); after
that, updates over Wi-Fi work as before. Until then an update over Wi-Fi stops
with "too big for this radio's slots". Cached map tiles are cleared by the
move and download again. The T-Deck Pro and Max keep the old table.

It does three things:

1. **Listens.** Wi-Fi probe requests (promiscuous mode, hopping channels 1-13)
   and BLE advertisements are matched against Flock camera and Raven sensor
   signatures. Hits are logged with your GPS position and drawn on the map.
2. **Knows where cameras already are.** Cameras from OpenStreetMap (where
   [DeFlock](https://deflock.me) maps them), downloaded by the radio over Wi-Fi
   or put there as a `cameras.csv`, show every
   known ALPR on the map, and the radio warns as you come within range of one.
3. **Tells the mesh.** Confident detections, and cameras you mark by hand, can
   be posted to a channel you pick, e.g. `#flockwatch`. Other Wadamesh radios
   (Team > Watch) and MeshCore Open TAK (Watch tab) watching that channel put
   them on their maps.

## Detection

| Rule | What it matches | Confidence |
|------|-----------------|------------|
| Wi-Fi probe | A probe request from a Flock module OUI with the camera's exact IE sequence | high |
| Raven sensor | Two or more of Raven's GATT services (0x3100-0x3500) | high |
| BLE name | "FS Ext Battery", "Pigvision" | high |
| Raven-like | One Raven service | medium |
| BLE maker id | XUNTONG manufacturer data (0x09C8) | medium |
| BLE name (weak) | "Penguin...", "Flock..." | medium |
| BLE address | A *public* BLE address from a Flock module OUI | low |

Every rule is a heuristic: the same Wi-Fi and BLE chips go into other products.
Low-confidence hits show on the map faded and are never posted unless you ask
for "everything".

Changes from the 1.2 app: the log no longer stops at 160 entries; Raven UUIDs
match whether advertised as 16- or 128-bit, and the standard Device
Information / Health Thermometer / Location services no longer count on their
own; random BLE addresses never match on OUI; the `82:6b:f2` and `cc:cc:cc`
prefixes are gone; each BLE device is reported once per scan window instead of
on every advertisement.

## Sharing the radios

Flock You only borrows what the rest of the firmware is not using:

- **Wi-Fi** is sniffed only while the station is not connected, no access point
  is up (web remote, file transfer) and no Wi-Fi scan runs. With a saved network
  it waits 40 s after boot, and then steps aside 12 s out of every minute so the
  firmware's reconnect attempts can get through. Connect to Wi-Fi and Flock You
  lets go at once.
- **BLE** shares NimBLE's scanner: a 4 s window every 10 s, 60 % duty, so the
  phone link keeps working. It stays off while the Bluetooth keyboard is
  scanning, pairing or reconnecting, and while Bluetooth is off.
- **GPS** is only read.

Listening survives a reboot; *Stop listening* hands both radios back.

## Known cameras

**On the radio, over Wi-Fi.** Cameras > *Download cameras within 50 km now*
fetches the ALPRs mapped around the radio (its GPS fix, else the position set
in Settings) from OpenStreetMap's Overpass API. *Area* picks 25, 50, 100 or
200 km. Turn on *Keep them current over Wi-Fi* and it downloads by itself
whenever Wi-Fi is connected and the radio has moved half the area away from
the last download, or a week has passed. A failed try waits 30 minutes. The
download replaces `cameras.csv` only once it has arrived whole, and never
touches `marked.csv`. Wi-Fi sniffing already steps aside while Wi-Fi is
connected, so the two don't meet.

**From a computer**, for a whole state or an exact box:

```
scripts/flock_cameras.py --bbox 47.0,-123.0,48.2,-121.5 -o cameras.csv
scripts/flock_cameras.py --area "Washington" -o cameras.csv
```

Copy `cameras.csv` into the radio's `flock-you/` data folder (on the SD card,
`meshcomod/flock-you/` when the radio keeps its data there) with Files, Transfer
or USB Files, then Flock You > Cameras > *Reload the camera files*. Format, one
camera a line: `lat,lon[,direction[,label]]`. Up to 40,000 cameras; they load in
the background without stalling the UI.

On the map: a red tile is a known camera (with an arrow for the way it faces,
zoom 15 and in), a pink one is a camera you marked, a number is a cluster (tap
to zoom in). Amber dots are Wi-Fi hits, teal dots BLE hits; faded ones are from
the log.

**Mark a camera** you can see: Cameras > *Mark a camera at my position*, or
long-press the map (when Team mode is not using long-press). Pick the way it
faces. Marks go to `marked.csv`, ready to add to OpenStreetMap through DeFlock.

## Alerts to a channel

Setup > pick the alert channel (any channel but Public), then *Post detections
there*. A post looks like:

```
🔴 ALERT Flock ALPR
Wi-Fi probe · -61dBm
📍 47.60378,-122.34052
```

- Only detections with a GPS fix are posted (a hit waits up to 10 minutes for
  one), at the chosen confidence or above (high only by default).
- At most one post a minute; a device is posted once a session and not again
  within 30 minutes.
- Marked cameras post as `🟡 OBSERVED Flock camera (marked)`.
- *Show #flockwatch alerts from others on the map* adds the channel to Team's
  watched channels.

Posts carry your position to everyone on that channel. A hashtag channel is
open to anyone who knows its name; use a private channel to keep it to a group.

## Files

| File | What |
|------|------|
| `src/ui-touch/FlockCore.h` | signatures, detection table, log/camera formats, alert text, send limit (host-tested: `test/test_flock_core.cpp`) |
| `src/ui-touch/FlockRadio.cpp` | Wi-Fi and BLE ownership, the detection queue |
| `src/ui-touch/FlockYouUI.inc` | app page, map layer, proximity, channel posts, files |
| `scripts/flock_cameras.py` | cameras.csv from OpenStreetMap |

On the radio: `flock-you/detections.csv` (rotated to `detections.old.csv` at
512 KB), `flock-you/cameras.csv`, `flock-you/marked.csv`; settings in
`/flockyou` on internal flash.

Signatures adapted from [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)
(MIT) and [wgreenberg/flock-you](https://github.com/wgreenberg/flock-you); the
first Wadamesh T-Deck port by Chase H. Barnes.
