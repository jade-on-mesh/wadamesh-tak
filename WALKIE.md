# Walkie-talkie (push to talk)

Hold a button and talk: everyone on the channel with MediaMesh hears you about
two seconds later, while you're still talking, on standalones and in MeshCore
Open TAK alike. When you let go, the talk is posted in the chat as an ordinary
voice message, so anyone who missed it can play it later.

## Using it

1. Turn on MediaMesh (radio2 on rx+tx, photos and voice on radio2): talks go
   out on radio2 only, like voice messages. See [MEDIAMESH.md](MEDIAMESH.md).
2. Long-press a channel in Chats and pick **Walkie-talkie**.
3. **Hold to talk**, and let go to finish. A talk can be up to 20 seconds.

On the T-Deck, **hold the keyboard's Mic key** (the one with the microphone,
which types 0 with Sym) to talk from any screen, even locked, without opening
anything first: after a third of a second the walkie-talkie card comes up on
the air, and letting go ends the talk. It talks on the channel whose chat is
open, otherwise on the walkie-talkie you opened last. This needs the
keyboard's current firmware (raw mode, the default); with **Older keyboard
protocol** on, the keyboard reports no key releases, so use the card's button.

On the T-Deck, the **trackball's button** is push-to-talk in a channel's chat:
press it in and you're talking (the walkie-talkie card comes up), let go to
finish. It works the same with the walkie-talkie card up, from any screen. In
D-pad mode it talks only with the composer focused (elsewhere it selects, so
it holds the card's Talk button). Everywhere else the button clicks as before.
Turn it off with **Trackball button talks in channel chats** on the card.

Without the trackball: hold the Mic key (above), or long-press a channel in
Chats, choose **Walkie-talkie**, and hold the card's **Talk** button on the
touchscreen.

A talk goes only to the channel it's on, and only people with that channel
hear it. The Mic key talks on the channel whose chat is open (else the
walkie-talkie opened last); in a DM it doesn't talk at all, as a talk would go
to a whole channel: record a voice message there instead.

Talks play by themselves as they arrive on the channel whose chat or card is
open. **Play talks on every channel** (off by default) plays them from every
channel, like a scanner. The card shows who is talking. While someone else is talking, the button waits
until they finish. It's half duplex: nothing plays while you talk.

- **Reach** switches between *through repeaters* (the default: the whole
  MediaMesh hears it live, 2-3 s behind) and *direct* (radio to radio: only
  radios in range hear it live, a little sooner, and repeaters stay quiet).
  People out of range still get the voice message.
- **Play talks on every channel** is off by default: talks play in the
  channel you're in. On, every channel's talks play as they arrive.
- **Mute voice in this channel**: its talks don't play, even with its chat
  open (only with its card open). They still arrive as voice messages, to
  play with a tap.

A talk heard live in full is kept with its voice message, so the message plays
straight away with nothing to download.

In the chat a talk shows as **Talk**, with a waveform of the speech on the line
under it instead of a plain voice message. The waveform is worked out once the
talk is stored here (heard live in full, or downloaded; a flat line until
then), and fills in with the accent colour as it plays. MeshCore Open TAK draws
the same waveform: each of its 40 bars is the loudness of its slice of the
decoded speech.

Talking needs a microphone (T-Deck, T-Display P4). Boards with a speaker but no
microphone (the T-LoRa Pager, the Tanmatsu) can listen.

## On the air

Talk packets are channel data (GRP_DATA) of type `0x5643`, the same as MeshCore
Open TAK's channel photos and voice, so MediaMesh routing sends them on radio2
only (`markTransfer`):

| Packet | Bytes |
| --- | --- |
| Talk start | `[0x53][session id: 4, big-endian][mode: 1][name: UTF-8, up to 32]` |
| Talk audio | `[0x54][session id: 4][index: 1][flags: 1][Codec2 frames]` |

- `flags`: bit 7 marks the talk's last packet (which may carry no audio); bits
  0-4 are the VE3 mode (1 = Codec2 1200).
- Audio packet *i* is exactly packet *i* of the talk's VE3 clip: 156 bytes (26
  frames, 1.04 s), fewer in the last.
- After the last packet the talk is posted as
  `VE3:<id>:1:<packets>:<seconds> PTT` on the channel, with the same session
  id, and served like any recording. The `PTT` caption marks it as a talk;
  versions that don't know it show "Voice message: PTT". A talk announced
  without it still shows as a talk on a standalone that heard its packets.

Packets are spaced like a photo transfer: through repeaters at least four
packet airtimes plus 150 ms (800 ms at least), so a packet doesn't talk over
the relays of the one before; direct, one airtime plus 60 ms. At the 250 kHz
MediaMesh preset a packet is about 0.16 s on air, so speech keeps up. At 125
kHz, talks through repeaters fall a little behind the longer they run; use
direct there.

Listeners wait half a second after the first packet before playing, play
packets in order, and skip one that hasn't come 0.4 s after it was needed. A
talk with nothing heard for 3.5 s has ended.

## Code

| What | Where |
| --- | --- |
| Packets and the jitter buffer (host-tested) | `src/ui-touch/WalkieWire.h`, `test/test_walkie.cpp` |
| The card, talking, live playback | `src/ui-touch/WalkieUI.inc` |
| Mic bytes while recording | `VoxMic::liveBits` (`src/ui-touch/VoxMic.cpp`) |
| The Mic key | `tdeckKeyboardMicHeld` (`src/helpers/input/TDeckKeyboard.cpp`), `walkie::keyTick` |
| The trackball's button | `TBH_TALK` in the trackball handler (`UITask.cpp`), `walkie::ballTalks` |
| The waveform (host-tested) | `voxCodec2Waveform` (`src/ui-touch/VoxVoice.cpp`, `test/test_vox_record.cpp`); drawn by `chatTalkWaveDrawCb` |
| Keeping a talk heard in full | `VoxPhotos::storeHeard` |
| From the mesh | `MyMesh::onChannelDataRecv` -> `walkieOnChannelData` |

The golden bytes in `test/test_walkie.cpp` are the ones MeshCore Open TAK's
`test/vox/walkie_wire_test.dart` checks.
