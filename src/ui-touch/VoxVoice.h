// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decodes MeshCore Open TAK voice clips sent with Codec2 (VE3) to 8 kHz 16-bit
// mono PCM, with the same Codec2 the app uses (lib/codec2). Mimi clips (VM1)
// need a 223 MB neural model and can't be played on a radio.
//
// Platform-neutral and host-tested (test/test_vox_voice.cpp). Builds without
// lib/codec2 (Tanmatsu, P4 for now) get a stub that decodes nothing.

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<codec2/codec2.h>)
#define VOX_CODEC2_AVAILABLE 1
#endif
#endif
#ifndef VOX_CODEC2_AVAILABLE
#define VOX_CODEC2_AVAILABLE 0
#endif

constexpr int kVoxVoiceSampleRate = 8000;

// Whether this build can decode a VE3 wire mode (0..6). 700C is compiled out
// of lib/codec2 to save flash; Open TAK sends 1200 by default.
bool voxCodec2CanDecode(int wire_mode);

// Samples a clip of `len` codec bytes decodes to (whole frames only).
size_t voxCodec2Samples(int wire_mode, size_t len);

// Decodes `len` bytes of back-to-back frames into `out` (capacity in samples).
// Returns the number of samples written, or -1 if the mode isn't supported, the
// codec couldn't start, or `out` is too small.
long voxCodec2Decode(int wire_mode, const uint8_t* bits, size_t len, int16_t* out, size_t out_cap);

// A clip's loudness as `n_bars` bars, 0..100 (a voice message's waveform, as
// MeshCore Open TAK draws it: each bar the RMS of its slice of the decoded
// audio, square-rooted against the loudest; all 0 for silence). Decodes frame
// by frame (Codec2's decode stack, no PCM buffer). False if the mode can't be
// decoded or the codec couldn't start.
constexpr int kVoxWaveBars = 40;
bool voxCodec2Waveform(int wire_mode, const uint8_t* bits, size_t len, uint8_t* bars, int n_bars);

// Frame-by-frame decoding, for streaming a clip straight to the speaker.
class VoxCodec2Stream {
 public:
  VoxCodec2Stream() {}
  ~VoxCodec2Stream() { end(); }
  bool begin(int wire_mode);   // false: mode unsupported or no memory
  int bytesPerFrame() const { return bpf_; }
  int samplesPerFrame() const { return spf_; }
  void decode(const uint8_t* frame, int16_t* out);   // bpf_ bytes in, spf_ samples out
  void end();

 private:
  void* c2_ = nullptr;
  int bpf_ = 0;
  int spf_ = 0;
};

// ---- Recording on the radio --------------------------------------------------

// The modes the radio can record in (VE3 wire ids), chosen in Settings > Sound,
// the same three the app offers:
//   1200 (1) MeshCore Open TAK's default: a 10 s clip is 10 packets.
//   1600 (5) clearer, a third more airtime (8 B per 40 ms frame).
//   3200 (6) clearest, 2.7x the airtime of 1200 (8 B per 20 ms frame).
// Every VE3 app can play all three.
constexpr int kVoxRecordWireMode = 1;   // the default
inline bool voxRecordModeOk(int wire_mode) { return wire_mode == 1 || wire_mode == 5 || wire_mode == 6; }

// Longest clip the radio records, as in the app (VoxVoiceService.maxClipLength).
constexpr int kVoxMaxRecordSeconds = 20;

// Frame-by-frame Codec2 encoding of 8 kHz mono PCM.
class VoxCodec2Encoder {
 public:
  VoxCodec2Encoder() {}
  ~VoxCodec2Encoder() { end(); }
  bool begin(int wire_mode);   // false: mode unsupported or no memory
  int bytesPerFrame() const { return bpf_; }
  int samplesPerFrame() const { return spf_; }
  // samplesPerFrame() samples in, bytesPerFrame() bytes out. Codec2 may scale
  // the input in place, so `pcm` is not const.
  void encode(int16_t* pcm, uint8_t* out);
  void end();

