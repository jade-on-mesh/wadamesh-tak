# Sound link

Share a channel or your contact, or chat, by sound: one radio plays a short
burst of tones and another radio, or a phone running MeshCore Open TAK, hears
it through its microphone. Messages use the format of
[Wavest](https://github.com/bennjordan/Wavest) (Benn Jordan's ggwave
messenger), so Wavest's web page joins in too. No pairing, no network, no
camera: anything in the same room that can hear the speaker.

**Radios:** LilyGo T-Deck / T-Deck Plus and T-Display P4 (the boards with a
microphone). App drawer › **Sound link**.

## Using it

The **Link** tab:

- **Start / Stop listening.** The meter shows the mic level. Listening keeps
  going with the page closed (heard messages pop up), and survives a reboot.
- **Send a message.** Plays a Wavest message from your callsign.
- **Play my contact.** Plays your contact card, the same link as the
  share-my-contact QR. Whoever hears it gets a **Tap to add** card.
- **Play a channel.** Pick one of your channels (or long-press a channel in
  Chats › **Play by sound**). Whoever hears it gets a
  **Tap to join** card. Anyone in earshot can join, so mind where you play a
  private channel.
- Below that, what was heard, newest first. Tap a channel or contact card to
  add it.

**Checking a one-time pad.** OTP Messenger's pad screen shows the pad's check
code. **Play the check code** on one device and **Listen for a check code** on
the other (or just read the codes out). The listener says whether its pad is
the same. The code reveals nothing about the pad; the pad itself never goes
over sound, because anyone in earshot could record it.

In MeshCore Open TAK the same links play from the channel share screen
(**Play by sound**), and its QR scanners have a **Listen** button.

The **Settings** tab:

| Setting | What it does |
|---|---|
| Callsign | The name on your messages (empty: the radio's name) |
| Passkey | Encrypts your messages and opens others' the way Wavest does |
| Speed | Normal, Fast (Wavest's default) or Fastest. Slower carries further and copes better with noise; every radio hears all three |
| Loudness | How loud the tones are. Settings › Sound still sets the speaker volume, and muted means no sound link |
| Bridge channel | A private or hashtag channel (never Public) |
| Post what is heard there | Readable messages heard by sound go to the channel as `[Wavest] sender: text` |
| Play its new messages aloud | The channel's incoming messages are played as Wavest messages from their sender |

With both bridge options on, a Wavest page in the room and the mesh channel
talk to each other through the radio. Bridge posts start with `[Wavest]` and
are never played back, so two bridges can't feed each other in a loop.

**About the passkey.** Wavest's key is SHA-256 of the passkey with no salt,
and its XOR fallback (which this radio can read but never sends) is no
protection at all. It keeps a chat from people who only overhear it. It is
not the mesh's encryption: never reuse a channel secret or a password here.

## What goes over the air

One ggwave message carries up to 140 bytes:

```
U:<callsign>|<text>                      plain
E:<callsign>|AES:<base64(iv12|ct|tag16)>  AES-256-GCM, key = SHA-256(passkey)
E:<callsign>|XOR:<base64>                 Wavest's fallback (read only)
meshcore://channel/add?name=<name>&secret=<32 hex>
meshcore://contact/add?name=<name>&public_key=<64 hex>&type=1
meshcore://team/join?n=<team channel>&s=<32 hex>&...   a team setup (TEAM_SETUP.md)
MCT:<id>:<i>/<n>:<piece>                                 one part of a longer team setup
```

The audible protocols use tones from about 1.9 to 6.4 kHz. Ultrasound needs a
phone's 48 kHz audio, so the radios neither send nor hear it.

## How it works on the radio

- [ggwave](https://github.com/ggerganov/ggwave) (Georgi Gerganov, MIT) is in
  `lib/ggwave`, with four small changes listed in its README.
- ggwave runs at 24 kHz with 512-sample frames. That is the same 46.875 Hz
  tone spacing as the 48 kHz / 1024 the app and Wavest use, so they hear each
  other. The mic (and the P4's speaker) run at 16 kHz.
  `soundlink::Resampler` converts, because ggwave's own resampler costs about
  80 times its decoder, too much for an ESP32-S3 in real time.
- The mic is read by a small task of its own into a 4-second ring in PSRAM, and ggwave decodes from the ring. ggwave's work at the end of a message takes about 2 seconds on a T-Deck. Before the ring, nothing read the mic during that time, so whatever followed was lost, such as the next part of a team setup.
- Of the T-Deck's two mic inputs, Sound link takes the louder one (DC removed, switching only at twice the level). Voice messages mix the two. Mixed, two mics a few centimetres apart cancel at some of ggwave's pitches, and an unused input only adds noise. On a T-Deck, messages were heard starting but never decoded.
- Listening and playing take turns. The loop task stops the mic, plays, waits
  a moment, then listens again. Decoding and playing run on the audio worker
  that voice messages use.
- ggwave needs about 1.4 MB of PSRAM while listening and about 1.7 MB while
  playing, freed in between. It needs no internal RAM beyond the mic's I2S
  driver.
- Voice messages and Sound link share the mic. Recording asks you to stop
  listening first. On the P4, which has one audio lock for the mic and the
  speaker, voice clips also wait until listening stops.

Code: `src/ui-touch/SoundLinkCore.h` (format, links, resampler; host-tested
by `test/build_sound_link_test.sh`), `src/ui-touch/SoundLinkUI.inc` (the page,
the engine, the bridge), and `VoxMic::raw*` (the capture).
