// Codec2 encode + decode of raw 8 kHz s16le audio, for fitting and training
// the voice tools (scripts/voice). "-" reads stdin / writes stdout.
// Build: gcc -O2 -DCODEC2_MODE_700C_EN=0 -DGIT_HASH=\"\" -I lib/codec2/include -I lib/codec2/src c2round.c lib/codec2/src/*.c -lm
// Usage: c2round MODE IN.raw OUT.raw   (MODE: 0 = 3200, 4 = 1600, 5 = 1200)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "codec2.h"

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "c2round MODE IN.raw OUT.raw\n"); return 2; }
  struct CODEC2* c = codec2_create(atoi(argv[1]));
  if (!c) { fprintf(stderr, "unsupported mode\n"); return 2; }
  int ns = codec2_samples_per_frame(c), nb = codec2_bytes_per_frame(c);
  short* buf = malloc(ns * 2);
  unsigned char* bits = malloc(nb);
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  FILE* fi = strcmp(argv[2], "-") ? fopen(argv[2], "rb") : stdin;
  FILE* fo = strcmp(argv[3], "-") ? fopen(argv[3], "wb") : stdout;
  if (!fi || !fo) { fprintf(stderr, "can't open files\n"); return 1; }
  while (fread(buf, 2, ns, fi) == (size_t)ns) {
    codec2_encode(c, bits, buf);
    codec2_decode(c, buf, bits);
    fwrite(buf, 2, ns, fo);
  }
  fflush(fo);
  codec2_destroy(c);
  return 0;
}
