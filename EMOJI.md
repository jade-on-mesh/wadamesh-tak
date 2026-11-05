# Emoji

Standalones draw colour emoji from two places:

- **Built in:** about 280 common emoji are part of the firmware, so they always work.
- **The full set:** every other single-character emoji (about 1,400 more, e.g. 🐠 🥇 🏀 🎵 🤌) and every flag comes from a file, `emoji-pack.wep`. It loads into PSRAM a few seconds after start-up.

Without the full set, an emoji that isn't built in shows as a box. That includes one in someone's name.

## Getting the full set

You don't usually need to do anything. A standalone finds the file in one of these places, in order:

1. **Its internal storage** (`/emoji.wep`). It's kept there once it has been found or downloaded.
2. **The SD card**, at `/emoji/emoji-pack.wep` or `/emoji-pack.wep`. It's copied onto internal storage from there.
3. **Wi-Fi.** It downloads `emoji/emoji-pack.wep` (about 820 KB) from this firmware's release, or from `main` if the release doesn't have it. If that fails, it tries again every 10 minutes.

So a standalone on Wi-Fi gets it on its own. To install it on one that never uses Wi-Fi, copy [`emoji/emoji-pack.wep`](emoji/emoji-pack.wep) to its SD card.

**About** shows `Full emoji set:` followed by the emoji count and where it came from, or what it's waiting for. It needs about 2 MB of free PSRAM. On a board without PSRAM, only the built-in emoji show.

## What shows

- **Skin tones:** the emoji shows in its default yellow.
- **Combined emoji** (e.g. 🧑‍🧑‍🧒): the parts show side by side.
- **Flags:** every country and region flag (259) is in the full set. Without the full set, only Lesotho's 🇱🇸 is built in, as before.

## Making the file

```sh
git clone --depth 1 --branch v2.051 --filter=blob:none --sparse \
    https://github.com/googlefonts/noto-emoji.git noto
git -C noto sparse-checkout set png/128 third_party/region-flags/png
python3 scripts/build/make-emoji-pack.py noto/png/128 noto/third_party/region-flags/png
```

The images are made and compressed the same way as the built-in ones (`add-emoji.py`, `pack-emoji.py`). The script also writes `test/emoji_pack_file_expected.h`, and CI's `test_emoji_pack_file` checks the committed file against it. The file format is described at the top of the script.

The art is Noto Color Emoji v2.051 (SIL Open Font License).

## How flags work

A flag is two regional-indicator letters, e.g. 🇺🇸 is 🇺 followed by 🇸. LVGL draws text one character at a time. So Wadamesh replaces its "next / previous letter" helpers with versions that read such a pair as a single letter (`src/ui-touch/FlagText.h`). Line wrapping, the cursor and Backspace then treat a flag as one character, as a phone does. The emoji font draws that letter as the pair's flag.

Pairs are counted from the start of a run of letters, so 🇺🇸🇫🇷 is two flags. An odd letter left over shows on its own.

