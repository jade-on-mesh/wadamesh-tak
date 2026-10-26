// SPDX-License-Identifier: GPL-3.0-or-later
#include "VoxAv1Core.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

const char* voxAv1ResultText(VoxAv1Result r) {
  switch (r) {
    case kVoxAv1Ok:           return "ok";
    case kVoxAv1Unsupported:  return "Photos aren't supported on this device yet";
    case kVoxAv1BadInput:     return "Not a photo this firmware can read";
    case kVoxAv1TooLarge:     return "Photo is too large";
    case kVoxAv1NoMemory:     return "Not enough memory for the photo";
    case kVoxAv1DecodeFailed: return "Photo data is damaged";
    case kVoxAv1NoStack:      return "Not enough internal RAM to start the decoder";
    case kVoxAv1OpenFailed:   return "The photo decoder failed to start";
  }
  return "error";
}

// ------------------------------------------------------------------ memory
//
// Every block carries a 32-byte header recording its size and the offset back
// to the raw allocation, so plain and aligned blocks share one free path and
// the decoder's footprint can be counted. On the device the blocks come from
// PSRAM, falling back to internal RAM only if PSRAM is exhausted.

namespace {
constexpr size_t kHdr = 32;
struct Hdr { size_t size; size_t back; };
// Atomic: the decode task allocates while the UI task may free a finished picture.
std::atomic<size_t> g_in_use{0};
std::atomic<size_t> g_peak{0};

void* rawAlloc(size_t n) {
#ifdef ESP_PLATFORM
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
#else
  return malloc(n);
#endif
}
void rawFree(void* p) {
#ifdef ESP_PLATFORM
  heap_caps_free(p);
#else
  free(p);
#endif
}

void* allocAligned(size_t size, size_t align) {
  if (align < kHdr) align = kHdr;
  if (size > (size_t)64 * 1024 * 1024) return nullptr;
  uint8_t* raw = (uint8_t*)rawAlloc(size + align + kHdr);
  if (!raw) return nullptr;
  uintptr_t user = ((uintptr_t)raw + kHdr + align - 1) & ~(uintptr_t)(align - 1);
  Hdr* h = (Hdr*)(user - sizeof(Hdr));
  h->size = size;
  h->back = user - (uintptr_t)raw;
  const size_t now = g_in_use.fetch_add(size) + size;
  size_t peak = g_peak.load();
  while (now > peak && !g_peak.compare_exchange_weak(peak, now)) {}
  return (void*)user;
}

void freeAny(void* p) {
  if (!p) return;
  Hdr* h = (Hdr*)((uintptr_t)p - sizeof(Hdr));
  g_in_use.fetch_sub(h->size);
  rawFree((uint8_t*)p - h->back);
}

size_t sizeOf(void* p) { return ((Hdr*)((uintptr_t)p - sizeof(Hdr)))->size; }
}  // namespace

void* voxAv1Alloc(size_t bytes) { return allocAligned(bytes, kHdr); }
void voxAv1Free(void* p) { freeAny(p); }
size_t voxAv1HeapInUse() { return g_in_use.load(); }
size_t voxAv1HeapPeak() { return g_peak.load(); }
void voxAv1HeapResetPeak() { g_peak.store(g_in_use.load()); }

// dav1d's allocation hooks (DAV1D_EXTERNAL_ALLOC, lib/dav1d/patches).
extern "C" {
void* dav1d_ext_malloc(size_t sz) { return allocAligned(sz, kHdr); }
void dav1d_ext_free(void* p) { freeAny(p); }
void* dav1d_ext_alloc_aligned(size_t sz, size_t align) { return allocAligned(sz, align); }
void dav1d_ext_free_aligned(void* p) { freeAny(p); }
void* dav1d_ext_realloc(void* p, size_t sz) {
  if (!p) return allocAligned(sz, kHdr);
  if (!sz) { freeAny(p); return nullptr; }
  void* q = allocAligned(sz, kHdr);
  if (!q) return nullptr;
  const size_t old = sizeOf(p);
  memcpy(q, p, old < sz ? old : sz);
  freeAny(p);
  return q;
}
}

