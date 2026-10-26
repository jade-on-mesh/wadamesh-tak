# AI voice restorer (experimental)

Codec2 voice sounds buzzy and robotic, with nothing above 4 kHz. MeshCore Open
TAK rebuilds it with an AI voice restorer, a 28 MB model far too big and slow
for a standalone. This is a small one, about 30 times smaller, to find out how
it sounds and how fast each standalone runs it.

## Trying it

1. Train it with MeshCore Open TAK's voice training kit
   (`tools/voice-training`): option **4, Small restorer**, about 2 hours on a
   gaming GPU. Train the big restorer (option 2) first if you can: the small
   one then learns to sound like it.
2. Copy `output/vox-restorer-small.bin` (about 1 MB) to the standalone's SD
   card as `/models/vox-restorer-small.bin`.
3. **Settings > Sound > AI voice restorer**: on.
4. Play a voice message. The clip is restored first, then plays, so there is a
   pause before it starts on a slow board. The setting's line then says how
   long the last clip took ("Last clip: 8.0 s of voice restored in 5.1 s"),
   or why it couldn't be used.

Off by default. Turning it off frees the model's memory. Talks heard live
(the walkie-talkie) play as before; Brighter voice isn't used on a restored
clip, which already has its treble.

## What it does

`src/ui-touch/VoxRestoreSmall.cpp`, plain C++ with no libraries:

- Codec2 decoded at 8 kHz, levelled as plain playback is.
- An STFT (256 samples, hop 64) and 32 log-mel bands up to 4 kHz.
- A 7-wide convolution into 128 channels, then 4 ConvNeXt blocks (depthwise
  convolution, LayerNorm, 128 -> 384 -> 128 with GELU).
- A head giving log-magnitude and phase for an inverse STFT (512 samples, hop
  128) at 16 kHz, the standalones' speaker rate.

About 0.5 million weights (kept as floats in PSRAM, about 2 MB, while it's on)
and 60 million multiply-adds per second of voice. A clip also needs about
160 KB of PSRAM per second of voice while it's restored. The model file's shape is read
from its header, so a kit run at another size (`KIT_SMALL_DIM`,
`KIT_SMALL_LAYERS`) loads too.

`test/test_restore_small.cpp` checks it against the kit's numpy reference
(`kit/small.py`), with a tiny model in `test/data/restore_small`.
