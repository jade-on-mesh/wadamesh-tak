// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The small AI voice restorer: rebuilds Codec2 voice (8 kHz, buzzy, nothing
// above 4 kHz) as 16 kHz speech, with a network trained by MeshCore Open TAK's
// voice training kit (tools/voice-training, option 4: kit/small.py). The model
// is a file on the SD card, /models/vox-restorer-small.bin, about 1 MB; its
// format and the arithmetic below follow kit/small.py's numpy reference, which
// test/test_restore_small.cpp checks this against.
//
//   8 kHz -> STFT (256, hop 64, Hann) -> |X| -> 32 mel bands -> log
//   -> conv (7 wide) -> LayerNorm -> ConvNeXt blocks (depthwise conv 7,
//      LayerNorm, linear, GELU, linear, scaled, added back)
//   -> LayerNorm -> linear head: log-magnitude (clipped) and phase
//   -> inverse STFT (512, hop 128, Hann, overlap-add) -> 16 kHz
//
// A whole clip at a time (the convolutions look 3 frames ahead at every
// layer). Memory, in PSRAM on a standalone: the weights as float (about 2 MB
// for the kit's default size) while loaded, plus about 80 KB per second of
// voice while a clip runs (160 KB with the player's buffers).
// Platform-neutral; host-tested.

#include <stddef.h>
#include <stdint.h>

class VoxRestoreSmall {
 public:
  VoxRestoreSmall() {}
  ~VoxRestoreSmall() { unload(); }
  VoxRestoreSmall(const VoxRestoreSmall&) = delete;
  VoxRestoreSmall& operator=(const VoxRestoreSmall&) = delete;

  // Parses the model file (the bytes can be freed afterwards). False with
  // `err` set when it isn't one, is damaged, or there's no memory.
  bool load(const uint8_t* file, size_t len, const char** err);
  void unload();
  bool loaded() const { return w_ != nullptr; }

  int dim() const { return dim_; }
  int layers() const { return layers_; }
  // Multiply-adds per second of voice (for a speed estimate).
  uint32_t macsPerSecond() const;

  // Receives the output as it is made: `n` samples at 16 kHz, in order.
  typedef bool (*Sink)(void* ctx, const float* y, size_t n);

  // Restores `n` samples of 8 kHz speech (floats, about -1..1) to 2n samples
  // at 16 kHz, handed to `sink` in pieces. `stop`, when set, ends it early.
  // False if there's no memory, `stop` was set, or `sink` returned false.
  bool run(const float* x8, size_t n, Sink sink, void* ctx, volatile bool* stop = nullptr);

  // Called between pieces of work (every 16 frames a layer), so a long clip
  // can let other tasks run (a standalone: vTaskDelay(1)). None by default.
  void setYield(void (*fn)()) { yield_ = fn; }

 private:
  bool alloc(void** p, size_t bytes);

  void (*yield_)() = nullptr;
  float* w_ = nullptr;   // every weight, as float, in the file's order (pointers below into it)
  int n_mels_ = 0, dim_ = 0, inter_ = 0, layers_ = 0;
  float log_mag_max_ = 5.f;
  const float* fb_ = nullptr;        // [n_mels][129]
  float* embed_w_ = nullptr;         // [dim][k][n_mels] (reordered)
  const float* embed_b_ = nullptr;
  const float* norm_w_ = nullptr;
  const float* norm_b_ = nullptr;
  struct Block {
    const float *dw_w, *dw_b, *ln_w, *ln_b, *w1, *b1, *w2, *b2, *gamma;
  };
  Block* blocks_ = nullptr;
  const float* fln_w_ = nullptr;
  const float* fln_b_ = nullptr;
  const float* out_w_ = nullptr;     // [out_nfft+2][dim]
  const float* out_b_ = nullptr;
};

// Host-side helpers, exposed for the test.
namespace voxrs {
// In-place complex FFT of size n (a power of two); `inverse` without 1/n.
void fft(float* re, float* im, int n, bool inverse);
}  // namespace voxrs