 private:
  void* c2_ = nullptr;
  int bpf_ = 0;
  int spf_ = 0;
};

// Turns the microphone's 16 kHz interleaved stereo into Codec2's 8 kHz mono:
// both channels mixed, DC removed, low-passed and decimated by 2, then levelled
// by a slow AGC with a soft limiter so quiet and loud talkers both reach Codec2
// at a level it models well. Stateful across calls; reset() between clips.
class VoxMicPrep {
 public:
  static constexpr int kInputRate = 16000;
  static constexpr int kTaps = 31;   // low-pass FIR, cutoff 3.4 kHz at 16 kHz

  VoxMicPrep() { reset(); }
  void reset();

  // Consumes `frames` stereo frames (L, R, L, R ...) and writes 8 kHz samples
  // to `out` (capacity `cap`). Returns samples written (frames / 2, give or
  // take the odd one carried over).
  size_t process(const int16_t* stereo, size_t frames, int16_t* out, size_t cap);

  // Same, for a mono 16 kHz source.
  size_t processMono(const int16_t* mono, size_t n, int16_t* out, size_t cap);

  // Peak of the levelled output since the last call (0..32767), for a meter.
  int takePeak();
  // The AGC's current gain (1 = none).
  float gain() const { return gain_; }

 private:
  void push(float x, int16_t* out, size_t cap, size_t* n);
  int16_t level(float s);

  float hist_[kTaps];
  int   pos_ = 0;
  bool  odd_ = false;
  float dc_x_ = 0, dc_y_ = 0;
  float env_ = 0;
  float gain_ = 1;
  float gate_ = 1;   // the noise gate's gain (kGateFloor..1)
  int   peak_ = 0;
};

// 8 kHz decoded speech to the speaker's rate (16 kHz on the T-Deck and Pager).
// Each output sample is interpolated between the two 8 kHz samples around it
// and low-passed at 3.6 kHz (Butterworth biquad at the output rate), where the
// old path repeated each sample: that put mirror images of the voice around
// 4-8 kHz, heard as buzz and harshness on top of Codec2's own. A soft limiter
// rounds off the rare overshoot. (As GridDown Secure Messenger plays Codec2.)
class VoxPlayOut {
 public:
  void begin(uint32_t out_rate, float gain);
  void setGain(float g) { gain_ = g; }
  // Pauses: while the decoded voice stays under `open_level` (raw Codec2
  // samples, before the gain) it is turned down 16 dB, as the sender's noise
  // gate does, so Codec2's buzzy rendering of background noise between words
  // fades instead of droning on. 0 (the default) turns it off. For a clip,
  // voxPlayGateLevel(peak) of its peak.
  void setGate(float open_level) { gate_open_ = open_level; }
  // One 8 kHz sample in; the output samples it completes are appended to
  // out[*n] (capacity `cap`).
  void push(int16_t s, int16_t* out, size_t cap, size_t* n);

 private:
  float step_ = 1, rel_ = 1, prev_ = 0, gain_ = 1;
  float gate_open_ = 0, env_ = 0, gate_ = 1;
  bool lpf_ = false;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// The loudest sample in the first `max_samples` of a Codec2 clip once decoded
// (a few seconds is enough to set a playback level), or -1.
int voxCodec2Peak(int wire_mode, const uint8_t* bits, size_t len, size_t max_samples);

// The level to play a clip at so its speech peaks near -3 dBFS: 1 for a clip
// already loud, up to 6x for a quiet one, from its peak (voxCodec2Peak).
float voxPlayNormGain(int peak);

// The level that opens VoxPlayOut's pause gate for a clip peaking at `peak`:
// 36 dB under it (0 = no gate, for an unknown or silent peak).
float voxPlayGateLevel(int peak);
