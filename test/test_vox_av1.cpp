// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decodes real MeshCore Open TAK photo payloads (test/data/vox, encoded with the
// app's own vox_av1.c settings) through the firmware's decoder and checks every
// pixel against libaom's decode of the same payload. Then feeds it damaged and
// hostile input. Build: test/build_vox_av1_test.sh

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui-touch/VoxAv1Core.h"
#include "ui-touch/VoxWire.h"

static uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

static uint8_t* readFile(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)n);
  *len = fread(b, 1, (size_t)n, f);
  fclose(f);
  return b;
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "test/data/vox";
  char path[512];
  snprintf(path, sizeof path, "%s/expected.txt", dir);
  FILE* m = fopen(path, "r");
  assert(m && "missing expected.txt");

  char line[256];
  int checked = 0;
  size_t worst_peak = 0;
  uint8_t* sample = nullptr;
  size_t sample_len = 0;
  while (fgets(line, sizeof line, m)) {
    if (line[0] == '#') continue;
    char name[128];
    int w, h;
    unsigned long long want;
    if (sscanf(line, "%127s %d %d %llx", name, &w, &h, &want) != 4) continue;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len = 0;
    uint8_t* payload = readFile(path, &len);
    assert(payload);

    size_t obu_len = 0;
    const uint8_t* obus = VoxWire::imageObus(payload, len, &obu_len);
    assert(obus);

    VoxAv1Image rgb, px;
    VoxAv1Result r = voxAv1Decode(obus, obu_len, kVoxAv1Rgb888, 768, &rgb);
    assert(r == kVoxAv1Ok);
    assert(rgb.width == w && rgb.height == h);
    const uint64_t got = fnv1a(rgb.pixels, (size_t)w * h * 3);
    printf("%-28s %3dx%-3d %s\n", name, w, h, got == want ? "matches libaom" : "MISMATCH");
    assert(got == want);

    // What the device needs: one RGB565 decode with nothing else held.
    voxAv1Free(rgb.pixels);
    rgb.pixels = nullptr;
    voxAv1HeapResetPeak();
    const size_t before = voxAv1HeapInUse();
    r = voxAv1Decode(obus, obu_len, kVoxAv1Rgb565, 768, &px);
    const size_t peak = voxAv1HeapPeak() - before;
    if (peak > worst_peak) worst_peak = peak;
    assert(r == kVoxAv1Ok && px.width == w && px.height == h);
    r = voxAv1Decode(obus, obu_len, kVoxAv1Rgb888, 768, &rgb);   // again, to compare
    assert(r == kVoxAv1Ok);
    const uint16_t* p16 = (const uint16_t*)px.pixels;
    for (int i = 0; i < w * h; i++) {
      const uint8_t* c = rgb.pixels + i * 3;
      assert(p16[i] == (uint16_t)(((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3)));
    }

    // A picture larger than the caller allows is refused.
    VoxAv1Image small;
    assert(voxAv1Decode(obus, obu_len, kVoxAv1Rgb565, 128, &small) != kVoxAv1Ok);
    assert(small.pixels == nullptr);

    voxAv1Free(rgb.pixels);
    voxAv1Free(px.pixels);
    if (!sample) { sample = payload; sample_len = len; } else free(payload);
    checked++;
  }
  fclose(m);
  assert(checked >= 10);

  // Only the tables the decoder keeps for the next photo stay allocated.
  const size_t resident = voxAv1HeapInUse();
  printf("decoder heap: worst single RGB565 decode %zu bytes (incl. output), resident between photos %zu bytes\n", worst_peak, resident);
  assert(worst_peak < 1536 * 1024);
  assert(resident < 300 * 1024);

  // A version 2 payload: the thumbnail from its first packet alone. (Its
  // photo is chelsea_2750's, checked bit-exact in the list above.)
  {
    snprintf(path, sizeof path, "%s/chelsea_2750_v2.vi1", dir);
    size_t len = 0;
    uint8_t* v2 = readFile(path, &len);
    assert(v2 && len > (size_t)VoxWire::kBytesPerPacket);
    size_t tl = 0;
    const uint8_t* t = VoxWire::imageThumbObus(v2, VoxWire::kBytesPerPacket, &tl);
    assert(t && tl <= (size_t)VoxWire::kBytesPerPacket - 2);
    VoxAv1Image th;
    assert(voxAv1Decode(t, tl, kVoxAv1Rgb565, 256, &th) == kVoxAv1Ok);
    assert(th.width == 64 && th.height > 0 && th.height <= 64);
    printf("thumbnail from the first packet: %dx%d from %zu bytes\n", th.width, th.height, tl);
    voxAv1Free(th.pixels);
    free(v2);
  }
  // The 96 px thumbnail the app sends now: over the first two packets, not
  // readable from the first alone.
  {
    snprintf(path, sizeof path, "%s/chelsea_2750_v2t96.vi1", dir);
    size_t len = 0;
    uint8_t* v2 = readFile(path, &len);
    assert(v2 && len > 2u * VoxWire::kBytesPerPacket);
    size_t tl = 0;
    assert(!VoxWire::imageThumbObus(v2, VoxWire::kBytesPerPacket, &tl));
    const uint8_t* t = VoxWire::imageThumbObus(v2, VoxWire::kThumbPackets * VoxWire::kBytesPerPacket, &tl);
    assert(t && tl <= 255);
    VoxAv1Image th;
    assert(voxAv1Decode(t, tl, kVoxAv1Rgb565, 256, &th) == kVoxAv1Ok);
    assert(th.width == 96 && th.height > 0 && th.height <= 96);
    printf("thumbnail from the first two packets: %dx%d from %zu bytes\n", th.width, th.height, tl);
    voxAv1Free(th.pixels);
    free(v2);
  }

  // Damaged input: every truncation, and bit flips throughout. Must fail or
  // succeed cleanly (ASan/UBSan watch the decoder), never crash or leak.
  size_t obu_len = 0;
  const uint8_t* obus = VoxWire::imageObus(sample, sample_len, &obu_len);
  int failures = 0;
  for (size_t n = 0; n < obu_len; n += 7) {
    VoxAv1Image img;
    if (voxAv1Decode(obus, n, kVoxAv1Rgb565, 768, &img) == kVoxAv1Ok) voxAv1Free(img.pixels);
    else failures++;
  }
  uint8_t* bent = (uint8_t*)malloc(obu_len);
  uint32_t seed = 12345;
  for (int round = 0; round < 300; round++) {
    memcpy(bent, obus, obu_len);
    for (int k = 0; k < 1 + round % 4; k++) {
      seed = seed * 1664525u + 1013904223u;
      bent[(seed >> 8) % obu_len] ^= (uint8_t)(1u << (seed & 7));
    }
    VoxAv1Image img;
    if (voxAv1Decode(bent, obu_len, kVoxAv1Rgb565, 768, &img) == kVoxAv1Ok) voxAv1Free(img.pixels);
  }
  free(bent);
  assert(failures > 0);

  VoxAv1Image img;
  assert(voxAv1Decode(nullptr, 10, kVoxAv1Rgb565, 768, &img) == kVoxAv1BadInput);
  const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
  assert(voxAv1Decode(junk, sizeof junk, kVoxAv1Rgb565, 768, &img) != kVoxAv1Ok);
  assert(voxAv1HeapInUse() == resident);

  free(sample);
  printf("%d photos decoded bit-exact; damaged input handled\n", checked);
  return 0;
}
