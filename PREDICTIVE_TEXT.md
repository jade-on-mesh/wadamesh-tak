# Predictive text

While you type a message, up to three words appear in a slim strip along the
top of the message field, in the small font, the best one first and brightest.
Tap one to put it in, with a space after. They are, as on a phone keyboard:

- **Completions** of the word you're on, from its first letter.
- **Corrections** of a mistyped word: one letter wrong, missing or extra, or
  two swapped ("teh" -> "the", "recieve" -> "receive", "becuase" -> "because").
  A slip onto a neighbouring key counts as the likeliest. A missing apostrophe
  is always fixed ("dont" -> "don't", "im" -> "I'm").
- **The next word**, after a space, before you've typed any of it: "on my"
  offers "way", "thank" offers "you". After a full stop the suggestions are
  capitalised.

What comes first depends on the word before as well as the word itself.
Completions that usually follow the previous word rank higher ("good mor" ->
"morning").

**Space fixes or completes a word that isn't one**, as on a phone. "teh" +
Space gives "the "; "thoguh" + Space gives "though ". A real word is left
alone: "in" stays "in". Of the real words, only the spelling of "i" changes,
to "I". A short word (three letters or fewer) is only corrected for a likely
slip, so "thx" stays "thx".

**Two or three letters complete too**, when they are rarely a word on their
own: "th" + Space gives "the ", "co" gives "could", "tod" gives "today", "eve"
gives "even". The dictionary lists many such fragments and abbreviations, which
used to count as real words and stay as typed. Common short words ("go", "to",
"be"), chat words ("hi", "ok", "lol") and real three-letter words ("tea",
"bus", "bat") stay; so does any short word you have sent before.

A Backspace straight after puts back what you typed, without the space. A
space then keeps it as typed, and the word is learned, so it isn't changed
again.

*Settings › Keyboard › Typing › Space completes the word* turns just this off.

## Where the words come from

- **The words you send.** The more often you send a word, and the more recently,
  the higher it ranks. Place names, callsigns and channel words you use show up
  after you've sent them once. Words with digits aren't learned. Up to 400 words.
- **The word pairs you send** ("on my radio"), for the next word: your pairs
  outrank the built-in ones. Up to 400 pairs.
- **A built-in dictionary**: the 10,000 most used English words with how often
  each is used, plus mesh and team words ("repeater", "roger", "checkpoint",
  "MeshCore"...).
  - The dictionary is [wordfreq](https://github.com/rspeer/wordfreq)'s English
    list (Robyn Speer et al., CC-BY-SA 4.0). It is built from subtitles,
    Reddit, Twitter, news and Wikipedia, so it knows chat words ("gonna", "lol",
    "tho").
  - Profanity is left out of suggestions.
  - Apostrophe-less contractions ("dont") are left out so they get corrected.
  - The dictionary takes about 57 KB of flash.
- **A built-in table of what usually comes next** in chat on the mesh.

A capital typed ("Tho") gives capitalised words ("Thornton"). The strip steps
aside for the @-mention and accent pickers. The learned words and pairs are
kept in `/words` on the internal store, written 30 s after a send. Older
firmware reads the words from that file and skips the pairs.

*Settings › Keyboard › Typing › Predictive text* turns it on or off.

## The T-Display P4

The P4's app slot is full, so its firmware has no dictionary built in
(`PREDICT_BUILTIN_WORDS 0`). It uses the same dictionary as a 57 KB file,
`dictionaries/words-en.pwd`, kept on its internal storage. The P4 gets it from
the first of these that works:

1. its internal storage (`/words-en.pwd`), once it has the file;
2. the SD card, in `/predict/words-en.pwd` (or `/words-en.pwd` at the top). It
   copies the file to internal storage, so the card can come out again;
3. a download over Wi-Fi, the first time Wi-Fi is up. It fetches the copy in its
   own firmware release's tag on GitHub (or `main`'s), and retries every 10
   minutes until it works.

A notice says when a download is installed. *Settings › Keyboard › Typing*
shows the dictionary's state under the switches.

The firmware only takes the exact file it was built with: its size and an
FNV-1a checksum are in `PredictWordsInfo.h`, generated with the file. Until the
P4 has the dictionary, it suggests your own words and the next-word table.

## Rebuilding the dictionary

```
pip install wordfreq better_profanity
python3 scripts/predict/gen_words.py            # PREDICT_WORDS=15000 for more
```

This writes three files:

- `src/ui-touch/PredictWords.inc`: the dictionary, compiled in.
- `dictionaries/words-en.pwd`: the same dictionary as a file, for the P4.
- `src/ui-touch/PredictWordsInfo.h`: the file's size and checksum.

The words are sorted, front-coded in blocks of 32 with an index for binary
search, and carry a frequency byte (zipf × 10). The file is `PWD1`, then the word
count and the block count (u32 LE), then the block offsets, then the data.

## Code

| Piece | Where |
| --- | --- |
| Dictionary lookup, completions, corrections, next word, ranking, Space's choice, learning, save/load (host-tested) | `src/ui-touch/PredictText.h`, `test/test_predict_text.cpp` |
| The dictionary (generated) | `src/ui-touch/PredictWords.inc`, `dictionaries/words-en.pwd`, `src/ui-touch/PredictWordsInfo.h`, `scripts/predict/gen_words.py` |
| Loading the file (host-tested) | `dict::loadFile` in `PredictText.h`, `test/test_predict_file.cpp` |
| The P4's copy: storage, SD, download | `predictDictLoadLocal`, `predictDictTick` in `UITask.cpp`; the worker's `s_pwd_request`; `forkFetchFile` in `ForkUpdate.inc` |
| The strip over the message field, Space (`wordSpaceComplete`), learning on send and on undo, the settings | `wordBoxMaybeShow`, `predictLearn`, `predictToggleCb` in `src/ui-touch/UITask.cpp` |
