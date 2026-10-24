# Team on Home

Three Home widgets for standalones on a team, and a **Team** preset that lays
them out with battery, unread, the clock, mentions and GPS (*Settings › Display
› Home › Start over from a preset › Team*, or hold Home and add them one by
one; they're under *Mesh*).

| Widget | Shows | Tap |
| --- | --- | --- |
| **Nearest teammate** | Who, how far and which way ("420 m NE"), when last heard, their battery and status; red with SOS | Their sheet (Hound, Map, answer an alert) |
| **Latest alert** | A teammate's alert while one is on (red), else the newest watched report (#icewatch…) in its colour, with distance and age | The alarm, or the report's sheet |
| **Mesh health** | A score each minute, 0–100, with the last half hour as a curve: up to 50 for how much was heard (5 a packet) and up to 50 for how well (the last packet's SNR, −15 to +10 dB). Good from 60, fair from 30 | Signal & traffic |

Each comes small (one cell: a figure and a word) or wide (two cells: title,
detail, and the curve for mesh health).

Not on the T-Display P4, whose app space is full (`CAP_TAK_EXTRAS`). Their kinds are numbered from 200 in the saved layout so
they never clash with upstream Wadamesh's.

## More widgets

Widgets for the fork's other features. Add them from Arrange (hold Home): the
first three are under *Messages*, MediaMesh is under *The radio*, and the rest
are under *The mesh*.

| Widget | Shows | Tap |
| --- | --- | --- |
| **Latest photo** | The newest photo sent to you or posted on a channel: its thumbnail (the one it sends first, while it downloads), who, when, and how far along. Small: the picture alone. Large: a big picture with who and when under it | Opens it, or downloads it and opens it when it lands |
| **Voice & talk** | The newest voice message or push-to-talk: who, how long, when, and whether it's here, downloading or playing | Plays it (downloading it first if needed); again to stop |
| **OTP pads** | The pad with the least left (the one to refill) and unread OTP messages; amber under 1,000 bytes, red under 200 | OTP Messenger |
| **MediaMesh** | radio2 on or off, whether a MediaMesh was heard in the last minute or ten, and its frequency and bandwidth | Settings › Radio, where MediaMesh is |
| **Route** | What Bloodhound is following (a team route, your track back, a teammate or a point): the next turn, or how far and which way | The map |
| **Nearest camera** | The nearest known Flock ALPR camera within 5 km, how far and which way; red inside your warning distance or while a Flock device is detected | Flock You |
| **Drones** | How many drones are in the air around you (Remote ID) and the nearest one's distance, direction and height | Drones |

Latest photo needs the AV1 decoder, so it isn't on the T-Display P4 or Tanmatsu.
Voice & talk needs a speaker. Nearest camera and Drones need Flock You's radios,
so they aren't on the T-LoRa Pager. Route needs `CAP_TAK_EXTRAS`.
MediaMesh needs a radio that can run radio2. The latest photo and voice message
are remembered from when the standalone started (RAM only).

## Code

`src/ui-touch/UITask.cpp`: `hwMate*`, `hwAlert*`, `hwMeshHealth*`, `kHwPresetTeam`;
`hwPhoto*`, `hwVoice*`, `hwOtp*`, `hwMediaMesh*`, `hwRoute*`, `hwFlock*`, `hwDrone*`
(kinds 203-209). The newest photo and voice message: `VoxPhotos::latest()`.
