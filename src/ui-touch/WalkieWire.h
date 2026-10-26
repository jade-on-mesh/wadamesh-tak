// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Push-to-talk ("walkie-talkie") on MediaMesh, shared with MeshCore Open TAK
// (lib/vox/walkie_wire.dart). A talk is a Codec2 1200 voice clip sent while
// it's spoken: each second goes out on the channel as soon as it's encoded,
// and listeners play it as it arrives. Afterwards the talk is announced as an
// ordinary VE3 voice message with the same session id, so it stays in the
// chat and anyone who missed it can fetch it.
//
// GRP_DATA type 0x5643 (Open TAK's channel photos and voice, so MediaMesh
// firmware sends it on radio2 only):
//
//   talk start  [0x53 'S'][sid:4 BE][mode:1][name: UTF-8, the rest, <= 32 B]
//   talk audio  [0x54 'T'][sid:4 BE][index:1][flags:1][Codec2 frames]
//               flags bit 7: the talk's last packet (it may hold no audio)
//               flags bits 0-4: the VE3 mode id (1 = Codec2 1200)
//
// Audio packet i is exactly VE3 packet i of the clip: packetBytes(mode)
// (156), fewer in the last. Pure C++ and host-tested (test/test_walkie.cpp).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "VoxWire.h"

