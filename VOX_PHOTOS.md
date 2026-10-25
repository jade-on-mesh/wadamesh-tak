# MeshCore Open TAK photos and voice on wadamesh

Work in progress on the `vox-images` branch: showing the photo messages and
playing the Codec2 voice clips that
[MeshCore Open TAK](https://github.com/jade-on-mesh/meshcore-open-tak) sends,
on the radio itself.

## How the app sends a photo

A photo is announced with an ordinary text message,
`VI1:{sid}:14:{packets}:{aspect}` (base36), and its bytes are only sent when a
receiver asks for them: direct raw-data fetch first, then a `VR3:` DM, then a
GRP_DATA flood (type `0x5643`) on the channel the photo was posted in. The
reassembled payload is `[0x01][AV1 OBUs]`: one 8-bit 4:2:0 key frame with a
reduced still-picture header and no AVIF container, 2,750 B ("Standard") or
5,500 B ("More detail"). Wire details: `src/ui-touch/VoxWire.h`.

## Pieces

| Piece | Where | State |
|---|---|---|
| Channel data reaches apps and the UI | `MyMesh::onChannelDataRecv` | done |
| Wire format: envelope, packets, fetch requests, reassembly | `src/ui-touch/VoxWire.h`, `test/test_vox_wire.cpp` | done |
| AV1 decoder (dav1d 1.5.1, 8-bit, no asm) | `lib/dav1d`, `scripts/vendor_dav1d.sh` | done |
| Decode + exact Open TAK colour conversion | `src/ui-touch/VoxAv1Core.*`, `test/test_vox_av1.cpp` | done |
| Background decode task | `src/ui-touch/VoxAv1Job.*` | done, verified on a T-Deck |
| `.vi1` files open in Files → image viewer | `UITask.cpp` (`fmVoxPhotoStart`) | done, for testing |
| Fetch client (request, retry, store) | `src/ui-touch/VoxFetch.*`, `test/test_vox_fetch.cpp` | done, host-tested |
| Mesh + storage glue | `src/ui-touch/VoxPhotos.*`, `MyMesh` hooks | done |
| Photo bubbles in chat | `UITask.cpp` (`chatParseMessageDisplay`, `bubblePhotoTapCb`) | done, needs device testing |

## Photos in chat

- A `VI1:` message shows as a photo bubble instead of its text: "Photo - tap
  to download", then "Requesting...", "Downloading 12/36", "Photo - tap to
  view". `VR3:` fetch-request DMs never show in chat (a connected app still
  gets them).
- A photo sent to you in a DM downloads as soon as it arrives, as the app's
  auto-play does (on by default), and its thumbnail shows in the bubble by
  itself. A photo posted on a channel is pushed by its sender to the radios in
  range, which keep its packets the same way, so its thumbnail shows up too.
  Packets that arrive anyway (another listener's channel transfer, or the
  phone app's own fetch through this radio) are kept, and can finish a photo
  without a request. A channel photo that didn't all arrive downloads on a tap.
- The fetch follows the app (`voice_service.dart`), so Open TAK senders answer
  it unchanged:
  1. **direct**: a binary request over raw data along our route, carrying that
     route so the sender replies along its reverse; after a silent attempt the
     next one lets the sender use its own route instead;
  2. **VR3 DM**: flooded (the route is forgotten in RAM first), so both radios
     learn a route from the path exchange; the sender answers over raw data;
  3. **channel**: the request as GRP_DATA `0x5643` on the photo's channel, for
     channel posts; the sender, or anyone holding the photo, floods it back.

  Timeouts are sized from the radio's airtime and hop count; a re-request asks
  only for missing packets, and comes as soon as the sender's last packet is
  in. Up to 15 attempts (more for big photos: 10 + packets/6, so 49 for a
  234-packet Super Sharp), or 3 in a row that bring nothing; 5 once half is
  in, and 8 once 90% is in or 4 or fewer are missing, so a long route doesn't
  stop a packet short. Each fruitless request waits longer for its answer (up
  to 3x), every third reopens the channel, and the fourth floods a fresh DM.
- Channel posts carry only a name; the author is matched to a contact by name
  (case-insensitive, emoji variation selectors ignored). If none matches, only
  the channel fallback is possible.
- **Thumbnail first.** The app sends photos as payload version 2:
  `[2][thumbnail length][thumbnail OBUs][photo OBUs]`.
  - The thumbnail is an AV1 picture of up to 96 px, all of it in the first two
    packets (255 bytes at most). It comes out of the photo's budget, so the
    photo costs the same airtime. (The first version sent 64 px in one packet;
    both are read the same way.)
  - As soon as those packets are in (asked for, or pushed on a channel), the radio
    decodes the thumbnail and the bubble shows it, scaled up to the bubble's
    size, while the rest downloads.
  - When the photo completes, its own thumbnail replaces that one.
  - A channel photo whose first packet doesn't come by itself (the sender's
    push didn't reach here) is asked for: 4 to 8 s after the announcement,
    the radio asks the channel for the thumbnail's two packets alone. That's
    one small request and 312 bytes. The answer goes to the whole channel, so a
    listener that has heard them by then doesn't ask too.
  - If the answer doesn't come, it asks again, up to three times, 8 to 12 s
    apart, then stops. Nothing waits on it: the bubble offers the download
    the whole time, and a tap downloads the photo as before.
  - Version 1 payloads (`[1][photo OBUs]`) still open. Versions before this
    one can't read version 2.
- Finished photos are stored as `/vox_<sid>.vi1` on internal storage, so they
  survive a reboot and open in Files too. When a download you started
  finishes, the photo is decoded on the background task and opens in the image
  viewer by itself; tapping a downloaded photo later opens it again.

## Voice clips

- A `VE3:` (Codec2) message shows as a voice bubble with its length: "Voice 7s
  - tap to play".
- A voice message sent to you in a DM downloads as soon as it arrives, like a
  photo, so a tap plays it at once. A channel's clip is pushed to the radios
  in range.
- A tap on a clip that isn't here yet downloads it the same way as a photo and
  plays it as soon as it arrives. Later taps play it again, or stop it while it
  plays.
- Downloads of voice and photos share their timing. A clip coming over radio2
  (MediaMesh) is timed by radio2's airtime. Once a few packets are in, the
  stall wait follows the gaps they actually came at, so a lost tail is asked
  for again in seconds.
- Every request is salted (`VoxWire::encodeFetch`): one listed packet is
  repeated 1-16 times, counting up from a random start for each download.
  Radios drop a packet whose payload they have already seen, so an unsalted
  retry for the same last packet or two was dropped by the first repeater
  that relayed the earlier one, and a download several hops away stalled at
  the end ("Got 213/215"). A retry before any packet came in lists the
  packets instead of asking for "all", so it can be salted too. The app does
  the same.
- Decoding uses the app's own Codec2 tree (`lib/codec2`, speech codec only,
  codebooks in flash) and is sample-exact with it (`test/test_vox_voice.cpp`,
  four modes). Every mode the app can send plays except 700C, compiled out to
  save 82 KB of flash; the app sends 1200 by default.
