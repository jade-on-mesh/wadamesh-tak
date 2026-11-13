# Emoji

Standalones draw colour emoji from two places:

- **Built in:** about 280 common emoji are part of the firmware, so they always work.
- **The full set:** upstream Wadamesh's emoji pack, `emoji.pack`, with about 1,350 more (e.g. 🐠 🥇 🏀 🎵 🤌), every country flag and the England, Scotland and Wales flags. It loads into PSRAM a few seconds after start-up. With it in, the emoji picker gets tabs (Smileys, People, Nature, Food, Activity, Travel, Objects, Symbols, Flags, Chars) and a Recent tab.

Without the full set, an emoji that isn't built in shows as a box. That includes one in someone's name. A pack can only add emoji: a built-in one always shows as the firmware draws it.

## Getting the full set

You don't usually need to do anything. A standalone looks for the pack in these places, in order:

1. **Its internal storage**: the map tiles partition, at `/emoji.pack`. It's kept there once it has been found or downloaded. It's never kept with the chat history and contacts, because filling that storage makes it pause more often.
2. **The SD card**, at `/emoji/emoji.pack` or `/emoji.pack`, or the same places on the storage the USB Files page shows (upstream's places). It's copied onto internal storage from there.
3. **Wi-Fi.** It downloads [`deploy/site/emoji/emoji.pack`](deploy/site/emoji/emoji.pack) (about 1 MB) from this firmware's release, or from `main` if the release doesn't have it. On a board without the tiles partition it's saved to the SD card instead. If the download fails, it tries again every 10 minutes.

So a standalone on Wi-Fi gets it on its own. To install it on one that never uses Wi-Fi, copy `emoji.pack` (and `emoji/emoji-names.txt`, for search) into an `emoji` folder on its SD card.

**About** shows `Full emoji set:` followed by the emoji count and where it came from, or what it's waiting for. It needs about 2.5 MB of free PSRAM, so it's on the boards with 8 MB or more: not the 2 MB Heltec V4.

An earlier build's full set, `emoji.wep`, is a different format. It's deleted from internal storage on the first start, which gives its space back.

## Searching

The emoji picker's first cell is 🔍, on every tab. Pick it to open a search sheet. The **React** sheet has 🔍 too, so you can react with any emoji ([REACTIONS.md](REACTIONS.md)).

Search needs the full set and its names, [`emoji/emoji-names.txt`](emoji/emoji-names.txt) (about 105 KB). The names come the same way as the pack: from internal storage (`/emoji-names.txt`), the SD card (`/emoji/emoji-names.txt`), or a download once the pack is in. If they aren't loaded yet, 🔍 says so; **About** shows how far along it is.

- **Typing** shows emoji whose name or keywords start with what you type. `fish` finds 🐟 🐠 🎣, `trop fi` finds 🐠, and `heart` finds every heart.
- **Flags** come up by country name or code: `lesotho`, `united states`, `us`.
- **Results** are only emoji this standalone can draw.
- **Choosing a result** works as it does in the picker: it goes into your message, or becomes the chat icon if you opened the picker for that.
- **Enter** takes the best match.

The names and keywords are Unicode's and CLDR's English ones.

## What shows

- **Skin tones:** the emoji shows in its default yellow (the tone takes no space).
- **Combined emoji** (e.g. 🏳️‍⚧️): as one image when the firmware or the pack has it, else the parts side by side.
- **Flags:** every country and region flag is in the full set. Without it, only Lesotho's 🇱🇸 is built in.

## Making the files

The pack is upstream's (`scripts/build/gen-emoji-pack.py`; its format is at the top of `src/ui-touch/EmojiPack.cpp`, and `scripts/build/check-emoji-pack.py` checks a file as the standalone would). The search names:

```sh
curl -LO https://raw.githubusercontent.com/unicode-org/unicodetools/main/unicodetools/data/emoji/15.1/emoji-test.txt
curl -L -o annotations.json https://raw.githubusercontent.com/unicode-org/cldr-json/main/cldr-json/cldr-annotations-full/annotations/en/annotations.json
curl -L -o derived.json https://raw.githubusercontent.com/unicode-org/cldr-json/main/cldr-json/cldr-annotations-derived-full/annotationsDerived/en/annotations.json
python3 scripts/build/make-emoji-names.py emoji-test.txt annotations.json derived.json
```

One line per emoji: the emoji to insert, a tab, then `name|keywords`. CI checks the pack with `check-emoji-pack.py`, and checks with `check-emoji-names.py` that the names file is well formed and names every emoji the pack offers.

The art is Noto Color Emoji (SIL Open Font License). The names are from Unicode emoji 15.1 and the keywords from CLDR (Unicode License).

## How flags work

A flag is two regional-indicator letters, e.g. 🇺🇸 is 🇺 followed by 🇸. LVGL draws text one character at a time. So Wadamesh replaces its "next / previous letter" helpers with versions that read such a pair as a single letter (`src/ui-touch/FlagText.h`). Line wrapping, the cursor and Backspace then treat a flag as one character, as a phone does. The emoji font draws that letter as the pair's flag from the pack. Upstream's own flag handling (pairs turned into one private character when shown) works alongside it: both end at the same image.

Pairs are counted from the start of a run of letters, so 🇺🇸🇫🇷 is two flags. An odd letter left over shows on its own.