namespace WalkieWire {

constexpr uint8_t  kStartMagic     = 0x53;   // 'S'
constexpr uint8_t  kAudioMagic     = 0x54;   // 'T'
constexpr uint8_t  kLastFlag       = 0x80;
constexpr uint8_t  kModeMask       = 0x1F;
constexpr int      kAudioHeaderLen = 7;
constexpr int      kStartHeaderLen = 6;
constexpr int      kMaxNameBytes   = 32;
constexpr int      kMode           = 1;        // Codec2 1200: 26 frames, 1.04 s a packet
constexpr uint32_t kMaxTalkMs      = 20000;    // as long as a recorded voice message
constexpr uint32_t kTalkTimeoutMs  = 3500;     // nothing heard this long: the talk has ended
constexpr uint32_t kMinTalkMs      = 600;      // shorter: not announced as a message
constexpr uint32_t kStartDelayMs   = 500;      // listeners wait this long after the first packet
constexpr uint32_t kSkipGraceMs    = 400;      // a missing packet may come this late
constexpr int      kMaxPacketsTalk = (int)(kMaxTalkMs / 1040) + 2;

inline bool isLive(const uint8_t* p, size_t n) {
  return p && n > 0 && (p[0] == kStartMagic || p[0] == kAudioMagic);
}

inline bool codec2Mode(int mode) { return mode >= 0 && mode <= 6; }

struct Start {
  uint32_t sid = 0;
  uint8_t  mode = kMode;
  char     name[kMaxNameBytes + 1] = {0};
};

inline bool parseStart(const uint8_t* p, size_t n, Start* out) {
  if (!p || !out || n < (size_t)kStartHeaderLen || p[0] != kStartMagic) return false;
  if (!codec2Mode(p[5])) return false;
  out->sid = VoxWire::readSid(p + 1);
  out->mode = p[5];
  size_t len = n - kStartHeaderLen;
  if (len > (size_t)kMaxNameBytes) len = kMaxNameBytes;
  memcpy(out->name, p + kStartHeaderLen, len);
  out->name[len] = 0;
  return true;
}

// Cuts `name` to whole UTF-8 characters within kMaxNameBytes. Returns bytes written.
inline int encodeStart(uint32_t sid, uint8_t mode, const char* name, uint8_t* out, size_t cap) {
  size_t len = name ? strlen(name) : 0;
  if (len > (size_t)kMaxNameBytes) {
    len = kMaxNameBytes;
    while (len > 0 && ((uint8_t)name[len] & 0xC0) == 0x80) len--;   // don't split a character
  }
  if (!out || cap < (size_t)kStartHeaderLen + len) return 0;
  out[0] = kStartMagic;
  VoxWire::writeSid(sid, out + 1);
  out[5] = mode;
  if (len) memcpy(out + kStartHeaderLen, name, len);
  return kStartHeaderLen + (int)len;
}

struct Audio {
  uint32_t       sid = 0;
  uint8_t        index = 0;
  uint8_t        mode = kMode;
  bool           last = false;
  const uint8_t* data = nullptr;   // into the packet
  int            len = 0;
  // Packets in the talk, once this (its last) has arrived.
  int totalIfLast() const { return len > 0 ? index + 1 : index; }
};

inline bool parseAudio(const uint8_t* p, size_t n, Audio* out) {
  if (!p || !out || n < (size_t)kAudioHeaderLen || p[0] != kAudioMagic) return false;
  const int mode = p[6] & kModeMask;
  if (!codec2Mode(mode) || p[5] >= VoxWire::kMaxPackets) return false;
  const int len = (int)n - kAudioHeaderLen;
  const int bpf = VoxWire::bytesPerFrame(mode);
  if (bpf <= 0 || len % bpf != 0 || len > VoxWire::packetBytes(mode)) return false;
  out->sid = VoxWire::readSid(p + 1);
  out->index = p[5];
  out->mode = (uint8_t)mode;
  out->last = (p[6] & kLastFlag) != 0;
  out->data = p + kAudioHeaderLen;
  out->len = len;
  return true;
}

inline int encodeAudio(uint32_t sid, uint8_t index, uint8_t mode, bool last, const uint8_t* data, int len,
                       uint8_t* out, size_t cap) {
  if (!out || len < 0 || cap < (size_t)(kAudioHeaderLen + len)) return 0;
  out[0] = kAudioMagic;
  VoxWire::writeSid(sid, out + 1);
  out[5] = index;
  out[6] = (uint8_t)((mode & kModeMask) | (last ? kLastFlag : 0));
  if (len) memcpy(out + kAudioHeaderLen, data, (size_t)len);
  return kAudioHeaderLen + len;
}

// ---- DMs ------------------------------------------------------------------------
// Push-to-talk in a DM goes to the one contact, as raw data along the route to
// them (zero-hop with none), like a DM voice message's packets. VE3-framed, so
// companions send it on radio2:
//
//   [0x56][sid ^ 0xD7A1C0DE : 4 BE][index][flags][to: 4][from: 4][Codec2 frames]
//
// flags as a channel talk's; `to` and `from` are the first 4 bytes of the
// receiver's and the sender's public keys. Taken only when `to` is our key and
// `from` a contact. Older radios see a VE3 packet of a clip nobody announced
// (the sid is scrambled) and drop it. At the end the talk is sent to them as a
// VE3 voice message (a DM) with the real sid. Shared with MeshCore Open TAK
// (lib/vox/walkie_wire.dart WalkieDmAudio).
constexpr uint32_t kDmSidXor    = 0xD7A1C0DEu;
constexpr int      kDmHeaderLen = 15;
constexpr int      kDmKeyBytes  = 4;

struct DmAudio {
  Audio   audio;              // sid unscrambled; data points into the packet
  uint8_t to[kDmKeyBytes];
  uint8_t from[kDmKeyBytes];
};

// A DM talk packet addressed to `self` (our public key), or false.
inline bool parseDmAudio(const uint8_t* p, size_t n, const uint8_t* self, DmAudio* out) {
  if (!p || !out || !self || n < (size_t)kDmHeaderLen || p[0] != VoxWire::kPacketMagic) return false;
  if (memcmp(p + 7, self, kDmKeyBytes) != 0) return false;
  const int mode = p[6] & kModeMask;
  if (!codec2Mode(mode) || p[5] >= VoxWire::kMaxPackets) return false;
  const int len = (int)n - kDmHeaderLen;
  const int bpf = VoxWire::bytesPerFrame(mode);
  if (bpf <= 0 || len % bpf != 0 || len > VoxWire::packetBytes(mode)) return false;
  out->audio.sid = VoxWire::readSid(p + 1) ^ kDmSidXor;
  out->audio.index = p[5];
  out->audio.mode = (uint8_t)mode;
  out->audio.last = (p[6] & kLastFlag) != 0;
  out->audio.data = p + kDmHeaderLen;
  out->audio.len = len;
  memcpy(out->to, p + 7, kDmKeyBytes);
  memcpy(out->from, p + 11, kDmKeyBytes);
  return true;
}

inline int encodeDmAudio(uint32_t sid, uint8_t index, uint8_t mode, bool last, const uint8_t* to,
                         const uint8_t* from, const uint8_t* data, int len, uint8_t* out, size_t cap) {
  if (!out || !to || !from || len < 0 || cap < (size_t)(kDmHeaderLen + len)) return 0;
  out[0] = VoxWire::kPacketMagic;
  VoxWire::writeSid(sid ^ kDmSidXor, out + 1);
  out[5] = index;
  out[6] = (uint8_t)((mode & kModeMask) | (last ? kLastFlag : 0));
  memcpy(out + 7, to, kDmKeyBytes);
  memcpy(out + 11, from, kDmKeyBytes);
  if (len) memcpy(out + kDmHeaderLen, data, (size_t)len);
  return kDmHeaderLen + len;
}

// Decides when each packet of a talk being heard is played, as the app's
// WalkieJitter does: waits a little after the first, plays packets in order,
// and skips one that hasn't come by the time it's needed. Holds the packets
// itself (a talk is at most kMaxPacketsTalk of them). Times are millis().
class Jitter {
 public:
  // In place (no temporary: it's ~4 KB).
  void reset() {
    memset(slots_, 0, sizeof slots_);
    first_ = last_heard_ = play_end_ = 0;
    have_first_ = have_end_ = started_ = false;
    next_ = 0;
    total_ = -1;
  }