#if VOX_AV1_AVAILABLE

#include <dav1d/dav1d.h>

extern "C" int dav1d_ext_alloc_wedge_tables(void);
extern "C" int dav1d_ext_alloc_scratch(void);
extern "C" int dav1d_ext_alloc_qm_tables(void);
extern "C" int dav1d_ext_alloc_intra_edge_tree(void);
extern "C" int dav1d_ext_alloc_scan_tables(void);

namespace {

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Bit-for-bit the conversion in meshcore-open-tak's vox_av1.c i420_to_rgb:
// full-range BT.601, centre-sited 4:2:0 chroma upsampled bilinearly, float math.
// 4:2:0 to RGB, bilinear chroma, exactly as the app's i420_to_rgb. Chroma
// sits between luma samples, so every weight is 1/4 or 3/4 and the blend is an
// integer in 1/16ths: s/16 is the float the app gets, bit for bit (every step
// of its float sum is exact). The rows are blended once per output row, not
// once per pixel; only the colour matrix is float, as the app's is. False if
// the two row buffers can't be had.
bool toRgb(const Dav1dPicture& p, VoxAv1Format fmt, uint8_t* out) {
  const int w = p.p.w, h = p.p.h, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const uint8_t* Yp = (const uint8_t*)p.data[0];
  const uint8_t* Up = (const uint8_t*)p.data[1];
  const uint8_t* Vp = (const uint8_t*)p.data[2];
  const ptrdiff_t ys = p.stride[0], cs = p.stride[1];
  uint16_t* rows = (uint16_t*)voxAv1Alloc((size_t)cw * 2 * sizeof(uint16_t));
  if (!rows) return false;
  uint16_t* cu = rows;        // a row of U blended vertically, in 1/4ths
  uint16_t* cv = rows + cw;   // and of V
  uint16_t* o16 = (uint16_t*)out;
  uint8_t* o24 = out;
  for (int y = 0; y < h; y++) {
    // fy = (y - 0.5) / 2: an odd row is 1/4 past chroma row y/2, an even one
    // 3/4 past the row before (clamped at the edges).
    const int y0 = (y & 1) ? (y >> 1) : (y >> 1) - 1;
    const int wy = (y & 1) ? 1 : 3;
    const int ya = y0 < 0 ? 0 : y0, yb = y0 + 1 >= ch ? ch - 1 : y0 + 1;
    const uint8_t* ua = Up + ya * cs; const uint8_t* ub = Up + yb * cs;
    const uint8_t* va = Vp + ya * cs; const uint8_t* vb = Vp + yb * cs;
    for (int i = 0; i < cw; i++) {
      cu[i] = (uint16_t)((4 - wy) * ua[i] + wy * ub[i]);
      cv[i] = (uint16_t)((4 - wy) * va[i] + wy * vb[i]);
    }
    const uint8_t* yr = Yp + y * ys;
    for (int x = 0; x < w; x++) {
      const int x0 = (x & 1) ? (x >> 1) : (x >> 1) - 1;
      const int wx = (x & 1) ? 1 : 3;
      const int xa = x0 < 0 ? 0 : x0, xb = x0 + 1 >= cw ? cw - 1 : x0 + 1;
      const float u = (float)((4 - wx) * cu[xa] + wx * cu[xb]) * (1.0f / 16) - 128;
      const float v = (float)((4 - wx) * cv[xa] + wx * cv[xb]) * (1.0f / 16) - 128;
      const float Y = yr[x];
      const uint8_t r = clamp8((int)(Y + 1.402f * v + 0.5f));
      const uint8_t g = clamp8((int)(Y - 0.344136f * u - 0.714136f * v + 0.5f));
      const uint8_t b = clamp8((int)(Y + 1.772f * u + 0.5f));
      if (fmt == kVoxAv1Rgb888) {
        o24[0] = r; o24[1] = g; o24[2] = b;
        o24 += 3;
      } else {
        *o16++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }
  }
  voxAv1Free(rows);
  return true;
}

void noFree(const uint8_t*, void*) {}

}  // namespace

VoxAv1Result voxAv1Decode(const uint8_t* obus, size_t len, VoxAv1Format format,
                          int max_side, VoxAv1Image* out) {
  if (!obus || len < 1 || len > 256 * 1024 || !out) return kVoxAv1BadInput;
  *out = VoxAv1Image{};

  // Allocated by the first photo and kept for the next: the tables upstream keeps
  // in .bss, and the 40 KB arena that keeps dav1d's big DSP buffers off the stack.
  if (!dav1d_ext_alloc_scratch() || !dav1d_ext_alloc_wedge_tables() || !dav1d_ext_alloc_qm_tables() ||
      !dav1d_ext_alloc_intra_edge_tree() || !dav1d_ext_alloc_scan_tables())
    return kVoxAv1NoMemory;

  // The sender strips the 2-byte temporal delimiter; dav1d wants it back.
  uint8_t* tu = (uint8_t*)voxAv1Alloc(len + 2);
  if (!tu) return kVoxAv1NoMemory;
  tu[0] = 0x12;
  tu[1] = 0x00;
  memcpy(tu + 2, obus, len);

  Dav1dSettings s;
  dav1d_default_settings(&s);
  s.n_threads = 1;
  s.max_frame_delay = 1;
  s.apply_grain = 0;
  s.all_layers = 0;
  s.frame_size_limit = (unsigned)max_side * (unsigned)max_side;

  Dav1dContext* c = nullptr;
  if (dav1d_open(&c, &s) < 0) { voxAv1Free(tu); return kVoxAv1OpenFailed; }

  VoxAv1Result res = kVoxAv1DecodeFailed;
  Dav1dData d;
  Dav1dPicture pic;
  memset(&pic, 0, sizeof pic);
  if (dav1d_data_wrap(&d, tu, len + 2, noFree, nullptr) == 0) {
    int r = dav1d_send_data(c, &d);
    if (r == 0 || r == DAV1D_ERR(EAGAIN)) {
      r = dav1d_get_picture(c, &pic);
      if (r == DAV1D_ERR(EAGAIN)) r = dav1d_get_picture(c, &pic);
      if (r == 0) {
        if (pic.p.bpc != 8 || pic.p.layout != DAV1D_PIXEL_LAYOUT_I420) {
          res = kVoxAv1BadInput;
        } else if (pic.p.w > max_side || pic.p.h > max_side) {
          res = kVoxAv1TooLarge;
        } else {
          const size_t bpp = format == kVoxAv1Rgb888 ? 3 : 2;
          uint8_t* px = (uint8_t*)voxAv1Alloc((size_t)pic.p.w * pic.p.h * bpp);
          if (!px) {
            res = kVoxAv1NoMemory;
          } else if (!toRgb(pic, format, px)) {
            voxAv1Free(px);
            res = kVoxAv1NoMemory;
          } else {
            out->width = pic.p.w;
            out->height = pic.p.h;
            out->format = format;
            out->pixels = px;
            res = kVoxAv1Ok;
          }
        }
        dav1d_picture_unref(&pic);
      } else if (r == DAV1D_ERR(ENOMEM)) {
        res = kVoxAv1NoMemory;
      }
    } else if (r == DAV1D_ERR(ENOMEM)) {
      res = kVoxAv1NoMemory;
    }
    dav1d_data_unref(&d);
  }
  dav1d_close(&c);
  voxAv1Free(tu);
  return res;
}

#else  // !VOX_AV1_AVAILABLE

VoxAv1Result voxAv1Decode(const uint8_t*, size_t, VoxAv1Format, int, VoxAv1Image* out) {
  if (out) *out = VoxAv1Image{};
  return kVoxAv1Unsupported;
}

#endif
