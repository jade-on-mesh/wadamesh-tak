// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decodes the AV1 still pictures MeshCore Open TAK sends as photo messages
// (one key frame, 8-bit 4:2:0, reduced still-picture header, raw OBUs with the
// temporal delimiter stripped) and converts them exactly the way the app's
// vox_av1.c does, so the radio shows the same pixels the sender previewed.
//
// Synchronous and platform-neutral; the device runs it on its own task (see
// VoxAv1Job.h). Built on the vendored dav1d (lib/dav1d; the P4 builds take it
// as tdisplay_p4/components/dav1d). Builds without dav1d compile a stub that
// reports kVoxAv1Unsupported.

#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include(<dav1d/dav1d.h>)
#define VOX_AV1_AVAILABLE 1
#endif
#endif
#ifndef VOX_AV1_AVAILABLE
#define VOX_AV1_AVAILABLE 0
#endif

enum VoxAv1Result {
  kVoxAv1Ok = 0,
  kVoxAv1Unsupported,   // no decoder in this build
  kVoxAv1BadInput,      // not a picture we can read
  kVoxAv1TooLarge,      // larger than the caller's limit
  kVoxAv1NoMemory,      // a PSRAM allocation failed
  kVoxAv1DecodeFailed,
  kVoxAv1NoStack,       // no internal RAM for the decode task's stack (VoxAv1Job)
  kVoxAv1OpenFailed,    // dav1d_open failed
};

enum VoxAv1Format {
  kVoxAv1Rgb565 = 0,    // uint16_t per pixel, LVGL TRUE_COLOR with LV_COLOR_16_SWAP 0
  kVoxAv1Rgb888 = 1,    // 3 bytes per pixel, R G B (tests compare against libaom with this)
};

struct VoxAv1Image {
  int width = 0;
  int height = 0;
  VoxAv1Format format = kVoxAv1Rgb565;
  uint8_t* pixels = nullptr;   // from voxAv1Alloc(); release with voxAv1Free()
};

// Decodes `obus` (the payload after its 0x01 version byte). On kVoxAv1Ok,
// `out->pixels` holds width*height pixels in `format`. Pictures wider or taller
// than `max_side` are refused before any pixel buffer is allocated.
VoxAv1Result voxAv1Decode(const uint8_t* obus, size_t len, VoxAv1Format format,
                          int max_side, VoxAv1Image* out);

// Same allocator the decoder uses for everything (PSRAM on the device).
void* voxAv1Alloc(size_t bytes);
void voxAv1Free(void* p);

const char* voxAv1ResultText(VoxAv1Result r);

// Bytes the decoder currently holds (context, frame buffers, tables) and the
// most it has held since boot. For on-device diagnostics and tests.
size_t voxAv1HeapInUse();
size_t voxAv1HeapPeak();
void voxAv1HeapResetPeak();