- Playback streams frame by frame through the audio sink Lua apps use, on its
  own task with a 32 KB internal-RAM stack (Codec2 needs ~22 KB), so it works
  on boards with a speaker: the T-Deck and the T-LoRa Pager. Notification
  chimes wait while it plays. Others show the bubble and say there's no speaker.
- Clips are stored as `/vox_<sid>.ve3`: one byte of wire mode, then the frames.
- **Mimi** (`VM1:`), the app's default once its model is downloaded, needs a
  223 MB neural model and can never play on a radio; those bubbles say so. To
  reach radios, the sender has to use Codec2.

## The decoder on the device

- dav1d is vendored unmodified apart from `lib/dav1d/patches`:
  - every allocation goes through `dav1d_ext_*`, PSRAM first;
  - ~221 KB of tables upstream keeps in `.bss` (inter-prediction masks,
    quantizer matrices) move to the heap;
  - the biggest DSP scratch buffers (self-guided loop filter, inverse
    transform, motion compensation, up to 34 KB each) move from the stack to
    one 40 KB PSRAM arena.

  Tables and arena are allocated by the first decode and kept for the next.
- Each decode runs on its own task on core 0 at idle priority, with a 24 KB
  stack taken from internal RAM only while it runs (this Arduino build can't
  put task stacks in PSRAM). The host measures ~11 KB of stack at worst,
  including damaged input. Boards idling at 80 MHz are raised to 240 MHz for
  the decode. The first device build used a 64 KB stack, which the T-Deck
  couldn't spare: every photo failed with "Not enough memory".
- Heap per decode, measured on the host: up to ~0.9 MB including the RGB565
  output, plus the 221 KB of kept tables.
- Output is bit-exact with libaom's decode in the app (`vox_av1.c`), converted
  with the same full-range BT.601 formulas, then packed to RGB565.
- Flash: the AV1 decoder is ~285 KB of code at `-O2` (~255 KB at `-Os`);
  Codec2 and the voice player add ~75 KB on the T-Deck. As of `4373f43`, CI's
  T-Deck `firmware.bin` is 3,939,792 B and the M9's 3,794,480 B (no speaker,
  so no Codec2), against 4,063,232 B app slots: about 120 KB and 265 KB left.
- Tanmatsu and T-Display P4 builds don't link dav1d yet; `VOX_AV1_AVAILABLE` is
  0 there and opening a photo says it isn't supported.

## Testing on a device

1. Flash the build from this branch (CI uploads `firmware.bin` for the T-Deck
   and ThinkNode M9: the app-only image, for Launcher or `0x10000`).
2. Copy a few files from `test/data/vox/` to the SD card or internal storage
   (USB Files works). They are real Open TAK payloads of public-domain photos
   (NASA, SpaceX, and CC0 photos from scikit-image), at both detail levels.
3. Open one in **Files**. "Decoding photo..." appears, then the viewer, then a
   note with the decode time, clock, stack and heap used. The same line goes
   to the serial log as `[VOX] decode ...`. A failure shows its reason and code
   with the largest free internal-RAM block and free PSRAM at the time.

Worth trying `coffee_5500.vi1` and `astronaut_5500.vi1`: they take the deepest
stack path.

## Measured on a T-Deck (2026-10-11, build `d3c550d`)

| Photo | Size | Decode | Stack | Heap |
|---|---|---|---|---|
| `astronaut_5500.vi1` | 256×256 | 3,408 ms | 7 KB | 853 KB |
| `coffee_5500.vi1` | 384×256 | 3,057 ms | 7 KB | 729 KB |
| `hubble_deep_field` (both detail levels) | 384×335 | 2,851–3,277 ms | 5 KB | 883–886 KB |
| `rocket_*.vi1` | 320×214 | 1,928 ms | 5 KB | 669 KB |

All at 240 MHz. Colours and geometry look right. Peak stack is 7 KB of the
24 KB reserved. Decode takes 2–3.5 s, small next to the minutes a photo takes
to arrive over LoRa. Images using the self-guided loop filter (astronaut) are
slowest; that filter's scratch is in PSRAM, so trying the arena in internal RAM
first is an easy speed experiment.
