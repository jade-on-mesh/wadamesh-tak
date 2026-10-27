# Linked images

When someone posts a link to a picture in a channel or a DM, the radio can download it over Wi-Fi and show it in the message bubble.

**Settings > General > Linked images > Download over Wi-Fi** is on by default. Downloading a link tells that picture's server this radio's internet address, so turn it off if you'd rather not.

## What it does

- A message with a link whose path ends in `.jpg`, `.jpeg`, `.png`, `.gif`, `.webp` or `.bmp` gets its picture fetched. The link may have a `?query` after it, as image hosts often add. Your own messages count too.
- One picture is fetched at a time, while Wi-Fi is connected. The newest one waiting goes first, which is usually the one on screen. With no Wi-Fi, the radio waits for it.
- The bubble shows a thumbnail above the text, the same size as a mesh photo's. **Tap the thumbnail** to open the picture full size in the image viewer.
- Tapping the link itself still opens the link menu, which now has **View picture** first when the link is to a picture.
- With an SD card in, the picture is kept in `/linkimg/` on the card, so it opens again without Wi-Fi. Without a card only the thumbnail is kept, and viewing it full size downloads it again.

## Limits

- Up to 3 MB a picture on the T-Deck and the other 8 MB boards, and 8 MB on the P4 and the Tanmatsu.
- What the server sends is decoded, whatever the link says. A `.jpg` link that serves a WebP still shows.
- A GIF shows its first frame. Animated WebP isn't supported.
- A picture too big to decode in the memory that's free is scaled down while decoding if it's a baseline JPEG. Otherwise it's refused, and the log says why.
- A failed download isn't retried until the radio restarts, except one that failed for want of Wi-Fi.
- Only on boards with the web reader (`CAP_WEB_BROWSER`). The 2 MB Heltec V4 can't fit the HTTPS handshake.

## How it's built

- `src/helpers/LinkImage.*` finds the link in a message and decodes the file to RGB565 at the size asked for. It uses [stb_image](https://github.com/nothings/stb) for JPEG (progressive too), PNG, GIF and BMP, and [simplewebp](https://github.com/MikuAuahDark/simplewebp) for WebP, both in `lib/`. This part has no radio code in it, so `test/test_link_image.cpp` runs it on a computer in CI.
- `src/ui-touch/LinkImages.inc` does the downloading, the SD copy, the bubble's thumbnail and the viewer. It works on the shared audio worker, or a task of its own, so the screen never waits on the network. Thumbnails go in the photo store beside mesh photos' thumbnails, under the link's id.

MeshCore Open TAK has the same option, under **Settings > Linked images**.