  void add(const Audio& a, uint32_t now) {
    if (!have_first_) { first_ = now; have_first_ = true; }
    last_heard_ = now;
    if (a.last) total_ = a.totalIfLast();
    if (a.len <= 0 || a.index < next_ || a.index >= kSlots) return;
    Slot& s = slots_[a.index];
    memcpy(s.data, a.data, (size_t)a.len);
    s.len = (uint8_t)a.len;
    s.full = true;
  }

  bool started() const { return started_; }
  int nextIndex() const { return next_; }
  int total() const { return total_; }

  // The next packet to play now, or -1 when none is due yet. `data`/`len`
  // stay valid until the next call.
  int due(uint32_t now, const uint8_t** data, int* len) {
    if (!have_first_) return -1;
    if (!started_) {
      const bool ready = count() >= 2 || total_ >= 0 || now - first_ >= kStartDelayMs;
      if (!ready) return -1;
      started_ = true;
      if (!slots_[0].full) {   // packet 0 lost: start at the first one heard
        for (int i = 1; i < kSlots; i++) if (slots_[i].full) { next_ = i; break; }
      }
    }
    for (;;) {
      if (total_ >= 0 && next_ >= total_) return -1;
      if (next_ >= kSlots) return -1;
      Slot& s = slots_[next_];
      if (s.full) {
        const uint32_t from = have_end_ && (int32_t)(play_end_ - now) > 0 ? play_end_ : now;
        play_end_ = from + (uint32_t)(s.len / VoxWire::bytesPerFrame(kMode)) * 40u;
        have_end_ = true;
        s.full = false;
        *data = s.data;
        *len = s.len;
        return next_++;
      }
      const uint32_t end = have_end_ ? play_end_ : now;
      const bool late = (int32_t)(now - (end + kSkipGraceMs)) >= 0;
      bool later = total_ >= 0;
      for (int i = next_ + 1; i < kSlots && !later; i++) later = slots_[i].full;
      const bool quiet = now - last_heard_ > kTalkTimeoutMs;
      if (late && (later || quiet)) { next_++; continue; }
      return -1;
    }
  }

  // Played to its end, or gone quiet for good.
  bool finished(uint32_t now) const {
    if (total_ >= 0 && next_ >= total_) return true;
    if (!have_first_) return false;
    const bool quiet = now - last_heard_ > kTalkTimeoutMs;
    const uint32_t end = have_end_ ? play_end_ : last_heard_;
    return quiet && count() == 0 && (int32_t)(now - end) >= 0;
  }

 private:
  static constexpr int kSlots = kMaxPacketsTalk + 2;
  struct Slot { uint8_t data[VoxWire::kBytesPerPacket]; uint8_t len; bool full; };
  int count() const {
    int n = 0;
    for (int i = 0; i < kSlots; i++) n += slots_[i].full ? 1 : 0;
    return n;
  }
  Slot slots_[kSlots] = {};
  uint32_t first_ = 0, last_heard_ = 0, play_end_ = 0;
  bool have_first_ = false, have_end_ = false, started_ = false;
  int next_ = 0;
  int total_ = -1;
};

}  // namespace WalkieWire

// The touch UI's hook (WalkieUI.inc), called from MyMesh::onChannelDataRecv
// for GRP_DATA 0x5643 on one of our channels.
void walkieOnChannelData(int channel_idx, const uint8_t* data, size_t len);
// From MyMesh::onRawDataRecv: true when the packet was a DM talk to us.
bool walkieOnRawData(const uint8_t* data, size_t len);
// Opens a DM's walkie-talkie (the contact's full public key).
void walkieOpenDm(const uint8_t pub[32]);
