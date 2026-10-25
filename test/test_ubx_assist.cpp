// Host test: src/helpers/UbxAssist.h (u-blox start-up aiding frames).
//   g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I src -o /tmp/t test/test_ubx_assist.cpp
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "helpers/UbxAssist.h"

using namespace ubxassist;

static bool checksumOk(const Frame& f) {
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i + 2 < f.size; i++) { a += f.bytes[i]; b += a; }
  return f.bytes[f.size - 2] == a && f.bytes[f.size - 1] == b;
}

int main() {
  // The frames the provider already sends, rebuilt here: UBX-CFG-RST GNSS stop
  // and start (WadaNmeaLocationProvider::ubxGnss), checksums included.
  {
    const uint8_t stop[] = {0x00, 0x00, 0x08, 0x00};
    const Frame f = frame(0x06, 0x04, stop, sizeof stop);
    const uint8_t want[] = {0xB5, 0x62, 0x06, 0x04, 0x04, 0x00, 0x00, 0x00, 0x08, 0x00, 0x16, 0x74};
    assert(f.size == sizeof want && !memcmp(f.bytes, want, sizeof want));
  }
  {
    const Frame f = enableAutonomous();
    assert(f.size == 17 && f.bytes[2] == 0x06 && f.bytes[3] == 0x8A && checksumOk(f));
    assert(f.bytes[7] == 0x03);   // RAM | BBR, never flash
  }
  // Time: 2026-10-24 21:05:07 UTC.
  {
    const uint32_t epoch = 1792875907u;
    int y; unsigned mo, d;
    civil(epoch / 86400u, &y, &mo, &d);
    assert(y == 2026 && mo == 10 && d == 24);
    const Frame f = timeUtc(epoch);
    assert(f.size == 32 && checksumOk(f));
    const uint8_t* p = f.bytes + 6;
    assert(p[0] == 0x10 && p[3] == 0x80);                    // TIME_UTC, leap seconds unknown
    assert((p[4] | (p[5] << 8)) == 2026 && p[6] == 10 && p[7] == 24);
    assert(p[8] == 21 && p[9] == 5 && p[10] == 7);
    assert((p[16] | (p[17] << 8)) == kTimeAccuracyS);
  }
  // Leap day and year ends.
  {
    int y; unsigned mo, d;
    civil(19782, &y, &mo, &d); assert(y == 2024 && mo == 2 && d == 29);
    civil(20088, &y, &mo, &d); assert(y == 2024 && mo == 12 && d == 31);
  }
  assert(timeUtc(0).size == 0 && timeUtc(1600000000u).size == 0);   // clock never set
  // Position: Seattle.
  {
    const Frame f = positionLlh(47.6062, -122.3321);
    assert(f.size == 28 && checksumOk(f));
    const uint8_t* p = f.bytes + 6;
    int32_t lat, lon; uint32_t acc;
    memcpy(&lat, p + 4, 4); memcpy(&lon, p + 8, 4); memcpy(&acc, p + 16, 4);
    assert(p[0] == 0x01 && lat == 476062000 && lon == -1223321000 && acc == kPositionAccuracyCm);
  }
  assert(positionLlh(0, 0).size == 0 && positionLlh(91, 0).size == 0 && positionLlh(NAN, 1).size == 0);
  printf("ubx assist: ok\n");
  return 0;
}
