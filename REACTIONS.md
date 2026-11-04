# Reactions

Wadamesh TAK reads and sends the same emoji reactions as MeshCore Open (and Open TAK).
A reaction sent from either one shows under the message on both.

## Sending

Long-press a message (or open its menu) and pick **React**. A sheet opens with the
app's emoji list, in the app's order, with its six quick ones first. Pick one and the
reaction goes to that message's chat, just like a normal message, without touching
your draft.

Emoji this radio can't draw are left out of the sheet. The app still shows any you receive.

## Reading

Reactions are shown as a line under the message they're for, e.g. `👍 2  ❤️`. In
compact rows the line goes on the same row instead. Each person counts once per emoji.
Reactions don't appear as messages of their own. They don't make a chat unread, chime,
or move a chat up the list.

MeshCore One reactions (`@[name]🥳` plus an 8-character hash) are recognised too. If
one matches a message, it's stored and shown the same way as the app's.

## On the air

A reaction is a normal text message: `r:HASH:II`.

- `HASH` is MeshCore Open's 16-bit hash of the message's timestamp, the sender's
  name (for channels and rooms; none for direct messages), and the first five
  characters of its text. A channel reply's `@[name]` prefix is left out.
- `II` is the emoji's place in the app's list, in hex.

So a reaction only finds its message if both ends store the sender's own timestamp.
Wadamesh now keeps that timestamp for channel messages as well as DMs, and for the
ones it sends. Messages stored before this update may not match. A reaction to one of
those is kept but isn't shown on it.

`test/test_reactions.cpp` checks the hash, the emoji table and the MeshCore One
parsing against the app's own test values.
